// Unit tests for the parser: tokenization, quoting, operators,
// semicolon-group splitting, redirections, and error reporting.

#include <string>
#include <vector>

#include "parser.h"
#include "test_framework.h"

// Count non-sentinel commands in a parse result.
static std::vector<const Command*> real_commands(const ParseResult& r) {
  std::vector<const Command*> out;
  for (const Command& c : r.commands) {
    if (!c.is_group_end) out.push_back(&c);
  }
  return out;
}

// Number of ';'-separated groups (== number of sentinels).
static size_t group_count(const ParseResult& r) {
  size_t n = 0;
  for (const Command& c : r.commands) {
    if (c.is_group_end) ++n;
  }
  return n;
}

TEST(parser, simple_command) {
  ParseResult r = parse_input("ls -l /tmp");
  CHECK(r.ok());
  CHECK_EQ(group_count(r), 1);
  ASSERT_EQ(real_commands(r).size(), 1);
  const Command* c = real_commands(r)[0];
  CHECK_STR_EQ(c->args[0], "ls");
  CHECK_STR_EQ(c->args[1], "-l");
  CHECK_STR_EQ(c->args[2], "/tmp");
}

TEST(parser, semicolon_groups_get_separate_sentinels) {
  // Regression: previously only ONE sentinel was appended at end of input,
  // so "echo one; echo two" executed as the pipeline "echo one | echo two".
  ParseResult r = parse_input("echo one; echo two");
  CHECK(r.ok());
  CHECK_EQ(group_count(r), 2);
  ASSERT_EQ(real_commands(r).size(), 2);
  CHECK_STR_EQ(real_commands(r)[0]->args[0], "echo");
  CHECK_STR_EQ(real_commands(r)[0]->args[1], "one");
  CHECK_STR_EQ(real_commands(r)[1]->args[0], "echo");
  CHECK_STR_EQ(real_commands(r)[1]->args[1], "two");
}

TEST(parser, three_groups) {
  ParseResult r = parse_input("a; b; c");
  CHECK(r.ok());
  CHECK_EQ(group_count(r), 3);
}

TEST(parser, trailing_semicolon) {
  ParseResult r = parse_input("echo hi;");
  CHECK(r.ok());
  CHECK_EQ(group_count(r), 1);
  CHECK_EQ(real_commands(r).size(), 1);
}

TEST(parser, pipeline_two_stages) {
  ParseResult r = parse_input("ls | grep foo");
  CHECK(r.ok());
  CHECK_EQ(group_count(r), 1);
  ASSERT_EQ(real_commands(r).size(), 2);
  CHECK_STR_EQ(real_commands(r)[0]->args[0], "ls");
  CHECK_STR_EQ(real_commands(r)[1]->args[0], "grep");
}

TEST(parser, quoted_pipe_is_not_operator) {
  ParseResult r = parse_input("echo \"a|b\"");
  CHECK(r.ok());
  ASSERT_EQ(real_commands(r).size(), 1);
  CHECK_STR_EQ(real_commands(r)[0]->args[1], "a|b");
}

TEST(parser, quoted_semicolon_is_not_separator) {
  ParseResult r = parse_input("echo \"a;b\"");
  CHECK(r.ok());
  CHECK_EQ(group_count(r), 1);
  CHECK_STR_EQ(real_commands(r)[0]->args[1], "a;b");
}

TEST(parser, embedded_operator_chars_in_quotes) {
  ParseResult r = parse_input("echo \"x > y\" 'a & b'");
  CHECK(r.ok());
  CHECK_EQ(real_commands(r).size(), 1);
  CHECK_STR_EQ(real_commands(r)[0]->args[1], "x > y");
  CHECK_STR_EQ(real_commands(r)[0]->args[2], "a & b");
}

TEST(parser, background_flag) {
  ParseResult r = parse_input("sleep 10 &");
  CHECK(r.ok());
  ASSERT_EQ(real_commands(r).size(), 1);
  CHECK((real_commands(r)[0]->background));
}

TEST(parser, foreground_has_no_background_flag) {
  ParseResult r = parse_input("sleep 10");
  CHECK(r.ok());
  CHECK(!(real_commands(r)[0]->background));
}

TEST(parser, background_and_foreground_mix) {
  ParseResult r = parse_input("a & b");
  CHECK(r.ok());
  CHECK_EQ(real_commands(r).size(), 2);
  CHECK((real_commands(r)[0]->background));
  CHECK(!(real_commands(r)[1]->background));
}

