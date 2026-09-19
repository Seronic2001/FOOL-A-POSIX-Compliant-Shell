#include <algorithm>
#include <cctype>

#include "parser.h"

namespace {

// Returns true if the token is an operator token (unquoted <, >, >>, 2>,
// 2>>, 2>&, |, ;, &).
bool is_operator_token(const Token& tok) {
  if (tok.quoted) return false;
  return tok.text == ";" || tok.text == "|" || tok.text == "&" ||
         tok.text == "<" || tok.text == ">" || tok.text == ">>" ||
         tok.text == "2>" || tok.text == "2>>" || tok.text == "2>&";
}

// Parses tokens [start, end) as one command (argv + redirections).
void parse_command(const std::vector<Token>& tokens, size_t start, size_t end,
                   bool background, ParseResult& result) {
  Command command;
  command.background = background;

  for (size_t i = start; i < end; ++i) {
    const Token& tok = tokens[i];

    if (!is_operator_token(tok)) {
      command.args.push_back(tok.text);
      command.arg_is_globbed.push_back(!tok.quoted && tok.has_metachar);
      continue;
    }

    if (tok.text == "2>&") {
      if (i + 1 < end && !tokens[i + 1].quoted && tokens[i + 1].text == "1") {
        command.stderr_to_stdout = true;
        ++i;
        continue;
      }
      result.error = "syntax error: `2>&' must be immediately followed by `1'";
      return;
    }

    // All other operators require a following filename token.
    if (i + 1 >= end || is_operator_token(tokens[i + 1])) {
      result.error = "syntax error near unexpected token `" + tok.text + "'";
      return;
    }
    const std::string& filename = tokens[i + 1].text;

    if (tok.text == "<") {
      command.stdin_file = filename;
    } else if (tok.text == ">") {
      command.stdout_file = filename;
      command.append_out = false;
    } else if (tok.text == ">>") {
      command.stdout_file = filename;
      command.append_out = true;
    } else if (tok.text == "2>") {
      command.stderr_file = filename;
      command.append_err = false;
    } else if (tok.text == "2>>") {
      command.stderr_file = filename;
      command.append_err = true;
    } else {
      result.error = "syntax error near unexpected token `" + tok.text + "'";
      return;
    }
    ++i;  // Skip the filename token.
  }

  result.commands.push_back(command);
}

// Parses tokens [start, end) as a pipeline: commands separated by unquoted
// '|'. Empty segments are syntax errors ("| a", "a |", "a || b").
void parse_pipeline(const std::vector<Token>& tokens, size_t start, size_t end,
                    bool background, ParseResult& result) {
  size_t cmd_start = start;
  for (size_t i = start; i <= end; ++i) {
    bool is_end = (i == end);
    bool is_pipe = !is_end && tokens[i].text == "|" && !tokens[i].quoted;
    if (!is_end && !is_pipe) continue;

    if (i == cmd_start) {
      result.error = "syntax error near unexpected token `|'";
      return;
    }
    parse_command(tokens, cmd_start, i, background, result);
    if (!result.ok()) return;
    cmd_start = i + 1;
  }
}

// Parses tokens [start, end) as a ';'-separated group, splitting on
// unquoted '&' into background/foreground pipeline segments.
void parse_group(const std::vector<Token>& tokens, size_t start, size_t end,
                 ParseResult& result) {
  size_t seg_start = start;
  for (size_t i = start; i <= end; ++i) {
    bool is_end = (i == end);
    bool is_amp = !is_end && tokens[i].text == "&" && !tokens[i].quoted;
    if (!is_end && !is_amp) continue;

    // '&' with an empty segment before it: leading '&', "&&", or "a & &".
    if (is_amp && i == seg_start) {
      result.error = "syntax error near unexpected token `&'";
      return;
    }

    if (seg_start < i) {
      // A segment terminated by '&' runs in the background; the final
      // segment (terminated by end-of-group) runs in the foreground.
      parse_pipeline(tokens, seg_start, i, is_amp, result);
      if (!result.ok()) return;
    }
    seg_start = i + 1;
  }
}

// Character-by-character tokenizer. Handles quotes, splits unquoted
// metacharacters into standalone operator tokens (so `ls>/dev/null` works),
// tracks quoting/globbing metadata per word, and supports backslash escapes.
// Returns false on an unterminated quote.
bool tokenize_line(const std::string& input, std::vector<Token>& tokens) {
  std::string current;
  bool current_quoted = false;
  bool current_meta = false;
  char quote = 0;

  auto flush = [&]() {
    if (current.empty() && !current_quoted) return;
    tokens.emplace_back(current, current_quoted, current_meta);
    current.clear();
    current_quoted = false;
    current_meta = false;
  };

  for (size_t i = 0; i < input.size(); ++i) {
    char c = input[i];

    if (quote != 0) {  // Inside '...' or "..."
      if (c == quote) {
        quote = 0;
        current_quoted = true;  // The word contains quoted characters.
      } else {
        current += c;
        current_quoted = true;
      }
      continue;
    }

    if (c == '\'' || c == '"') {
      quote = c;
      current_quoted = true;
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(c))) {
      flush();
      continue;
    }

    // Backslash escape: the next character joins the word literally and
    // suppresses globbing (a backslashed '*' is literal).
    if (c == '\\' && i + 1 < input.size()) {
      char next = input[++i];
      if (next == '*' || next == '?') current_quoted = true;
      current += next;
      continue;
    }

    if (c == '<') {
      flush();
      tokens.emplace_back("<", false, false);
      continue;
    }

    if (c == '>') {
      const bool next_gt = (i + 1 < input.size()) && input[i + 1] == '>';
      const bool next_amp = (i + 1 < input.size()) && input[i + 1] == '&';
      // A run of unquoted digits immediately before the operator is an fd
      // number ("2>" redirects stderr), matching POSIX behavior.
      const bool digits_before =
          !current.empty() && !current_quoted &&
          std::all_of(current.begin(), current.end(), [](char d) {
            return std::isdigit(static_cast<unsigned char>(d)) != 0;
          });
      std::string digits;
      if (digits_before) {
        digits = current;
        current.clear();
        current_quoted = false;
        current_meta = false;
      } else {
        flush();
      }

      if (next_gt) {
        ++i;  // Consume the second '>'.
        tokens.emplace_back(digits.empty() ? ">>" : digits + ">>", false,
                            false);
      } else if (next_amp) {
        if (digits.empty()) {
          // Bare '>&' is not supported; emit '>' and let the '&' surface a
          // syntax error downstream.
          tokens.emplace_back(">", false, false);
        } else {
          ++i;  // Consume the '&'.
          tokens.emplace_back(digits + ">&", false, false);
        }
      } else {
        tokens.emplace_back(digits.empty() ? ">" : digits + ">", false, false);
      }
      continue;
    }

    if (c == ';' || c == '|' || c == '&') {
      flush();
      tokens.emplace_back(std::string(1, c), false, false);
      continue;
    }

    if (c == '*' || c == '?') current_meta = true;
    current += c;
  }

