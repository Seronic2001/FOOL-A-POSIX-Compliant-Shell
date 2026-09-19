#include "history.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The in-memory history data structure.
static std::vector<std::string> command_history;
const int MAX_HISTORY_SIZE = 1000;

// Sequence number of the first entry currently in memory; advances when
// entries are trimmed so displayed numbering stays monotonic (like bash).
static long long first_sequence_number = 1;
static const char* HISTORY_FILE_NAME = ".my_shell_history";

// Resolve the history file path. It lives in $HOME so it does not depend on
// the shell's current directory (which changes as the user runs `cd`).
// Falls back to /tmp/.my_shell_history.<uid> if HOME is unset.
static std::string history_file_path() {
  const char* home = getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    return std::string(home) + "/" + HISTORY_FILE_NAME;
  }
  return std::string("/tmp/") + HISTORY_FILE_NAME + "." +
         std::to_string((unsigned long)getuid());
}

void initialize_history() {
  FILE* history_file = fopen(history_file_path().c_str(), "r");
  if (history_file == nullptr) {
    return;  // First run: no history yet.
  }

  std::string line;
  int c;
  while ((c = fgetc(history_file)) != EOF) {
    if (c == '\n') {
      if (!line.empty()) command_history.push_back(line);
      line.clear();
    } else {
      line += (char)c;
    }
  }
  // Final line without trailing newline still counts.
  if (!line.empty()) command_history.push_back(line);
  fclose(history_file);

  // Respect the cap when loading, keeping the most recent entries.
  if (command_history.size() > (size_t)MAX_HISTORY_SIZE) {
    command_history.erase(command_history.begin(),
                          command_history.end() - MAX_HISTORY_SIZE);
  }
}

void save_history_on_exit() {
  FILE* history_file = fopen(history_file_path().c_str(), "w");
  if (history_file == nullptr) {
    return;
  }
  for (const auto& command : command_history) {
    fprintf(history_file, "%s\n", command.c_str());
  }
  fclose(history_file);
}

void add_to_history(const std::string& command) {
  if (command.empty()) return;

  // Avoid adding duplicate consecutive commands.
  if (!command_history.empty() && command_history.back() == command) {
    return;
  }

  command_history.push_back(command);

  // Enforce the maximum size limit. The first sequence number advances with
  // every dropped entry so displayed numbering stays monotonic (like bash).
  while (command_history.size() > (size_t)MAX_HISTORY_SIZE) {
    command_history.erase(command_history.begin());
    ++first_sequence_number;
  }
}

long long history_first_sequence_number() {
  return first_sequence_number;
}

size_t history_size() {
  return command_history.size();
}

int execute_history(const std::vector<std::string>& args, std::ostream& out) {
  long num_to_show = 10;  // Default number of commands to show.

  if (args.size() > 2) {
    std::cerr << "history: too many arguments" << std::endl;
    return 1;
  }
  if (args.size() == 2) {
    try {
      size_t pos = 0;
      long value = std::stol(args[1], &pos);
      if (pos != args[1].size() || value < 0) {
        throw std::invalid_argument("history");
      }
      num_to_show = value;
    } catch (const std::exception&) {
      std::cerr << "history: numeric argument required" << std::endl;
      return 2;
    }
  }

  long start_index = (long)command_history.size() - num_to_show;
  if (start_index < 0) start_index = 0;

  for (size_t i = (size_t)start_index; i < command_history.size(); ++i) {
    // Sequence numbers are stable and monotonic, like bash's history.
    out << "  " << (first_sequence_number + (long long)i) << "\t"
        << command_history[i] << std::endl;
  }
  return 0;
}

const std::vector<std::string>& get_history() {
  return command_history;
}
