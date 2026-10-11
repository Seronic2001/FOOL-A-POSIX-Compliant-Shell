#include <dirent.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "builtins.h"
#include "colors.h"
#include "executor.h"

const std::vector<std::string>& builtin_names() {
  static const std::vector<std::string> names = {
      "cd", "pwd", "echo", "ls", "search", "history", "pinfo",
      "jobs", "fg", "bg", "clear", "exit"};
  return names;
}

// ---------------- cd ----------------

// `~` is expanded by the executor before the arguments get here.
int execute_cd(const std::vector<std::string>& args, std::ostream& out) {
  if (args.size() > 2) {
    std::cerr << "cd: too many arguments" << std::endl;
    return 1;
  }

  static std::string previous_dir;  // Shell state, must live in the parent.

  std::string target_path;

  if (args.size() == 1) {
    const char* home_dir = getenv("HOME");
    if (home_dir == nullptr || home_dir[0] == '\0') {
      std::cerr << "cd: HOME not set" << std::endl;
      return 1;
    }
    target_path = home_dir;
  } else if (args[1] == "-") {
    if (previous_dir.empty()) {
      std::cerr << "cd: OLDPWD not set" << std::endl;
      return 1;
    }
    target_path = previous_dir;
    out << target_path << std::endl;
  } else {
    target_path = args[1];
  }

  char cwd_buffer[PATH_MAX];
  if (getcwd(cwd_buffer, sizeof(cwd_buffer)) == nullptr) {
    perror("cd: error getting current directory");
    return 1;
  }

  if (chdir(target_path.c_str()) != 0) {
    perror(("cd: " + target_path).c_str());
    return 1;
  }
  // Only remember the previous directory on success.
  previous_dir = cwd_buffer;
  return 0;
}

// ---------------- pwd ----------------

int execute_pwd(const std::vector<std::string>& args, std::ostream& out) {
  (void)args;
  char cwd_buffer[PATH_MAX];
  if (getcwd(cwd_buffer, sizeof(cwd_buffer)) != nullptr) {
    out << cwd_buffer << "\n";
  } else {
    perror("pwd");
  }
  return 0;
}

// ---------------- echo ----------------

int execute_echo(const std::vector<std::string>& args,
                 std::istream& in,
                 std::ostream& out) {
  (void)in;
  bool suppress_newline = false;
  size_t first_arg = 1;

  if (args.size() > 1 && args[1] == "-n") {
    suppress_newline = true;
    first_arg = 2;
  }

  for (size_t i = first_arg; i < args.size(); i++) {
    out << args[i];
    if (i < args.size() - 1) {
      out << " ";
    }
  }
  if (!suppress_newline) out << "\n";
  return 0;
}

// ---------------- ls helpers ----------------

std::string human_readable_size(off_t size) {
  const char* suffixes[] = {"B", "K", "M", "G", "T"};
  int suffix_index = 0;
  double formatted_size = (double)size;

  while (formatted_size >= 1024 && suffix_index < 4) {
    formatted_size /= 1024.0;
    suffix_index++;
  }

  std::stringstream ss;
  if (suffix_index > 0) {
    ss << std::fixed << std::setprecision(1) << formatted_size
       << suffixes[suffix_index];
  } else {
    ss << formatted_size << suffixes[suffix_index];
  }
  return ss.str();
}