  if (quote != 0) return false;  // Unterminated quote.
  flush();
  return true;
}

}  // namespace

ParseResult parse_input(const std::string& input_line) {
  ParseResult result;
  std::vector<Token> tokens;

  if (!tokenize_line(input_line, tokens)) {
    result.error = "unexpected EOF while looking for matching quote";
    return result;
  }

  // Split into ';'-separated groups. Each group is followed by an explicit
  // end-of-group sentinel so the executor knows the group boundaries; without
  // them the executor would run "a; b" as the pipeline "a | b".
  size_t group_start = 0;
  for (size_t i = 0; i <= tokens.size(); ++i) {
    const bool is_end = (i == tokens.size());
    const bool is_semi = !is_end && tokens[i].text == ";" && !tokens[i].quoted;
    if (is_end || is_semi) {
      // Only non-empty ranges form a group; a trailing ';' (or ';;') must
      // not create a phantom empty group.
      if (group_start < i) {
        parse_group(tokens, group_start, i, result);
        if (!result.ok()) return result;
        Command group_end;
        group_end.is_group_end = true;
        result.commands.push_back(group_end);
      }
      group_start = i + 1;
    }
  }
  return result;
}

std::vector<Command> parse_input_or_empty(const std::string& commands) {
  ParseResult result = parse_input(commands);
  if (!result.ok()) return {};
  return result.commands;
}
