#include <limits.h>  // For HOST_NAME_MAX
#include <pwd.h>     // For getpwuid
#include <unistd.h>  // For getcwd, gethostname, getuid

#include <iostream>
#include <string>

#include "colors.h"
#include "prompt.h"

extern std::string shell_home_dir;

std::string generate_prompt_string(void) {
  const char* user = getpwuid(getuid())->pw_name;
  char hostname_buffer[HOST_NAME_MAX];
  if (gethostname(hostname_buffer, HOST_NAME_MAX) == 0) {
    std::string hostname = hostname_buffer;

    // Getting current working directory
    char cwd_buffer[PATH_MAX];
    if (getcwd(cwd_buffer, PATH_MAX) == nullptr) {
      perror("Error: getting current working directory");
    } else {
      std::string cwd = cwd_buffer;

      // Replace home directory with '~'
      // check if the current path starts with the home directory path
      if (cwd.rfind(shell_home_dir, 0) == 0) {
        // replace it with '~'
        cwd.replace(0, shell_home_dir.length(), "~");
      }

      // determining the symbol ($ or #)
      std::string prompt_symbol = (getuid() == 0) ? "#" : "$";

      // Build the string and return it
      return BOLD_GREEN + std::string(user) + "@" + hostname + RESET + ":" +
             BOLD_BLUE + cwd + RESET + prompt_symbol + " ";
    }
  } else {
    perror("Error: getting gethostname");
  }

  return "UNREACHABLE";
}

void display_prompt() {
  std::cout << generate_prompt_string();
}

size_t calculate_visible_length(const std::string& s) {
  size_t length = 0;
  bool in_escape_sequence = false;

  for (char c : s) {
    if (c == '\033') {  // Start of an escape sequence
      in_escape_sequence = true;
    }

    if (!in_escape_sequence) {
      length++;
    }

    if (in_escape_sequence && c == 'm') {  // End of the sequence
      in_escape_sequence = false;
    }
  }
  return length;
}