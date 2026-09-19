#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
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
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "builtins.h"
#include "colors.h"
#include "executor.h"
#include "history.h"

extern std::string shell_home_dir;

// ---------------- cd ----------------

void execute_cd(const std::vector<std::string>& args, std::ostream& out) {
  if (args.size() > 2) {
    out << "cd : too many arguments \n";
    return;
  }

  static std::string previous_dir;  // Shell state, must live in the parent.

  std::string target_path;

  if (args.size() == 1 || args[1] == "~") {
    const char* home_dir = getenv("HOME");
    if (home_dir == nullptr || home_dir[0] == '\0') {
      std::cerr << "cd: HOME not set" << std::endl;
      return;
    }
    target_path = home_dir;
  } else if (args[1] == "-") {
    if (previous_dir.empty()) {
      std::cerr << "cd: OLDPWD not set" << std::endl;
      return;
    }
    target_path = previous_dir;
    out << target_path << std::endl;
  } else {
    target_path = args[1];
  }

  // Resolve ~ at the start of the target (cd ~/docs).
  if (target_path.size() >= 2 && target_path[0] == '~' &&
      (target_path[1] == '/' || target_path.size() == 2)) {
    const char* home_dir = getenv("HOME");
    if (home_dir != nullptr) {
      target_path = std::string(home_dir) + target_path.substr(1);
    }
  }

  char cwd_buffer[PATH_MAX];
  if (getcwd(cwd_buffer, sizeof(cwd_buffer)) == nullptr) {
    perror("cd: error getting current directory");
    return;
  }

  if (chdir(target_path.c_str()) != 0) {
    perror(("cd: " + target_path).c_str());
  } else {
    // Only remember the previous directory on success.
    previous_dir = cwd_buffer;
  }
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

void print_colored_name(const struct stat& file_stat,
                        const std::string& name,
                        std::ostream& out) {
  if (S_ISDIR(file_stat.st_mode)) {
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

// Safe wrappers: getpwuid/getgrgid return NULL for unknown ids.
static std::string user_name(uid_t uid) {
  struct passwd* pw = getpwuid(uid);
  if (pw != nullptr && pw->pw_name != nullptr) return pw->pw_name;
  return std::to_string(uid);
}

static std::string group_name(gid_t gid) {
  struct group* gr = getgrgid(gid);
  if (gr != nullptr && gr->gr_name != nullptr) return gr->gr_name;
  return std::to_string(gid);
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

// The basename of a path, used to display entries in long listings.
static std::string name_of_path(const std::string& path) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return path;
  if (slash + 1 >= path.size()) return path;
  return path.substr(slash + 1);
}

void print_long_format(const std::string& path,
                       bool human_readable,
                       std::ostream& out) {
  struct stat file_stat;
  // lstat: report the link itself for symlinks (like ls does).
  if (lstat(path.c_str(), &file_stat) == -1) {
    perror(("ls: " + path).c_str());
    return;
  }

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
      out << name_of_path(path) << " -> " << target;
      return;
    }
  }
  out << name_of_path(path);
}

struct DirectoryEntry {
  std::string name;
  struct stat stat_info;
};

void list_directory(const std::string& path,
                    bool show_all,
                    bool long_format,
                    bool human_readable,
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
      // print_long_format does not emit a trailing newline.
      print_long_format(path + "/" + ent.name, human_readable, out);
    } else {
      print_colored_name(ent.stat_info, ent.name, out);
    }
    out << "\n";
  }
}

