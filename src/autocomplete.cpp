#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "autocomplete.h"

namespace {

// Executable names found on PATH (plus builtins), scanned once at startup.
std::set<std::string> command_cache;

bool is_executable_regular(const std::string& path, mode_t mode) {
  // Trust the readdir d_type/mode hint instead of stat()ing each file:
  // on WSL, /mnt/c (9p/DrvFs) stat()s cost milliseconds each, which made
  // startup take seconds when PATH contained Windows directories.
  // S_IXUSR is present in dirent d_mode on most local filesystems; when the
  // type is unknown (DT_UNKNOWN) we conservatively accept the entry.
  (void)path;
  return mode == 0 /* unknown */ ||
         (S_ISREG(mode) && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)));
}

void scan_path_commands() {
  const char* path_env = getenv("PATH");
  if (path_env == nullptr) return;

  std::string path = path_env;
  size_t start = 0;
  while (start <= path.size()) {
    size_t colon = path.find(':', start);
    std::string dir = (colon == std::string::npos)
                          ? path.substr(start)
                          : path.substr(start, colon - start);
    if (dir.empty()) dir = ".";

    DIR* d = opendir(dir.c_str());
    if (d != nullptr) {
      struct dirent* entry;
      while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        mode_t mode = 0;
#ifdef _DIRENT_HAVE_D_TYPE
        if (entry->d_type == DT_REG) mode = S_IFREG | S_IXUSR;
        else if (entry->d_type == DT_UNKNOWN) mode = 0;
        else continue;  // Directories, symlinks, etc. -- skip cheaply.
#else
        mode = 0;  // No d_type: accept everything.
#endif
        if (is_executable_regular(dir + "/" + name, mode)) {
          command_cache.insert(name);
        }
      }
      closedir(d);
    }

    if (colon == std::string::npos) break;
    start = colon + 1;
  }
}

void add_builtin_commands() {
  for (const char* b : {"cd", "pwd", "echo", "ls", "search", "history",
                        "pinfo", "jobs", "clear", "exit"}) {
    command_cache.insert(b);
  }
}

// Splits `prefix` into (directory part, filename prefix). "/bin/ls" ->
// ("/bin/", "ls"); "src/ma" -> ("src/", "ma"); "foo" -> ("", "foo").
std::pair<std::string, std::string> split_path_prefix(
    const std::string& prefix) {
  size_t slash = prefix.find_last_of('/');
  if (slash == std::string::npos) return {"", prefix};
  return {prefix.substr(0, slash + 1), prefix.substr(slash + 1)};
}

std::vector<std::string> complete_file(const std::string& prefix) {
  std::vector<std::string> matches;
  auto [dir_part, name_prefix] = split_path_prefix(prefix);

  std::string dir_to_open = dir_part.empty() ? "." : dir_part;
  DIR* dir = opendir(dir_to_open.c_str());
  if (dir == nullptr) return matches;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;

    // Bash rule: dotfiles are hidden unless the prefix starts with '.'.
    if (name_prefix.empty() && name[0] == '.') continue;
    if (!name_prefix.empty() && name_prefix[0] != '.' && name[0] == '.') {
      continue;
    }
    // The explicit directory part may contain its own dotfiles (e.g.
    // completing "./.conf"), so only apply the rule to the basename.

    if (name.rfind(name_prefix, 0) == 0) {
      matches.push_back(dir_part + name);
    }
  }
  closedir(dir);

  std::sort(matches.begin(), matches.end());
  return matches;
}

std::vector<std::string> complete_command(const std::string& prefix) {
  std::vector<std::string> matches;
  for (const auto& cmd : command_cache) {
    if (cmd.rfind(prefix, 0) == 0) matches.push_back(cmd);
  }
  return matches;
}

bool is_directory(const std::string& path) {
  struct stat st;
  // Use stat (follow links): completing a symlink-to-dir should add '/'.
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string find_longest_common_prefix(const std::vector<std::string>& matches) {
  if (matches.empty()) return "";
  std::string lcp = matches[0];
  for (size_t i = 1; i < matches.size(); ++i) {
    size_t j = 0;
    while (j < lcp.length() && j < matches[i].length() &&
           lcp[j] == matches[i][j]) {
      j++;
    }
    lcp.resize(j);
  }
  return lcp;
}

}  // namespace

void initialize_command_cache() {
  add_builtin_commands();
  // Synchronous, one-time scan. A detached thread (the old approach) raced
  // with completions and made early Tabs miss PATH commands.
  scan_path_commands();
}

std::vector<std::string> handle_autocomplete(std::string& line,
                                             size_t& cursor_pos) {
  // Find the start of the word the cursor is in (no size_t underflow when
  // cursor_pos == 0).
  size_t start_of_word = 0;
  if (cursor_pos > 0) {
    size_t sep = line.find_last_of(" \t;", cursor_pos - 1);
    start_of_word = (sep == std::string::npos) ? 0 : sep + 1;
  }

  const std::string prefix = line.substr(start_of_word, cursor_pos - start_of_word);
  const bool is_command = (start_of_word == 0);

  std::vector<std::string> matches =
      is_command ? complete_command(prefix) : complete_file(prefix);

  if (matches.empty()) return {};

  // The completed word (what replaces [start_of_word, cursor_pos)).
  std::string completed;

  if (matches.size() == 1) {
    completed = matches[0];
    // Directories get a trailing '/', plain files a space.
    if (is_directory(matches[0])) {
      if (completed.back() != '/') completed += '/';
    } else {
      completed += ' ';
    }
  } else {
    std::string lcp = find_longest_common_prefix(matches);
    if (lcp.length() <= prefix.length()) {
      return matches;  // Nothing longer to insert.
    }
    if (is_directory(lcp)) lcp += '/';
    completed = lcp;
  }

  // Replace the whole word region so mid-line completion is correct:
  // erase [start_of_word, cursor_pos), insert the completion there.
  line.erase(start_of_word, cursor_pos - start_of_word);
  line.insert(start_of_word, completed);
  cursor_pos = start_of_word + completed.length();

  return matches;
}
