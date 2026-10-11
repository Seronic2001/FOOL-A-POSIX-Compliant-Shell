// Unit tests for the parser: tokenization, quoting, operators,
// semicolon-group splitting, redirections, and error reporting.

#include <string>
#include <vector>

#include "parser.h"
#include "test_framework.h"

// Every command in the parse result, across all pipelines, in order.
static std::vector<const Command*> real_commands(const ParseResult& r) {
  std::vector<const Command*> out;
  for (const Pipeline& p : r.pipelines) {
    for (const Command& c : p.commands) out.push_back(&c);
  }
  return out;
}

// Number of pipelines (';'- or '&'-separated statements).
static size_t group_count(const ParseResult& r) { return r.pipelines.size(); }

// The command's single redirection (the tests below use one per command).
static const Redirect& redirect_of(const Command* c) {
  static const Redirect none;
  return c->redirects.size() == 1 ? c->redirects[0] : none;
}

TEST(parser, simple_command) {
  ParseResult r = parse_input("ls -l /tmp");
  CHECK(r.ok());
  CHECK_EQ(group_count(r), 1);
  ASSERT_EQ(real_commands(r).size(), 1);
  const Command* c = real_commands(r)[0];
  CHECK_STR_EQ(c->words[0].text, "ls");
  CHECK_STR_EQ(c->words[1].text, "-l");
  CHECK_STR_EQ(c->words[2].text, "/tmp");
}

TEST(parser, semicolon_separates_pipelines) {
  // Regression: "echo one; echo two" once executed as the pipeline
  // "echo one | echo two".
  ParseResult r = parse_input("echo one; echo two");
  CHECK(r.ok());
  CHECK_EQ(group_count(r), 2);
  ASSERT_EQ(real_commands(r).size(), 2);
  CHECK_STR_EQ(real_commands(r)[0]->words[0].text, "echo");
  CHECK_STR_EQ(real_commands(r)[0]->words[1].text, "one");
  CHECK_STR_EQ(real_commands(r)[1]->words[0].text, "echo");
  CHECK_STR_EQ(real_commands(r)[1]->words[1].text, "two");
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
  CHECK_STR_EQ(real_commands(r)[0]->words[0].text, "ls");
  CHECK_STR_EQ(real_commands(r)[1]->words[0].text, "grep");
}

TEST(parser, quoted_pipe_is_not_operator) {
  ParseResult r = parse_input("echo \"a|b\"");
  CHECK(r.ok());
  ASSERT_EQ(real_commands(r).size(), 1);
  CHECK_STR_EQ(real_commands(r)[0]->words[1].text, "a|b");
}

TEST(parser, quoted_semicolon_is_not_separator) {
  ParseResult r = parse_input("echo \"a;b\"");
  CHECK(r.ok());
  CHECK_EQ(group_count(r), 1);
  CHECK_STR_EQ(real_commands(r)[0]->words[1].text, "a;b");
}

TEST(parser, embedded_operator_chars_in_quotes) {
  ParseResult r = parse_input("echo \"x > y\" 'a & b'");
  CHECK(r.ok());
  CHECK_EQ(real_commands(r).size(), 1);
  CHECK_STR_EQ(real_commands(r)[0]->words[1].text, "x > y");
  CHECK_STR_EQ(real_commands(r)[0]->words[2].text, "a & b");
}

TEST(parser, background_flag) {
  ParseResult r = parse_input("sleep 10 &");
  CHECK(r.ok());
  ASSERT_EQ(real_commands(r).size(), 1);
  CHECK((r.pipelines[0].background));
}

TEST(parser, foreground_has_no_background_flag) {
  ParseResult r = parse_input("sleep 10");
  CHECK(r.ok());
  CHECK(!(r.pipelines[0].background));
}

TEST(parser, background_and_foreground_mix) {
  ParseResult r = parse_input("a & b");
  CHECK(r.ok());
  // Regression: "a & b" must be two separate pipelines, not "a | b".
  ASSERT_EQ(r.pipelines.size(), 2);
  CHECK_EQ(r.pipelines[0].commands.size(), 1);
  CHECK((r.pipelines[0].background));
  CHECK(!(r.pipelines[1].background));
}

