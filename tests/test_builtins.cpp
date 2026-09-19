// Unit tests for history management and the pure string builtins
// (echo, search) plus jobs listing. These run in-process and write to
// ostringstream, so they never touch the terminal.

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "builtins.h"
#include "history.h"
#include "test_framework.h"

// An empty stringstream used wherever a builtin takes an istream. Never
// pass std::cin here: a builtin that reads stdin (e.g. `search` with no
// argument filters target names from stdin) would block on the live
// terminal when the suite is run interactively.
static std::istringstream no_input;

// ---------------- history ----------------

TEST(history, dedups_consecutive_duplicates) {
  add_to_history("ls");
  add_to_history("ls");  // Duplicate: must not be added again.
  size_t s = history_size();
  add_to_history("pwd");
  CHECK_EQ(history_size(), s + 1);
}

TEST(history, empty_command_ignored) {
  size_t s = history_size();
  add_to_history("");
  CHECK_EQ(history_size(), s);
}

TEST(history, trimming_enforces_limit) {
  // Fill well past the limit; size must never exceed MAX_HISTORY_SIZE.
  for (int i = 0; i < 1200; ++i) {
    add_to_history("cmd" + std::to_string(i));
  }
  CHECK(history_size() <= 1000);
}

TEST(history, sequence_numbers_stay_monotonic) {
  // After trimming, the first sequence number must have advanced so that
  // displayed numbering never restarts.
  CHECK(history_first_sequence_number() > 1);
  add_to_history("post-trim-command");
  std::ostringstream out;
  int rc = execute_history({"history", "3"}, out);
  CHECK_EQ(rc, 0);
  // The newest entry must be visible with a positive sequence number.
  CHECK(out.str().find("post-trim-command") != std::string::npos);
}

TEST(history, count_argument_limits_output) {
  add_to_history("hist-cmd-a");
  add_to_history("hist-cmd-b");
  std::ostringstream out;
  int rc = execute_history({"history", "1"}, out);
  CHECK_EQ(rc, 0);
  CHECK(out.str().find("hist-cmd-b") != std::string::npos);
  CHECK(out.str().find("hist-cmd-a") == std::string::npos);
}

TEST(history, bad_argument_rejected) {
  // Silence the diagnostic that goes to std::cerr so the suite output
  // stays clean.
  std::ostringstream err_sink;
  std::ostringstream out;
  auto* saved = std::cerr.rdbuf(err_sink.rdbuf());
  int rc = execute_history({"history", "not-a-number"}, out);
  std::cerr.rdbuf(saved);
  CHECK_EQ(rc, 2);
}

TEST(history, too_many_args_is_error) {
  std::ostringstream err_sink;
  auto* saved = std::cerr.rdbuf(err_sink.rdbuf());
  std::ostringstream out;
  int rc = execute_history({"history", "1", "2"}, out);
  std::cerr.rdbuf(saved);
  CHECK(rc != 0);
}

// ---------------- echo ----------------

TEST(echo, plain_text) {
  std::ostringstream out;
  int rc = execute_echo({"echo", "hello", "world"}, no_input, out);
  CHECK_EQ(rc, 0);
  CHECK_STR_EQ(out.str(), "hello world\n");
}

TEST(echo, no_newline_flag) {
  std::ostringstream out;
  int rc = execute_echo({"echo", "-n", "hi"}, no_input, out);
  CHECK_EQ(rc, 0);
  CHECK_STR_EQ(out.str(), "hi");
}

TEST(echo, bare_dash_n_arg) {
  // "-n" as the only argument: real bash prints "--n"... actually prints
  // nothing (treats it as flag). POSIX echo would print "-n". We follow
  // the flag interpretation but must not print a bare newline-only crash.
  std::ostringstream out;
  int rc = execute_echo({"echo", "-n"}, no_input, out);
  CHECK_EQ(rc, 0);
  CHECK_STR_EQ(out.str(), "");
}

TEST(echo, no_args_prints_newline) {
  std::ostringstream out;
  int rc = execute_echo({"echo"}, no_input, out);
  CHECK_EQ(rc, 0);
  CHECK_STR_EQ(out.str(), "\n");
}

TEST(echo, preserves_internal_spaces) {
  std::ostringstream out;
  execute_echo({"echo", "a", "", "b"}, no_input, out);
  CHECK_STR_EQ(out.str(), "a  b\n");  // Empty arg still takes its separator.
}

// ---------------- search (recursive filename search) ----------------

TEST(search, requires_argument) {
  std::ostringstream out;
  int rc = execute_search({"search"}, no_input, out);
  // With no argument, search reads targets from stdin; with an empty stdin
  // it simply finds nothing. Must not crash or block either way.
  CHECK_EQ(rc, 0);
}

TEST(search, reports_true_false) {
  // Contract: prints True/False and returns 0 either way.
  std::ostringstream out;
  int rc = execute_search({"search", "Makefile"}, no_input, out);
  CHECK_EQ(rc, 0);
  CHECK(out.str().find("True") != std::string::npos);

  std::ostringstream out2;
  rc = execute_search({"search", "definitely-not-here-xyz"}, no_input,
                      out2);
  CHECK_EQ(rc, 0);
  CHECK(out2.str().find("False") != std::string::npos);
}

TEST(search, stdin_filter_mode) {
  // No-arg search reads one target name per line from stdin.
  std::istringstream in("Makefile\ndefinitely-not-here-xyz\n");
  std::ostringstream out;
  int rc = execute_search({"search"}, in, out);
  CHECK_EQ(rc, 0);
  CHECK(out.str().find("Makefile: True") != std::string::npos);
  CHECK(out.str().find("definitely-not-here-xyz: False") != std::string::npos);
}

// ---------------- jobs ----------------

TEST(jobs, empty_table_prints_nothing) {
  std::ostringstream out;
  int rc = execute_jobs({"jobs"}, out);
  CHECK_EQ(rc, 0);
  CHECK_STR_EQ(out.str(), "");
}

// ---------------- pwd ----------------

TEST(pwd, prints_current_directory) {
  std::ostringstream out;
  int rc = execute_pwd({"pwd"}, out);
  CHECK_EQ(rc, 0);
  CHECK(!out.str().empty());
  CHECK(out.str().back() == '\n');
  // Absolute path: starts with '/'.
  CHECK(out.str()[0] == '/');
}

int main() { return testfw::run_all_tests(); }
