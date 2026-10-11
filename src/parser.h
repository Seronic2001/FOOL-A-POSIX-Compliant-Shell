#pragma once

#include <string>
#include <vector>

// One shell word after quote removal.
struct Word {
  std::string text;     // Literal value.
  std::string pattern;  // glob(3) pattern with quoted characters escaped;
                        // empty when the word has no unquoted metacharacter.
  bool tilde = false;   // Starts with an unquoted '~'.
};

// One redirection, applied in the order written.
struct Redirect {
  enum Kind { kIn, kOut, kAppend, kDup };
  Kind kind = kOut;
  int fd = 1;       // The descriptor being redirected.
  Word target;      // File name (kIn / kOut / kAppend).
  int dup_fd = -1;  // Source descriptor for kDup (N>&M).
};

// One command in a pipeline.
struct Command {
  std::vector<Word> words;          // e.g., {"ls", "-l", "/home"}
  std::vector<Redirect> redirects;

  bool empty() const { return words.empty() && redirects.empty(); }
};

// Commands joined by '|', terminated by ';', '&' or end of line.
struct Pipeline {
  std::vector<Command> commands;
  bool background = false;  // Terminated by '&'.
};

// Result of parsing: either a list of pipelines or an error message.
struct ParseResult {
  std::vector<Pipeline> pipelines;
  std::string error;        // Empty if no error.
  bool incomplete = false;  // The error is an unterminated quote.
  bool ok() const { return error.empty(); }
};

// Tokenizes and parses an input line. Never throws.
ParseResult parse_input(const std::string& input_line);

// Backslash-escapes glob(3) metacharacters so `s` matches only itself.
std::string glob_escape(const std::string& s);