// Colors are only used when `color` is set (stdout is a terminal), so
// `ls | grep` and `ls > file` see plain names.
void print_colored_name(const struct stat& file_stat,
                        const std::string& name,
                        bool color,
                        std::ostream& out) {
  if (!color) {
    out << name;
  } else if (S_ISDIR(file_stat.st_mode)) {
    out << BOLD_BLUE << name << RESET;
  } else if (S_ISREG(file_stat.st_mode) &&
             (file_stat.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
    out << BOLD_GREEN << name << RESET;
  } else if (S_ISLNK(file_stat.st_mode)) {
    out << BOLD_CYAN << name << RESET;
  } else {
    out << name;
  }
}

// getpwuid/getgrgid return NULL for unknown ids, and each call may hit the
// user database, so results are cached for the length of a listing.
static const std::string& user_name(uid_t uid) {
  static std::map<uid_t, std::string> cache;
  auto it = cache.find(uid);
  if (it != cache.end()) return it->second;
  struct passwd* pw = getpwuid(uid);
  return cache[uid] = (pw != nullptr && pw->pw_name != nullptr)
                          ? pw->pw_name
                          : std::to_string(uid);
}

static const std::string& group_name(gid_t gid) {
  static std::map<gid_t, std::string> cache;
  auto it = cache.find(gid);
  if (it != cache.end()) return it->second;
  struct group* gr = getgrgid(gid);
  return cache[gid] = (gr != nullptr && gr->gr_name != nullptr)
                          ? gr->gr_name
                          : std::to_string(gid);
}

static char type_letter(mode_t mode) {
  if (S_ISDIR(mode)) return 'd';
  if (S_ISLNK(mode)) return 'l';
  if (S_ISCHR(mode)) return 'c';
  if (S_ISBLK(mode)) return 'b';
  if (S_ISFIFO(mode)) return 'p';
  if (S_ISSOCK(mode)) return 's';
  return '-';
}

// Prints one long-format line (no trailing newline) for `path`, shown as
// `name`, from an lstat() result the caller already has.
void print_long_format(const std::string& path,
                       const std::string& name,
                       const struct stat& file_stat,
                       bool human_readable,
                       std::ostream& out) {
  out << type_letter(file_stat.st_mode);
  out << ((file_stat.st_mode & S_IRUSR) ? "r" : "-");
  out << ((file_stat.st_mode & S_IWUSR) ? "w" : "-");
  out << ((file_stat.st_mode & S_IXUSR) ? "x" : "-");
  out << ((file_stat.st_mode & S_IRGRP) ? "r" : "-");
  out << ((file_stat.st_mode & S_IWGRP) ? "w" : "-");
  out << ((file_stat.st_mode & S_IXGRP) ? "x" : "-");
  out << ((file_stat.st_mode & S_IROTH) ? "r" : "-");
  out << ((file_stat.st_mode & S_IWOTH) ? "w" : "-");
  out << ((file_stat.st_mode & S_IXOTH) ? "x" : "-");

  out << " " << file_stat.st_nlink;
  out << " " << user_name(file_stat.st_uid);
  out << " " << group_name(file_stat.st_gid);

  if (human_readable) {
    out << " " << std::setw(6) << human_readable_size(file_stat.st_size);
  } else {
    out << " " << std::setw(6) << file_stat.st_size;
  }

  // ls convention: files newer than ~6 months show the time, older ones the
  // year instead.
  time_t now = time(nullptr);
  const char* fmt =
      (now - file_stat.st_mtime > 6 * 30 * 24 * 3600) ? "%b %d  %Y"
                                                      : "%b %d %H:%M";
  char time_buf[80];
  strftime(time_buf, sizeof(time_buf), fmt, localtime(&file_stat.st_mtime));
  out << " " << time_buf << " ";

  // Show symlink targets like real ls.
  if (S_ISLNK(file_stat.st_mode)) {
    char target[PATH_MAX];
    ssize_t len = readlink(path.c_str(), target, sizeof(target) - 1);
    if (len >= 0) {
      target[len] = '\0';
      out << name << " -> " << target;
      return;
    }
  }
  out << name;
}

struct DirectoryEntry {
  std::string name;
  struct stat stat_info;
};

void list_directory(const std::string& path,
                    bool show_all,
                    bool long_format,
                    bool human_readable,
                    bool color,
                    std::ostream& out) {
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    perror(("ls: cannot open '" + path + "'").c_str());
    return;
  }
  std::unique_ptr<DIR, int (*)(DIR*)> dir_guard(dir, closedir);

  std::vector<DirectoryEntry> entries;
  long total_blocks = 0;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (!show_all && entry->d_name[0] == '.') continue;

    DirectoryEntry current_entry;
    current_entry.name = entry->d_name;

    std::string full_path = path + "/" + current_entry.name;
    // lstat so symlinks are listed as themselves; a broken symlink is still
    // listed instead of silently vanishing.
    if (lstat(full_path.c_str(), &current_entry.stat_info) == 0) {
      entries.push_back(current_entry);
      total_blocks += current_entry.stat_info.st_blocks;
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const DirectoryEntry& a, const DirectoryEntry& b) {
              return a.name < b.name;
            });

  if (long_format) {
    out << "total " << total_blocks / 2 << "\n";
  }

  for (const auto& ent : entries) {
    if (long_format) {
      print_long_format(path + "/" + ent.name, ent.name, ent.stat_info,
                        human_readable, out);
    } else {
      print_colored_name(ent.stat_info, ent.name, color, out);
    }
    out << "\n";
  }
}

