#pragma once

#include <string>
#include <vector>

// A lexical token with the metadata the parser needs to apply shell
// semantics correctly (quoting suppresses globbing and operator meaning).
struct Token {
  std::string text;
  bool quoted = false;  // Character came from inside '...' or "..."
  bool has_metachar =
      false;  // Unquoted '*' or '?' present -> candidate for globbing.

  Token() = default;
  Token(std::string t, bool q, bool meta)
      : text(std::move(t)), quoted(q), has_metachar(meta) {}
};

// One command in a pipeline.
struct Command {
  std::vector<std::string> args;      // e.g., {"ls", "-l", "/home"}
  std::vector<bool> arg_is_globbed;   // args[i] may be expanded by globbing
  bool background = false;            // Set to true if '&' was seen
  std::string stdin_file;             // For <
  std::string stdout_file;            // For > or >>
  std::string stderr_file;            // For 2> or 2>>
  bool stderr_to_stdout = false;      // For 2>&1
  bool append_out = false;   // To distinguish > (overwrite) from >> (append)
  bool append_err = false;   // To distinguish 2> (overwrite) from 2>> (append)
  bool is_group_end = false;  // Sentinel: end of a ';'-separated group.
};

// Result of parsing: either a list of commands or an error message.
struct ParseResult {
  std::vector<Command> commands;  // Group-end sentinel commands included.
  std::string error;              // Empty if no error.
  bool ok() const { return error.empty(); }
};

// Tokenizes and parses an input line into commands. Never throws.
ParseResult parse_input(const std::string& commands);

// Backwards-compatible convenience wrapper (discards the error message).
std::vector<Command> parse_input_or_empty(const std::string& commands);