int execute_ls(const std::vector<std::string>& args, std::ostream& out) {
  bool show_all = false;
  bool human_readable = false;
  bool long_format = false;
  std::vector<std::string> operands;  // Files and directories, in order.

  for (size_t i = 1; i < args.size(); i++) {
    const std::string& arg = args[i];
    if (arg.size() > 1 && arg[0] == '-' && arg != "--") {
      for (char flag : arg.substr(1)) {
        if (flag == 'l') {
          long_format = true;
        } else if (flag == 'a') {
          show_all = true;
        } else if (flag == 'h') {
          human_readable = true;
        } else {
          std::cerr << "ls: invalid option -- '" << flag << "'" << std::endl;
          return 1;
        }
      }
    } else if (arg == "--") {
      // Everything after -- is an operand, even if it starts with '-'.
      for (size_t j = i + 1; j < args.size(); j++) operands.push_back(args[j]);
      break;
    } else {
      operands.push_back(arg);
    }
  }

  if (operands.empty()) operands.push_back(".");

  // Classify operands once, preserving their relative order.
  std::vector<std::string> files, dirs;
  for (const auto& op : operands) {
    struct stat path_stat;
    // Use lstat on the operand itself: `ls broken_symlink` must list the
    // link, and `ls symlink_to_dir` behaves like the directory (stat).
    if (stat(op.c_str(), &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
      dirs.push_back(op);
    } else if (lstat(op.c_str(), &path_stat) == 0) {
      files.push_back(op);
    } else {
      std::cerr << "ls: cannot access '" << op << "': "
          << std::strerror(errno) << std::endl;
    }
  }

  for (const auto& path : files) {
    if (long_format) {
      print_long_format(path, human_readable, out);
    } else {
      struct stat st;
      if (lstat(path.c_str(), &st) == 0) {
        print_colored_name(st, path, out);
      }
    }
    out << "\n";
  }

  if (!files.empty() && !dirs.empty()) out << "\n";

  for (size_t i = 0; i < dirs.size(); ++i) {
    if (files.size() + dirs.size() > 1) {
      out << dirs[i] << ":" << "\n";
    }
    list_directory(dirs[i], show_all, long_format, human_readable, out);
    if (i < dirs.size() - 1) out << "\n";
  }
  return 0;
}

int execute_clear(const std::vector<std::string>& args, std::ostream& out) {
  (void)args;
  out << "\x1b[H\x1b[2J" << std::flush;
  return 0;
}

// ---------------- pinfo ----------------

// /proc/<pid>/stat's comm field (2nd) is "(...)" and may contain spaces.
// Parse defensively: find the closing ')' and scan fields after it.
static bool parse_proc_stat(pid_t pid, char* state, pid_t* pgrp,
                            unsigned long* vsize) {
  std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
  std::ifstream f(stat_path);
  if (!f) return false;

  std::string content;
  std::getline(f, content);

  size_t close_paren = content.rfind(')');
  if (close_paren == std::string::npos) return false;

  // Fields after comm: state pgrp session ... vsize is field 23 overall.
  // After ')' the remainder starts at overall field 3 => remainder index 0
  // is state, index 2 is pgrp (3,4,5 of the line: "state pgrp session").
  std::istringstream rest(content.substr(close_paren + 1));
  std::string state_tok, pgrp_tok;
  if (!(rest >> state_tok)) return false;
  if (!(rest >> pgrp_tok)) return false;

  // Skip to vsize (overall field 23 => remainder field 21).
  unsigned long skip;
  for (int i = 0; i < 17; ++i) {
    if (!(rest >> skip)) return false;  // fields 6..22 overall
  }
  std::string vsize_tok;
  if (!(rest >> vsize_tok)) return false;

  *state = state_tok[0];
  *pgrp = (pid_t)std::strtol(pgrp_tok.c_str(), nullptr, 10);
  *vsize = std::strtoul(vsize_tok.c_str(), nullptr, 10);
  return true;
}

void print_process_info(pid_t pid, std::ostream& out) {
  char state = '?';
  pid_t pgrp = 0;
  unsigned long vsize = 0;

  if (!parse_proc_stat(pid, &state, &pgrp, &vsize)) {
    std::cerr << "pinfo: process with pid " << pid << " not found" << std::endl;
    return;
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
  pid_t terminal_pgrp = tcgetpgrp(STDIN_FILENO);
  if (pgrp == terminal_pgrp) state_str += "+";

  out << "pid -- " << pid << std::endl;
  out << "Process Status -- " << state_str << std::endl;
  out << "Memory -- " << vsize / 1024 << " KB {Virtual Memory}" << std::endl;
  out << "Executable Path -- " << exe_path << std::endl;
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
  print_process_info(target_pid, out);
  return 0;
}

// ---------------- search ----------------

// Iterative DFS with a visited-(device,inode) set: immune to symlink loops
// and to directory cycles that a recursive stat()-based walker would chase
// forever (or stack-overflow on).
static bool recursive_search(const std::string& root,
                             const std::string& target_name) {
  struct Ctx {
    std::string path;
  };

  std::set<std::pair<dev_t, ino_t>> visited;
  std::vector<Ctx> stack;
  stack.push_back({root});

  while (!stack.empty()) {
    Ctx ctx = stack.back();
    stack.pop_back();

    DIR* dir = opendir(ctx.path.c_str());
    if (dir == nullptr) continue;
    std::unique_ptr<DIR, int (*)(DIR*)> dir_guard(dir, closedir);

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string name = entry->d_name;

      if (name == target_name) return true;

      std::string full_path =
          ctx.path == "." ? name : ctx.path + "/" + name;

      struct stat st;
      // lstat: never follow symlinks while walking.
      if (lstat(full_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

      if (name == "." || name == "..") continue;

      auto key = std::make_pair(st.st_dev, st.st_ino);
      if (!visited.insert(key).second) continue;  // Already walked.

      stack.push_back({full_path});
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
  // Snapshot under our lock-free convention: the main thread is the only
  // mutator, and this runs on the main thread.
  for (const auto& job : background_jobs) {
    out << "[" << job.id << "] " << (job.is_stopped ? "Stopped" : "Running")
        << " " << job.command_name << " [" << job.pgid << "]" << std::endl;
  }
  return 0;
}
