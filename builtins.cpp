#include <dirent.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "builtins.h"
#include "colors.h"
#include "executor.h"
#include "history.h"

// static variables for built-ins (like for 'cd -') ---
static std::string previous_dir = "";
extern std::string shell_home_dir;

// change directory command
void execute_cd(const std::vector<std::string>& args, std::ostream& out) {
  if (args.size() > 2) {
    out << "cd : too many arguments \n";
    return;
  }

  std::string target_path;

  if (args.size() == 1 || args[1] == "~") {
    // Case 1: "cd" (no arguments) or cd ~ -> go to SYSTEM HOME
    const char* home_dir = getenv("HOME");
    if (home_dir == nullptr) {
      out << "cd: HOME not set" << std::endl;
      return;
    }
    target_path = home_dir;
  } else if (args[1] == "-") {
    // Case 3: "cd -" -> go to previous directory
    if (previous_dir.empty()) {
      out << "cd: OLDPWD not set" << std::endl;
      return;
    }
    target_path = previous_dir;
    // Mimic shell behavior by printing the path
    out << target_path << std::endl;
  } else {
    // Case 4: "cd .", "cd ..", "cd /path/to/dir"
    target_path = args[1];
  }

  char cwd_buffer[PATH_MAX];
  if (getcwd(cwd_buffer, PATH_MAX) == nullptr) {
    perror("cd: error getting current directory");
    return;
  }

  // Execute the directory change
  if (chdir(target_path.c_str()) != 0) {
    perror(("cd: " + target_path).c_str());
  } else {
    // On successful change, update the previous directory
    previous_dir = cwd_buffer;
  }
}

// print working directory command
void execute_pwd(const std::vector<std::string>& args, std::ostream& out) {
  // The 'pwd' command typically ignores extra arguments.
  char cwd_buffer[PATH_MAX];

  if (getcwd(cwd_buffer, sizeof(cwd_buffer)) != nullptr) {
    // On success, print the path followed by a newline.
    out << cwd_buffer << "\n";
  } else {
    perror("pwd");
  }
}

// echo Command
void execute_echo(const std::vector<std::string>& args,
                  std::istream& in,
                  std::ostream& out) {
  if (args.size() > 1) {
    // Case 1: Echo arguments
    // Loop through the arguments, starting from the second element (index 1)
    for (size_t i = 1; i < args.size(); i++) {
      out << args[i];
      if (i < args.size() - 1) {
        out << " ";
      }
    }
    out << "\n";
  } else {
    // Case 2: No args → just print newline
    out << "\n";
  }
}

// helpers for ls command
std::string human_readable_size(off_t size) {
  const char* suffixes[] = {"B", "K", "M", "G", "T"};
  int suffix_index = 0;
  double formatted_size = size;

  // Keep dividing by 1024 until the size is less than 1024
  while (formatted_size >= 1024 && suffix_index < 4) {
    formatted_size /= 1024.0;
    suffix_index++;
  }

  std::stringstream ss;
  // Format to one decimal place, unless it's just bytes
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
    // It's a directory -> print in bold blue
    out << BOLD_BLUE << name << RESET;
  } else if (S_ISREG(file_stat.st_mode) &&
             (file_stat.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
    // It's a regular file with execute permissions -> print in bold green
    out << BOLD_GREEN << name << RESET;
  } else {
    // It's a regular file or something else -> print normally
    out << name;
  }
}

void print_long_format(const std::string& path,
                       bool human_readable,
                       std::ostream& out) {
  struct stat file_stat;
  if (stat(path.c_str(), &file_stat) == -1) {
    perror(("stat: " + path).c_str());
    return;
  }

  // Print Permissions
  out << ((S_ISDIR(file_stat.st_mode)) ? "d" : "-");
  out << ((file_stat.st_mode & S_IRUSR) ? "r" : "-");
  out << ((file_stat.st_mode & S_IWUSR) ? "w" : "-");
  out << ((file_stat.st_mode & S_IXUSR) ? "x" : "-");
  out << ((file_stat.st_mode & S_IRGRP) ? "r" : "-");
  out << ((file_stat.st_mode & S_IWGRP) ? "w" : "-");
  out << ((file_stat.st_mode & S_IXGRP) ? "x" : "-");
  out << ((file_stat.st_mode & S_IROTH) ? "r" : "-");
  out << ((file_stat.st_mode & S_IWOTH) ? "w" : "-");
  out << ((file_stat.st_mode & S_IXOTH) ? "x" : "-");

  // Print Link Count, Owner, Group, Size
  out << " " << file_stat.st_nlink;
  out << " " << getpwuid(file_stat.st_uid)->pw_name;
  out << " " << getgrgid(file_stat.st_gid)->gr_name;

  if (human_readable) {
    out << " " << std::setw(6) << human_readable_size(file_stat.st_size);
  } else {
    out << " " << std::setw(6) << file_stat.st_size;
  }

  // Print Timestamp
  char time_buf[80];
  strftime(time_buf, sizeof(time_buf), "%b %d %H:%M",
           localtime(&file_stat.st_mtime));
  out << " " << time_buf << "  ";
}