TEST(parser, background_pipeline_is_one_job) {
  ParseResult r = parse_input("a | b &");
  CHECK(r.ok());
  ASSERT_EQ(r.pipelines.size(), 1);
  CHECK_EQ(r.pipelines[0].commands.size(), 2);
  CHECK((r.pipelines[0].background));
}

TEST(parser, output_redirection) {
  ParseResult r = parse_input("echo hi > out.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_EQ(redirect_of(c).kind, Redirect::kOut);
  CHECK_EQ(redirect_of(c).fd, 1);
  CHECK_STR_EQ(redirect_of(c).target.text, "out.txt");
  // The filename must not remain an argument.
  CHECK_EQ(c->words.size(), 2);
}

TEST(parser, append_redirection) {
  ParseResult r = parse_input("echo hi >> out.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_EQ(redirect_of(c).kind, Redirect::kAppend);
  CHECK_STR_EQ(redirect_of(c).target.text, "out.txt");
}

TEST(parser, stderr_redirection) {
  ParseResult r = parse_input("ls nope 2> err.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_EQ(redirect_of(c).kind, Redirect::kOut);
  CHECK_EQ(redirect_of(c).fd, 2);
  CHECK_STR_EQ(redirect_of(c).target.text, "err.txt");
  CHECK_EQ(c->words.size(), 2);
}

TEST(parser, stderr_append) {
  ParseResult r = parse_input("ls nope 2>> err.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_EQ(redirect_of(c).kind, Redirect::kAppend);
  CHECK_EQ(redirect_of(c).fd, 2);
}

TEST(parser, stderr_to_stdout) {
  ParseResult r = parse_input("ls nope 2>&1");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_EQ(redirect_of(c).kind, Redirect::kDup);
  CHECK_EQ(redirect_of(c).fd, 2);
  CHECK_EQ(redirect_of(c).dup_fd, 1);
}

TEST(parser, redirections_keep_their_order) {
  // "> f 2>&1" and "2>&1 > f" mean different things, so order matters.
  ParseResult r = parse_input("cmd > f 2>&1");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  ASSERT_EQ(c->redirects.size(), 2);
  CHECK_EQ(c->redirects[0].kind, Redirect::kOut);
  CHECK_EQ(c->redirects[1].kind, Redirect::kDup);
}

TEST(parser, explicit_stdout_fd) {
  // Regression: "1>" was passed to the command as a literal argument.
  ParseResult r = parse_input("echo hi 1>out");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_EQ(c->words.size(), 2);
  CHECK_EQ(redirect_of(c).fd, 1);
  CHECK_STR_EQ(redirect_of(c).target.text, "out");
}

TEST(parser, stdout_to_stderr) {
  ParseResult r = parse_input("echo oops >&2");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_EQ(redirect_of(c).kind, Redirect::kDup);
  CHECK_EQ(redirect_of(c).fd, 1);
  CHECK_EQ(redirect_of(c).dup_fd, 2);
}

TEST(parser, error_dup_needs_fd_number) {
  ParseResult r = parse_input("echo 2>&file");
  CHECK(!r.ok());
}

TEST(parser, redirect_only_command) {
  ParseResult r = parse_input("> created");
  CHECK(r.ok());
  ASSERT_EQ(real_commands(r).size(), 1);
  CHECK_EQ(real_commands(r)[0]->words.size(), 0);
  CHECK_EQ(real_commands(r)[0]->redirects.size(), 1);
}

TEST(parser, fd_number_stays_arg_when_quoted) {
  // A quoted "2" before > must NOT become an fd number.
  ParseResult r = parse_input("echo \"2\" > f");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_EQ(redirect_of(c).fd, 1);
  CHECK_STR_EQ(redirect_of(c).target.text, "f");
  CHECK_EQ(c->words.size(), 2);  // echo, 2
}

TEST(parser, input_redirection) {
  ParseResult r = parse_input("cat < in.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_EQ(redirect_of(c).kind, Redirect::kIn);
  CHECK_EQ(redirect_of(c).fd, 0);
  CHECK_STR_EQ(redirect_of(c).target.text, "in.txt");
}

TEST(parser, glob_metadata_tracked) {
  ParseResult r = parse_input("ls *.txt plain.txt");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  ASSERT_EQ(c->words.size(), 3);
  CHECK_STR_EQ(c->words[1].pattern, "*.txt");  // unquoted -> globbable
  CHECK((c->words[2].pattern.empty()));        // no metachar
}

TEST(parser, quoted_glob_not_expanded) {
  ParseResult r = parse_input("ls \"*.txt\"");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK_STR_EQ(c->words[1].text, "*.txt");
  CHECK((c->words[1].pattern.empty()));
}

TEST(parser, backslash_escaped_star_is_literal) {
  ParseResult r = parse_input("ls \\*.txt");
  CHECK(r.ok());
  CHECK_STR_EQ(real_commands(r)[0]->words[1].text, "*.txt");
  CHECK((real_commands(r)[0]->words[1].pattern.empty()));
}

TEST(parser, partly_quoted_word_still_globs) {
  // Regression: any quote in a word used to disable globbing for all of
  // it. Only the quoted part is literal (escaped in the pattern).
  ParseResult r = parse_input("ls \"my*dir\"/*.c");
  CHECK(r.ok());
  const Word& w = real_commands(r)[0]->words[1];
  CHECK_STR_EQ(w.text, "my*dir/*.c");
  CHECK_STR_EQ(w.pattern, "my\\*dir/*.c");
}

TEST(parser, backslash_escaped_operators_are_literal) {
  // Regression: "\;" and "\|" were still treated as operators.
  ParseResult r = parse_input("echo \\; a \\| b \\> c \\&");
  CHECK(r.ok());
  ASSERT_EQ(r.pipelines.size(), 1);
  const Command* c = real_commands(r)[0];
  ASSERT_EQ(c->words.size(), 8);
  CHECK_STR_EQ(c->words[1].text, ";");
  CHECK_STR_EQ(c->words[3].text, "|");
  CHECK_STR_EQ(c->words[5].text, ">");
  CHECK_STR_EQ(c->words[7].text, "&");
  CHECK_EQ(c->redirects.size(), 0);
}

TEST(parser, escaped_quote_inside_double_quotes) {
  ParseResult r = parse_input("echo \"say \\\"hi\\\"\"");
  CHECK(r.ok());
  CHECK_STR_EQ(real_commands(r)[0]->words[1].text, "say \"hi\"");
}

TEST(parser, tilde_only_when_leading_and_unquoted) {
  ParseResult r = parse_input("ls ~ ~/x \"~\" a~");
  CHECK(r.ok());
  const Command* c = real_commands(r)[0];
  CHECK((c->words[1].tilde));
  CHECK((c->words[2].tilde));
  CHECK(!(c->words[3].tilde));
  CHECK(!(c->words[4].tilde));
}

TEST(parser, unterminated_quote_is_incomplete) {
  ParseResult r = parse_input("echo \"abc");
  CHECK(!r.ok());
  CHECK((r.incomplete));
  CHECK(!(parse_input("| ls").incomplete));
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
  CHECK_STR_EQ(redirect_of(c).target.text, "f");
  CHECK_EQ(c->words.size(), 2);
}

TEST(parser, pipeline_with_redirections) {
  ParseResult r = parse_input("cat < a | grep x > b");
  CHECK(r.ok());
  ASSERT_EQ(real_commands(r).size(), 2);
  CHECK_STR_EQ(redirect_of(real_commands(r)[0]).target.text, "a");
  CHECK_STR_EQ(redirect_of(real_commands(r)[1]).target.text, "b");
}

// Need main via helper macro guard; each test binary defines its own main.
int main() { return testfw::run_all_tests(); }
