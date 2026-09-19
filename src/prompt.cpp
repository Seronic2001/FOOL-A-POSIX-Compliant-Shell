#include <limits.h>
#include <pwd.h>     // For getpwuid
#include <unistd.h>  // For getcwd, gethostname, getuid

#include <iostream>
#include <string>

#include "colors.h"
#include "prompt.h"

// The directory the shell started in, treated as its "home" for the
// prompt's ~ substitution. Defined here (not main.cpp) so test binaries
// linking every object except main.o resolve it.
std::string shell_home_dir;

std::string generate_prompt_string(void) {
  // getpwuid can return NULL for UIDs not in the user database (containers,
  // NFS, LDAP hiccups). Never dereference blindly.
  const char* user = "unknown";
  struct passwd* pw = getpwuid(getuid());
  if (pw != nullptr && pw->pw_name != nullptr) {
    user = pw->pw_name;
  }

  char hostname_buffer[HOST_NAME_MAX];
  if (gethostname(hostname_buffer, sizeof(hostname_buffer)) != 0) {
    hostname_buffer[0] = '\0';
  } else {
    hostname_buffer[sizeof(hostname_buffer) - 1] = '\0';
  }

  // Getting current working directory
  char cwd_buffer[PATH_MAX];
  if (getcwd(cwd_buffer, sizeof(cwd_buffer)) == nullptr) {
    // The cwd can disappear if deleted from another shell. Print a stable
    // placeholder instead of a broken prompt.
    return BOLD_GREEN + std::string(user) + "@" + hostname_buffer + RESET +
           ":" + BOLD_RED + "<deleted cwd>" + RESET + "$ ";
  }

  std::string cwd = cwd_buffer;

  // Replace the home directory prefix with '~'. Only when it is a real path
  // component boundary: /home/u must match /home/u and /home/u/foo but not
  // /home/user2.
  if (!shell_home_dir.empty() && shell_home_dir != "/" &&
      cwd.rfind(shell_home_dir, 0) == 0) {
    size_t len = shell_home_dir.length();
    if (cwd.length() == len || cwd[len] == '/') {
      cwd.replace(0, len, "~");
    }
  }

  // Determining the symbol ($ or #)
  std::string prompt_symbol = (getuid() == 0) ? "#" : "$";

  return BOLD_GREEN + std::string(user) + "@" + hostname_buffer + RESET + ":" +
         BOLD_BLUE + cwd + RESET + prompt_symbol + " ";
}

void display_prompt() {
  std::cout << generate_prompt_string();
}

size_t calculate_visible_length(const std::string& s) {
  size_t length = 0;
  bool in_escape_sequence = false;

  for (char c : s) {
    if (!in_escape_sequence) {
      if (c == '\033') {
        in_escape_sequence = true;
      } else {
        // Only count printable columns. UTF-8 continuation bytes (0x80-0xBF)
        // are part of the previous character's single column.
        if ((unsigned char)c < 0x80 || (unsigned char)c >= 0xC0) {
          length++;
        }
      }
    } else if (c == 'm') {
      // This parser only handles the SGR sequences used by colors.h.
      in_escape_sequence = false;
    }
  }
  return length;
}
