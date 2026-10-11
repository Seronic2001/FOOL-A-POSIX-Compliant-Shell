#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "parser.h"

namespace {

bool is_glob_meta(char c) { return c == '*' || c == '?' || c == '['; }

bool is_all_digits(const std::string& s) {
  return !s.empty() && s.size() <= 9 &&
         std::all_of(s.begin(), s.end(), [](char d) {
           return std::isdigit(static_cast<unsigned char>(d)) != 0;
         });
}

// A lexical token: either a word or an operator (; | & < > >> >&).
struct Token {
  bool is_word = false;
  Word word;
  std::string op;
  int fd = -1;  // Explicit descriptor prefix on a redirection ("2>"), or -1.
};

// Accumulates one word, tracking which characters were quoted so that only
// unquoted metacharacters glob (`"my dir"/*.c` still expands the `*`).
struct WordBuilder {
  Word word;
  bool active = false;      // A word is in progress (possibly empty: "").
  bool any_quoted = false;  // Some part came from quotes or a backslash.
  bool has_meta = false;    // An unquoted glob metacharacter is present.

  void add_literal(char c) {
    word.text += c;
    word.pattern += glob_escape(std::string(1, c));
    active = any_quoted = true;
  }

  void add_unquoted(char c) {
    if (!active && c == '~') word.tilde = true;
    if (is_glob_meta(c)) has_meta = true;
    word.text += c;
    word.pattern += c;
    active = true;
  }

  void reset() { *this = WordBuilder(); }
};

// Character-by-character tokenizer. Handles quotes and backslash escapes,
// and splits unquoted metacharacters into standalone operator tokens (so
// `ls>/dev/null` works). Returns false on an unterminated quote.
bool tokenize_line(const std::string& input, std::vector<Token>& tokens) {
  WordBuilder current;
  char quote = 0;

  auto flush = [&]() {
    if (!current.active) return;
    Token tok;
    tok.is_word = true;
    tok.word = std::move(current.word);
    if (!current.has_meta) tok.word.pattern.clear();
    tokens.push_back(std::move(tok));
    current.reset();
  };

  auto push_op = [&](const std::string& op, int fd = -1) {
    Token tok;
    tok.op = op;
    tok.fd = fd;
    tokens.push_back(std::move(tok));
  };

  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    const char next = (i + 1 < input.size()) ? input[i + 1] : '\0';

    if (quote != 0) {  // Inside '...' or "..."
      if (c == quote) {
        quote = 0;
      } else if (quote == '"' && c == '\\' &&
                 (next == '"' || next == '\\' || next == '$' || next == '`')) {
        current.add_literal(next);
        ++i;
      } else {
        current.add_literal(c);
      }
      continue;
    }

    if (c == '\'' || c == '"') {
      quote = c;
      current.active = current.any_quoted = true;
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(c))) {
      flush();
      continue;
    }

    // Backslash escape: the next character joins the word literally, so an
    // escaped operator or glob character loses its special meaning.
    if (c == '\\' && i + 1 < input.size()) {
      current.add_literal(next);
      ++i;
      continue;
    }

    if (c == '<' || c == '>') {
      // A run of unquoted digits immediately before the operator is an fd
      // number ("2>" redirects stderr), matching POSIX behavior.
      int fd = -1;
      if (current.active && !current.any_quoted &&
          is_all_digits(current.word.text)) {
        fd = std::atoi(current.word.text.c_str());
        current.reset();
      } else {
        flush();
      }

      if (c == '<') {
        push_op("<", fd);
      } else if (next == '>') {
        ++i;
        push_op(">>", fd);
      } else if (next == '&') {
        ++i;
        push_op(">&", fd);
      } else {
        push_op(">", fd);
      }
      continue;
    }

    if (c == ';' || c == '|' || c == '&') {
      flush();
      push_op(std::string(1, c));
      continue;
    }

    current.add_unquoted(c);
  }

  if (quote != 0) return false;  // Unterminated quote.
  flush();
  return true;
}

bool is_redirect_op(const std::string& op) {
  return op == "<" || op == ">" || op == ">>" || op == ">&";
}

}  // namespace

std::string glob_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (is_glob_meta(c) || c == ']' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

ParseResult parse_input(const std::string& input_line) {
  ParseResult result;
  std::vector<Token> tokens;

  if (!tokenize_line(input_line, tokens)) {
    result.error = "unexpected EOF while looking for matching quote";
    result.incomplete = true;
    return result;
  }

  auto unexpected = [&](const std::string& token) {
    result.pipelines.clear();
    result.error = "syntax error near unexpected token `" + token + "'";
    return result;
  };

  Pipeline pipeline;
  Command command;

  for (size_t i = 0; i <= tokens.size(); ++i) {
    const bool is_end = (i == tokens.size());

    if (!is_end && tokens[i].is_word) {
      command.words.push_back(tokens[i].word);
      continue;
    }

    if (!is_end && is_redirect_op(tokens[i].op)) {
      const Token& tok = tokens[i];
      // Every redirection needs a following word (file name or fd number).
      if (i + 1 >= tokens.size() || !tokens[i + 1].is_word) {
        return unexpected(tok.op);
      }
      const Word& target = tokens[++i].word;

      Redirect redirect;
      if (tok.op == "<") {
        redirect.kind = Redirect::kIn;
        redirect.fd = tok.fd < 0 ? 0 : tok.fd;
        redirect.target = target;
      } else if (tok.op == ">&") {
        if (!is_all_digits(target.text)) {
          result.error =
              "syntax error: `>&' must be followed by a file descriptor";
          return result;
        }
        redirect.kind = Redirect::kDup;
        redirect.fd = tok.fd < 0 ? 1 : tok.fd;
        redirect.dup_fd = std::atoi(target.text.c_str());
      } else {
        redirect.kind = (tok.op == ">>") ? Redirect::kAppend : Redirect::kOut;
        redirect.fd = tok.fd < 0 ? 1 : tok.fd;
        redirect.target = target;
      }
      command.redirects.push_back(std::move(redirect));
      continue;
    }

    // A separator: '|', ';', '&', or end of input.
    const std::string op = is_end ? "" : tokens[i].op;

    if (op == "|") {
      // Empty pipeline stage: "| a" or "a || b".
      if (command.empty()) return unexpected("|");
      pipeline.commands.push_back(std::move(command));
      command = Command();
      continue;
    }

    if (command.empty()) {
      if (!pipeline.commands.empty()) return unexpected("|");  // "a |"
      if (op == "&") return unexpected("&");  // "& a", "a && b", "a & &"
      continue;  // Empty statement: a trailing ';' or ';;' is harmless.
    }

    pipeline.commands.push_back(std::move(command));
    command = Command();
    pipeline.background = (op == "&");
    result.pipelines.push_back(std::move(pipeline));
    pipeline = Pipeline();
  }
  return result;
}