struct LsOptions {
  bool show_all = false;
  bool human_readable = false;
  bool long_format = false;
  std::vector<std::string> operands;  // Files and directories, in order.
};

// Returns the first option character that isn't supported, or 0.
static char parse_ls_args(const std::vector<std::string>& args,
                          LsOptions& opts) {
  for (size_t i = 1; i < args.size(); i++) {
    const std::string& arg = args[i];
    if (arg == "--") {
      // Everything after -- is an operand, even if it starts with '-'.
      opts.operands.insert(opts.operands.end(), args.begin() + i + 1,
                           args.end());
      break;
    }
    if (arg.size() < 2 || arg[0] != '-') {
      opts.operands.push_back(arg);
      continue;
    }
    for (char flag : arg.substr(1)) {
      if (flag == 'l') {
        opts.long_format = true;
      } else if (flag == 'a') {
        opts.show_all = true;
      } else if (flag == 'h') {
        opts.human_readable = true;
      } else {
        return flag;
      }
    }
  }
  return 0;
}

bool ls_supports(const std::vector<std::string>& args) {
  LsOptions opts;
  return parse_ls_args(args, opts) == 0;
}

int execute_ls(const std::vector<std::string>& args, std::ostream& out) {
  LsOptions opts;
  if (char bad = parse_ls_args(args, opts)) {
    std::cerr << "ls: invalid option -- '" << bad << "'" << std::endl;
    return 1;
  }
  if (opts.operands.empty()) opts.operands.push_back(".");
  const bool color = isatty(STDOUT_FILENO);
  int status = 0;

  // Classify operands once, preserving their relative order. A symlink to
  // a directory is listed as the directory (stat); anything else,
  // including a broken symlink, as itself (lstat).
  std::vector<std::pair<std::string, struct stat>> files;
  std::vector<std::string> dirs;
  for (const auto& op : opts.operands) {
    struct stat path_stat;
    if (stat(op.c_str(), &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
      dirs.push_back(op);
    } else if (lstat(op.c_str(), &path_stat) == 0) {
      files.emplace_back(op, path_stat);
    } else {
      std::cerr << "ls: cannot access '" << op << "': "
          << std::strerror(errno) << std::endl;
      status = 1;
    }
  }

  for (const auto& [path, path_stat] : files) {
    if (opts.long_format) {
      print_long_format(path, path, path_stat, opts.human_readable, out);
    } else {
      print_colored_name(path_stat, path, color, out);
    }
    out << "\n";
  }

  if (!files.empty() && !dirs.empty()) out << "\n";

  for (size_t i = 0; i < dirs.size(); ++i) {
    if (files.size() + dirs.size() > 1) {
      out << dirs[i] << ":" << "\n";
    }
    list_directory(dirs[i], opts.show_all, opts.long_format,
                   opts.human_readable, color, out);
    if (i < dirs.size() - 1) out << "\n";
  }
  return status;
}

int execute_clear(const std::vector<std::string>& args, std::ostream& out) {
  (void)args;
  out << "\x1b[H\x1b[2J\x1b[3J" << std::flush;
  return 0;
}

// ---------------- pinfo ----------------

// /proc/<pid>/stat's comm field (2nd) is "(...)" and may contain spaces.
// Parse defensively: find the closing ')' and split the fields after it.
// `foreground` is set when the process group owns its terminal.
static bool parse_proc_stat(pid_t pid, char* state, bool* foreground,
                            unsigned long* vsize) {
  std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
  std::ifstream f(stat_path);
  if (!f) return false;

  std::string content;
  std::getline(f, content);

  size_t close_paren = content.rfind(')');
  if (close_paren == std::string::npos) return false;

  // fields[i] is field i + 3 of proc(5): state(3) ppid(4) pgrp(5)
  // session(6) tty_nr(7) tpgid(8) ... vsize(23).
  std::istringstream rest(content.substr(close_paren + 1));
  std::vector<std::string> fields;
  for (std::string field; rest >> field;) fields.push_back(field);
  if (fields.size() < 21) return false;

  *state = fields[0][0];
  *foreground = fields[2] == fields[5];  // pgrp == tpgid
  *vsize = std::strtoul(fields[20].c_str(), nullptr, 10);
  return true;
}

static int print_process_info(pid_t pid, std::ostream& out) {
  char state = '?';
  bool foreground = false;
  unsigned long vsize = 0;

  if (!parse_proc_stat(pid, &state, &foreground, &vsize)) {
    std::cerr << "pinfo: process with pid " << pid << " not found" << std::endl;
    return 1;
  }

  std::string exe_path = "path not accessible";
  {
    std::string exe_link = "/proc/" + std::to_string(pid) + "/exe";
    std::vector<char> buf(PATH_MAX);
    ssize_t len = readlink(exe_link.c_str(), buf.data(), buf.size() - 1);
    if (len >= 0) {
      buf[len] = '\0';
      exe_path = buf.data();
    }
  }

  const char* state_name;
  switch (state) {
    case 'R': state_name = "Running"; break;
    case 'S': state_name = "Sleeping"; break;
    case 'D': state_name = "Disk Sleep"; break;
    case 'Z': state_name = "Zombie"; break;
    case 'T': state_name = "Stopped"; break;
    case 'I': state_name = "Idle"; break;
    default: state_name = "Unknown"; break;
  }

  std::string state_str = std::string(1, state) + " {" + state_name + "}";
  if (foreground) state_str += "+";

  out << "pid -- " << pid << std::endl;
  out << "Process Status -- " << state_str << std::endl;
  out << "Memory -- " << vsize / 1024 << " KB {Virtual Memory}" << std::endl;
  out << "Executable Path -- " << exe_path << std::endl;
  return 0;
}

int execute_pinfo(const std::vector<std::string>& args, std::ostream& out) {
  if (args.size() > 2) {
    std::cerr << "pinfo: too many arguments" << std::endl;
    return 1;
  }

  pid_t target_pid;
  if (args.size() == 1) {
    target_pid = getpid();
  } else {
    try {
      size_t pos = 0;
      long value = std::stol(args[1], &pos);
      if (pos != args[1].size() || value < 1) throw std::invalid_argument("");
      target_pid = (pid_t)value;
    } catch (const std::exception&) {
      std::cerr << "pinfo: invalid pid '" << args[1] << "'" << std::endl;
      return 1;
    }
  }
  return print_process_info(target_pid, out);
}

// ---------------- search ----------------

// Iterative DFS. Symlinks are never followed, so the walk cannot loop,
// and readdir's d_type spares an lstat() per entry on most filesystems.
static bool recursive_search(const std::string& root,
                             const std::string& target_name) {
  std::vector<std::string> stack = {root};

  while (!stack.empty()) {
    const std::string dir_path = std::move(stack.back());
    stack.pop_back();

    DIR* dir = opendir(dir_path.c_str());
    if (dir == nullptr) continue;
    std::unique_ptr<DIR, int (*)(DIR*)> dir_guard(dir, closedir);

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      const std::string name = entry->d_name;
      if (name == target_name) return true;
      if (name == "." || name == "..") continue;

      std::string full_path = dir_path == "." ? name : dir_path + "/" + name;

      bool is_dir = entry->d_type == DT_DIR;
      if (entry->d_type == DT_UNKNOWN) {
        struct stat st;
        is_dir = lstat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      }
      if (is_dir) stack.push_back(std::move(full_path));
    }
  }
  return false;
}

int execute_search(const std::vector<std::string>& args,
                   std::istream& in,
                   std::ostream& out) {
  if (args.size() == 2) {
    bool found = recursive_search(".", args[1]);
    out << (found ? "True" : "False") << std::endl;
  } else if (args.size() == 1) {
    std::string target_name;
    while (std::getline(in, target_name)) {
      if (!target_name.empty() && target_name.back() == '\r') {
        target_name.pop_back();
      }
      bool found = recursive_search(".", target_name);
      out << target_name << ": " << (found ? "True" : "False") << std::endl;
    }
  } else {
    std::cerr << "search: incorrect number of arguments (expected 0 or 1)"
        << std::endl;
  }
  return 0;
}

// ---------------- jobs ----------------

int execute_jobs(const std::vector<std::string>& args, std::ostream& out) {
  (void)args;
  for (const auto& job : background_jobs) {
    out << "[" << job.id << "] " << (job.is_stopped ? "Stopped" : "Running")
        << " " << job.command_name << " [" << job.pgid << "]" << std::endl;
  }
  return 0;
}
