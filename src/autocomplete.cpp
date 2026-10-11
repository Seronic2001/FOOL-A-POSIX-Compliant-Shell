#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "autocomplete.h"
#include "builtins.h"

namespace {

// Builtins plus the names found on PATH. Built on the first Tab rather
// than at startup: on WSL, PATH includes Windows directories that take
// hundreds of milliseconds to read, which every script run would pay for.
const std::set<std::string>& command_cache() {
  static std::set<std::string> cache;
  static bool scanned = false;
  if (scanned) return cache;
  scanned = true;

  cache.insert(builtin_names().begin(), builtin_names().end());

  const char* path_env = getenv("PATH");
  const std::string path = path_env != nullptr ? path_env : "";
  size_t start = 0;
  while (start <= path.size()) {
    size_t colon = path.find(':', start);
    if (colon == std::string::npos) colon = path.size();
    std::string dir = path.substr(start, colon - start);
    if (dir.empty()) dir = ".";
    start = colon + 1;

    DIR* d = opendir(dir.c_str());
    if (d == nullptr) continue;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      // Trust readdir's d_type instead of stat()ing each file (slow on
      // /mnt/c). Symlinks count: many commands are installed as links.
      if (entry->d_type == DT_REG || entry->d_type == DT_LNK ||
          entry->d_type == DT_UNKNOWN) {
        cache.insert(entry->d_name);
      }
    }
    closedir(d);
  }
  return cache;
}

std::vector<std::string> complete_file(const std::string& prefix) {
  std::vector<std::string> matches;
  // "/bin/ls" -> ("/bin/", "ls"); "src/ma" -> ("src/", "ma").
  const size_t slash = prefix.find_last_of('/');
  const std::string dir_part =
      slash == std::string::npos ? "" : prefix.substr(0, slash + 1);
  const std::string name_prefix = prefix.substr(dir_part.size());

  DIR* dir = opendir(dir_part.empty() ? "." : dir_part.c_str());
  if (dir == nullptr) return matches;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    // Bash rule: dotfiles are hidden unless the prefix starts with '.'.
    if (name[0] == '.' && (name_prefix.empty() || name_prefix[0] != '.')) {
      continue;
    }
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
  const auto& cache = command_cache();
  for (auto it = cache.lower_bound(prefix);
       it != cache.end() && it->rfind(prefix, 0) == 0; ++it) {
    matches.push_back(*it);
  }
  return matches;
}

bool is_directory(const std::string& path) {
  struct stat st;
  // Use stat (follow links): completing a symlink-to-dir should add '/'.
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string find_longest_common_prefix(const std::vector<std::string>& matches) {
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

std::vector<std::string> handle_autocomplete(std::string& line,
                                             size_t& cursor_pos) {
  // Find the start of the word the cursor is in (no size_t underflow when
  // cursor_pos == 0).
  size_t start_of_word = 0;
  if (cursor_pos > 0) {
    size_t sep = line.find_last_of(" \t;|&<>", cursor_pos - 1);
    start_of_word = (sep == std::string::npos) ? 0 : sep + 1;
  }
  const std::string prefix =
      line.substr(start_of_word, cursor_pos - start_of_word);

  // The word names a command when it is the first of a pipeline stage:
  // nothing before it, or the nearest non-blank before it is ; | or &.
  // A command given as a path ("./run", "/bin/ls") completes as a file.
  const size_t before = line.find_last_not_of(" \t", start_of_word - 1);
  const bool command_position =
      start_of_word == 0 || before == std::string::npos ||
      line[before] == ';' || line[before] == '|' || line[before] == '&';
  const bool as_file =
      !command_position || prefix.find('/') != std::string::npos;

  std::vector<std::string> matches =
      as_file ? complete_file(prefix) : complete_command(prefix);

  if (matches.empty()) return {};

  // The completed word (what replaces [start_of_word, cursor_pos)).
  std::string completed;

  if (matches.size() == 1) {
    completed = matches[0];
    // Directories get a trailing '/', everything else a space.
    completed += (as_file && is_directory(completed)) ? '/' : ' ';
  } else {
    completed = find_longest_common_prefix(matches);
    if (completed.length() <= prefix.length()) {
      return matches;  // Nothing longer to insert.
    }
  }

  // Replace the whole word region so mid-line completion is correct.
  line.replace(start_of_word, cursor_pos - start_of_word, completed);
  cursor_pos = start_of_word + completed.length();

  return matches;
}
