#pragma once

#include <string>
#include <vector>

struct Command {
  std::vector<std::string> args;  // e.g., {"ls", "-l", "/home"}
  bool background = false;        // Set to true if '&' is found
  std::string stdin_file;         // For <
  std::string stdout_file;        // For > or >>
  bool append_out = false;  // To distinguish > (overwrite) from >> (append)
};

std::vector<Command> parse_input(const std::string& commands);