// A helper struct to hold file info
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
    perror(("ls: cannot access '" + path + "'").c_str());
    return;
  }

  std::vector<DirectoryEntry> entries;
  long total_blocks = 0;

  // Read all entries into a vector
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (!show_all && entry->d_name[0] == '.') {
      continue;
    }

    DirectoryEntry current_entry;
    current_entry.name = entry->d_name;

    std::string full_path = path + "/" + current_entry.name;
    if (stat(full_path.c_str(), &current_entry.stat_info) == 0) {
      entries.push_back(current_entry);
      total_blocks += current_entry.stat_info.st_blocks;
    }
  }
  closedir(dir);

  // Sort the vector alphabetically
  std::sort(entries.begin(), entries.end(),
            [](const DirectoryEntry& a, const DirectoryEntry& b) {
              return a.name < b.name;
            });

  if (long_format) {
    // Print the total block count, converted to kilobytes
    out << "total " << total_blocks / 2 << "\n";
  }

  for (const auto& ent : entries) {
    if (long_format) {
      print_long_format(path + "/" + ent.name, human_readable, out);
    }
    print_colored_name(ent.stat_info, ent.name, out);
    out << "\n";
  }
}

// ls command
void execute_ls(const std::vector<std::string>& args, std::ostream& out) {
  bool show_all = false;
  bool human_readable = false;
  bool long_format = false;
  std::vector<std::string> file_paths;
  std::vector<std::string> dir_paths;

  // parsing to find the flags and paths
  for (size_t i = 1; i < args.size(); i++) {
    if (args[i][0] == '-') {
      // its a flag
      for (char flag : args[i].substr(1)) {
        if (flag == 'l') {
          long_format = true;
        } else if (flag == 'a') {
          show_all = true;
        } else if (flag == 'h') {
          human_readable = true;
        }
      }
    } else {
      // It's a path, now let's check its type
      struct stat path_stat;
      if (stat(args[i].c_str(), &path_stat) == 0) {
        if (S_ISDIR(path_stat.st_mode)) {
          dir_paths.push_back(args[i]);
        } else {
          file_paths.push_back(args[i]);
        }
      } else {
        // Handle error: file or directory doesn't exist
        perror(("ls: cannot access '" + args[i] + "'").c_str());
      }
    }
  }

  // Only default to "." if *no* arguments (besides flags) were given
  bool only_flags = true;
  for (size_t i = 1; i < args.size(); i++) {
    if (args[i][0] != '-') {
      only_flags = false;
      break;
    }
  }

  if (only_flags && file_paths.empty() && dir_paths.empty()) {
    dir_paths.push_back(".");
  }

  // List all files first
  for (const auto& path : file_paths) {
    struct stat path_stat;
    if (stat(path.c_str(), &path_stat) == 0) {
      if (long_format) {
        print_long_format(path, human_readable, out);
      }
      print_colored_name(path_stat, path, out);
      out << "\n";
    }
  }

  // Add a newline as we printed files and are about to print directories
  if (!file_paths.empty() && !dir_paths.empty()) {
    out << "\n";
  }

  // List the contents of all directories next
  for (size_t i = 0; i < dir_paths.size(); ++i) {
    // If there's more than one path total, print the directory name
    if (file_paths.size() + dir_paths.size() > 1) {
      out << dir_paths[i] << ":" << "\n";
    }
    list_directory(dir_paths[i], show_all, long_format, human_readable, out);
    // Add a newline between directory listings
    if (i < dir_paths.size() - 1) {
      out << "\n";
    }
  }
}

void execute_clear(const std::vector<std::string>& args, std::ostream& out) {
  // The clear command ignores any arguments.
  // Write the ANSI escape code to the console.
  out << "\x1b[H\x1b[2J" << std::flush;
}

