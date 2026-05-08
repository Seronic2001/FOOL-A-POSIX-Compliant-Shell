#include <cstring>
#include <iostream>

#include "history.h"

// The history storing data structure
static std::vector<std::string> command_history;
const int MAX_HISTORY_SIZE = 20;
const char* HISTORY_FILE = ".my_shell_history";

void initialize_history() {
  FILE* history_file = fopen(HISTORY_FILE, "r");
  if (history_file != nullptr) {
    char line_buffer[4096];
    // Read from the file line by line until fgets returns NULL (end of file).
    while (fgets(line_buffer, sizeof(line_buffer), history_file) != nullptr) {
      // fgets includes the trailing newline character ('\n'), so we must
      // remove it to match the behavior of std::getline.
      line_buffer[strcspn(line_buffer, "\n")] = 0;

      // Add the clean line (now a C-style string) to our command history
      // vector.
      command_history.push_back(line_buffer);
    }
    fclose(history_file);
  }
}

void save_history_on_exit() {
  FILE* history_file = fopen(HISTORY_FILE, "w");
  if (history_file != nullptr) {
    for (const auto& command : command_history) {
      // Write each command string to the file, followed by a newline character.
      fprintf(history_file, "%s\n", command.c_str());
    }
    fclose(history_file);
  }
}

void add_to_history(const std::string& command) {
  if (command.empty())
    return;

  // Avoid adding duplicate consecutive commands
  if (!command_history.empty() && command_history.back() == command) {
    return;
  }

  command_history.push_back(command);

  // Enforce the maximum size limit
  if (command_history.size() > MAX_HISTORY_SIZE) {
    command_history.erase(command_history.begin());
  }
}

void execute_history(const std::vector<std::string>& args, std::ostream& out) {
  int num_to_show = 10;  // Default number of commands to show

  if (args.size() > 2) {
    out << "history: too many arguments" << std::endl;
    return;
  }
  if (args.size() == 2) {
    try {
      num_to_show = std::stoi(args[1]);
    } catch (const std::exception& e) {
      out << "history: numeric argument required" << std::endl;
      return;
    }
  }

  int start_index = std::max(0, (int)command_history.size() - num_to_show);

  for (size_t i = start_index; i < command_history.size(); ++i) {
    out << "  " << i + 1 << "\t" << command_history[i] << std::endl;
  }
}

const std::vector<std::string>& get_history() {
  return command_history;
}