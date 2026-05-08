#include <dirent.h>
#include <sys/stat.h>  // For stat()

#include <atomic>
#include <set>  // Required for std::set to handle duplicate command names
#include <sstream>
#include <thread>

#include "autocomplete.h"

static std::set<std::string> command_cache;
static std::atomic<bool> path_scan_done(false);

// Background scan for executables in PATH
void scan_path_commands() {
  const char* path_env = getenv("PATH");
  if (!path_env) {
    path_scan_done = true;
    return;
  }

  std::stringstream path_stream(path_env);
  std::string path_dir;
  while (std::getline(path_stream, path_dir, ':')) {
    DIR* dir = opendir(path_dir.c_str());
    if (!dir)
      continue;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string name = entry->d_name;
      std::string full_path = path_dir + "/" + name;
      struct stat file_stat;
      if (stat(full_path.c_str(), &file_stat) == 0) {
        if (S_ISREG(file_stat.st_mode) && (file_stat.st_mode & S_IXUSR)) {
          command_cache.insert(name);
        }
      }
    }
    closedir(dir);
  }

  path_scan_done = true;  // mark cache ready
}

void add_builtin_commands() {
  command_cache.insert("cd");
  command_cache.insert("pwd");
  command_cache.insert("echo");
  command_cache.insert("ls");
  command_cache.insert("search");
  command_cache.insert("history");
  command_cache.insert("pinfo");
  command_cache.insert("jobs");
  command_cache.insert("clear");
  command_cache.insert("exit");
}

void initialize_command_cache() {
  add_builtin_commands();                    // instant load
  std::thread(scan_path_commands).detach();  // async path scan
}

std::string find_longest_common_prefix(
    const std::vector<std::string>& matches) {
  if (matches.empty()) {
    return "";
  }
  std::string lcp = matches[0];
  for (size_t i = 1; i < matches.size(); ++i) {
    size_t j = 0;
    while (j < lcp.length() && j < matches[i].length() &&
           lcp[j] == matches[i][j]) {
      j++;
    }
    lcp = lcp.substr(0, j);
  }
  return lcp;
}

std::vector<std::string> get_completions(const std::string& prefix,
                                         bool is_command) {
  std::vector<std::string> matches;
  if (is_command) {
    // Command Completion
    for (const auto& cmd : command_cache) {
      if (cmd.rfind(prefix, 0) == 0) {
        matches.push_back(cmd);
      }
    }
  } else {
    // File/Directory Completion
    DIR* dir = opendir(".");
    if (dir == nullptr) {
      return matches;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string name = entry->d_name;
      if (name.rfind(prefix, 0) == 0) {
        matches.push_back(name);
      }
    }
    closedir(dir);
  }
  return matches;
}

std::vector<std::string> handle_autocomplete(std::string& line,
                                             size_t& cursor_pos) {
  size_t start_of_word = line.find_last_of(" \t;", cursor_pos - 1);

  if (start_of_word == std::string::npos) {
    start_of_word = 0;  // It's the first word
  } else {
    start_of_word++;  // Move past the space
  }

  std::string prefix = line.substr(start_of_word, cursor_pos - start_of_word);
  bool is_command = (start_of_word == 0);

  std::vector<std::string> matches = get_completions(prefix, is_command);

  if (matches.empty()) {
    return {};  // No matches, do nothing
  }

  if (matches.size() == 1) {
    // Single match: complete the word
    std::string completion = matches[0].substr(prefix.length());
    line.insert(cursor_pos, completion);
    // Update the cursor position to the end of the newly inserted text
    cursor_pos += completion.length();
    DIR* dir = opendir(matches[0].c_str());
    if (dir != NULL) {
      // directory, so add a slash
      line += "/";
      closedir(dir);
    } else {
      // file, so add a space
      line += " ";
    }
    cursor_pos++;
  } else {
    // Multiple matches: complete the longest common prefix
    std::string lcp = find_longest_common_prefix(matches);
    if (lcp.length() > prefix.length()) {
      std::string completion = lcp.substr(prefix.length());
      // insert for correctness when editing in the middle of a line
      line.insert(cursor_pos, completion);
      // Update the cursor position
      cursor_pos += completion.length();
    }
  }
  return matches;
}