// helper for pinfo command
void print_process_info(pid_t pid, std::ostream& out) {
  std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
  FILE* stat_file = fopen(stat_path.c_str(), "r");

  if (stat_file == nullptr) {
    perror(("pinfo: process with pid " + std::to_string(pid) + " not found")
               .c_str());
    return;
  }

  char state;           // To hold the process state char (e.g., 'R', 'S', 'Z').
  int pgrp;             // To hold the process group ID.
  unsigned long vsize;  // To hold the virtual memory size in bytes.

  fscanf(stat_file,
         // Format String Breakdown (maps to fields in /proc/[pid]/stat):
         // '*' after a '%' means discard it.
         "%*d"                   // Field  1 (pid)
         " %*s"                  // Field  2 (comm)
         " %c"                   // Field  3 (state):  STORE it in state.
         " %*d"                  // Field  4 (ppid)
         " %d"                   // Field  5 (pgrp):   STORE it in pgrp.
         " %*d"                  // Field  6 (session)
         " %*d"                  // Field  7 (tty_nr)
         " %*d"                  // Field  8 (tpgid)
         " %*u %*u %*u %*u %*u"  // Fields 9-13
         " %*u %*u %*u %*u"      // Fields 14-17
         " %*d %*d %*d %*d"      // Fields 18-21
         " %*u"                  // Field 22 (starttime)
         " %lu",                 // Field 23 (vsize): STORE it in vsize.
         &state, &pgrp, &vsize);

  fclose(stat_file);

  // Get Executable Path from /proc/<pid>/exe
  std::string exe_path_str = "/proc/" + std::to_string(pid) + "/exe";
  char exe_path[PATH_MAX] = {0};
  ssize_t len = readlink(exe_path_str.c_str(), exe_path, sizeof(exe_path) - 1);
  if (len != -1) {
    exe_path[len] = '\0';
  } else {
    // Error reading link, might be a kernel process or permissions issue
    strcpy(exe_path, "path not accessible");
  }

  std::string state_str(1, state);
  switch (state) {
    case 'R':
      state_str = "Running";
      break;
    case 'S':
      state_str = "Sleeping";
      break;
    case 'D':
      state_str = "Disk Sleep";
      break;
    case 'Z':
      state_str = "Zombie";
      break;
    case 'T':
      state_str = "Stopped";
      break;
    default:
      state_str = "Unknown";
      break;
  }

  // Check if it's a foreground process
  pid_t terminal_pgrp = tcgetpgrp(STDIN_FILENO);
  if (pgrp == terminal_pgrp) {
    state_str += "+";
  }

  // Print the final output ---
  out << "pid -- " << pid << std::endl;
  out << "Process Status -- " << state << " {" << state_str << "}" << std::endl;
  out << "Memory -- " << vsize / 1024 << " KB {Virtual Memory}" << std::endl;
  out << "Executable Path -- " << exe_path << std::endl;
}

void execute_pinfo(const std::vector<std::string>& args, std::ostream& out) {
  if (args.size() > 2) {
    out << "pinfo: too many arguments" << std::endl;
    return;
  }

  pid_t target_pid;
  if (args.size() == 1) {
    // Case: "pinfo" -> show info for the shell itself
    target_pid = getpid();
  } else {
    // Case: "pinfo <pid>" -> show info for specified pid
    try {
      target_pid = std::stoi(args[1]);
    } catch (const std::exception& e) {
      out << "pinfo: invalid pid '" << args[1] << "'" << std::endl;
      return;
    }
  }
  print_process_info(target_pid, out);
}

// Recursive Helper Function for Search
bool recursive_search(const std::string& dir_path,
                      const std::string& target_name) {
  DIR* dir = opendir(dir_path.c_str());
  if (dir == nullptr) {
    return false;
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;

    // Case 1: The entry's name matches our target
    if (name == target_name) {
      closedir(dir);
      return true;
    }

    // Case 2: The entry is a subdirectory, so we recurse
    std::string full_path = dir_path + "/" + name;
    struct stat entry_stat;
    if (stat(full_path.c_str(), &entry_stat) == 0 &&
        S_ISDIR(entry_stat.st_mode)) {
      // Ignore '.' and '..' to prevent infinite loops
      if (name != "." && name != "..") {
        if (recursive_search(full_path, target_name)) {
          closedir(dir);
          return true;
        }
      }
    }
  }

  closedir(dir);
  return false;
}

// Search Command ---
void execute_search(const std::vector<std::string>& args,
                    std::istream& in,
                    std::ostream& out) {
  if (args.size() == 2) {
    // Case 1: Normal usage "search <name>"
    const std::string& target_name = args[1];
    // Start the search from the current directory "."
    bool found = recursive_search(".", target_name);
    out << (found ? "True" : "False") << std::endl;
  } else if (args.size() == 1) {
    // Case 2: Read names from stdin
    std::string target_name;
    while (std::getline(in, target_name)) {
      bool found = recursive_search(".", target_name);
      out << target_name << ": " << (found ? "True" : "False") << std::endl;
    }
  } else {
    out << "search: incorrect number of arguments (expected 0 or 1)"
        << std::endl;
  }
}

void execute_jobs(const std::vector<std::string>& args, std::ostream& out) {
  for (size_t i = 0; i < background_jobs.size(); ++i) {
    out << "[" << i + 1 << "] "
        << (background_jobs[i].is_stopped ? "Stopped" : "Running") << " "
        << background_jobs[i].command_name << " [" << background_jobs[i].pid
        << "]" << std::endl;
  }
}

void execute_exit(const std::vector<std::string>& args, std::ostream& out) {
  save_history_on_exit();
  exit(0);
}