TEST(parser, output_redirection) {
  ParseResult r = parse_input("echo hi > out.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_STR_EQ(c->stdout_file, "out.txt");
  CHECK(!c->append_out);
  // The filename must not remain an argument.
  CHECK_EQ(c->args.size(), 2);
}

TEST(parser, append_redirection) {
  ParseResult r = parse_input("echo hi >> out.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_STR_EQ(c->stdout_file, "out.txt");
  CHECK((c->append_out));
}

TEST(parser, stderr_redirection) {
  ParseResult r = parse_input("ls nope 2> err.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_STR_EQ(c->stderr_file, "err.txt");
  CHECK(!c->append_err);
}

TEST(parser, stderr_append) {
  ParseResult r = parse_input("ls nope 2>> err.txt");
  CHECK(r.ok());
  CHECK_STR_EQ(real_commands(r)[0]->stderr_file, "err.txt");
  CHECK((real_commands(r)[0]->append_err));
}

TEST(parser, stderr_to_stdout) {
  ParseResult r = parse_input("ls nope 2>&1");
  CHECK(r.ok());
  CHECK((real_commands(r)[0]->stderr_to_stdout));
}

TEST(parser, fd_number_stays_arg_when_quoted) {
  // A quoted "2" before > must NOT become an fd number.
  ParseResult r = parse_input("echo \"2\" > f");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK(c->stderr_file.empty());
  CHECK_STR_EQ(c->stdout_file, "f");
  CHECK_EQ(c->args.size(), 2);  // echo, 2
}

TEST(parser, input_redirection) {
  ParseResult r = parse_input("cat < in.txt");
  CHECK(r.ok());
  CHECK_STR_EQ(real_commands(r)[0]->stdin_file, "in.txt");
}

TEST(parser, glob_metadata_tracked) {
  ParseResult r = parse_input("ls *.txt plain.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  ASSERT_EQ(c->args.size(), 3);
  CHECK((c->arg_is_globbed[1]));   // *.txt unquoted -> globbable
  CHECK(!(c->arg_is_globbed[2]));  // plain.txt has no metachar
}

TEST(parser, quoted_glob_not_expanded) {
  ParseResult r = parse_input("ls \"*.txt\"");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_STR_EQ(c->args[1], "*.txt");
  CHECK(!(c->arg_is_globbed[1]));
}

TEST(parser, backslash_escaped_star_is_literal) {
  ParseResult r = parse_input("ls \\*.txt");
  CHECK(r.ok());
  CHECK_STR_EQ(real_commands(r)[0]->args[1], "*.txt");
  CHECK(!(real_commands(r)[0]->arg_is_globbed[1]));
}

TEST(parser, error_leading_pipe) {
  ParseResult r = parse_input("| ls");
  CHECK(!r.ok());
  CHECK(!r.error.empty());
}

TEST(parser, error_trailing_pipe) {
  ParseResult r = parse_input("ls |");
  CHECK(!r.ok());
}

TEST(parser, error_double_pipe) {
  ParseResult r = parse_input("a || b");
  CHECK(!r.ok());
}

TEST(parser, error_leading_amp) {
  ParseResult r = parse_input("& ls");
  CHECK(!r.ok());
}

TEST(parser, error_double_amp) {
  ParseResult r = parse_input("a && b");
  CHECK(!r.ok());
}

TEST(parser, error_dangling_redirect) {
  ParseResult r = parse_input("echo >");
  CHECK(!r.ok());
}

TEST(parser, error_redirect_followed_by_operator) {
  ParseResult r = parse_input("echo > |");
  CHECK(!r.ok());
}

TEST(parser, empty_and_whitespace_input) {
  ParseResult r = parse_input("   ");
  CHECK(r.ok());
  CHECK_EQ(real_commands(r).size(), 0);
}

TEST(parser, adjacent_redirect_chars) {
  // "echo hi>f" must parse identically to "echo hi > f".
  ParseResult r = parse_input("echo hi>f");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_STR_EQ(c->stdout_file, "f");
  CHECK_EQ(c->args.size(), 2);
}

TEST(parser, pipeline_with_redirections) {
  ParseResult r = parse_input("cat < a | grep x > b");
  CHECK(r.ok());
  ASSERT_EQ(real_commands(r).size(), 2);
  CHECK_STR_EQ(real_commands(r)[0]->stdin_file, "a");
  CHECK_STR_EQ(real_commands(r)[1]->stdout_file, "b");
}

// Need main via helper macro guard; each test binary defines its own main.
int main() { return testfw::run_all_tests(); }
