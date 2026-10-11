#include "executor.h"

#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

#include "builtins.h"
#include "history.h"
#include "shell_signals.h"

bool shell_interactive = false;
pid_t shell_pgid = 0;
std::vector<Job> background_jobs;

static int next_job_id = 1;

namespace {

// ---------------- Word expansion ----------------

// Applies tilde expansion ("~" and "~/...") to the word's literal text and,
// when given, to its glob pattern.
std::string expand_tilde(const Word& word, std::string* pattern = nullptr) {
  if (pattern != nullptr) *pattern = word.pattern;
  const char* home = getenv("HOME");
  if (!word.tilde || home == nullptr || home[0] == '\0' ||
      (word.text.size() > 1 && word.text[1] != '/')) {
    return word.text;
  }
  if (pattern != nullptr && !pattern->empty()) {
    *pattern = glob_escape(home) + pattern->substr(1);
  }
  return home + word.text.substr(1);
}

// Expands a word to possibly MULTIPLE arguments (real glob behavior). A
// pattern that matches nothing stays literal.
std::vector<std::string> expand_word(const Word& word) {
  std::string pattern;
  std::string text = expand_tilde(word, &pattern);
  if (pattern.empty()) return {text};

  std::vector<std::string> out;
  glob_t matches = {};
  if (glob(pattern.c_str(), 0, nullptr, &matches) == 0) {
    out.assign(matches.gl_pathv, matches.gl_pathv + matches.gl_pathc);
  }
  globfree(&matches);
  if (out.empty()) out.push_back(std::move(text));
  return out;
}

std::vector<std::string> expand_argv(const Command& command) {
  std::vector<std::string> argv;
  for (const Word& word : command.words) {
    auto expanded = expand_word(word);
    argv.insert(argv.end(), expanded.begin(), expanded.end());
  }
  return argv;
}

// ---------------- Redirections ----------------

// (descriptor, saved copy or -1 if it was closed) pairs for undoing
// redirections applied to the shell's own descriptors.
using SavedFds = std::vector<std::pair<int, int>>;

// Applies the command's redirections, in order, to the current process.
// With `saved`, records how to undo them. Returns false (after printing an
// error) on failure; anything already recorded must still be restored.
bool apply_redirections(const Command& command, SavedFds* saved = nullptr) {
  for (const Redirect& r : command.redirects) {
    int source = r.dup_fd;
    if (r.kind != Redirect::kDup) {
      const std::string path = expand_tilde(r.target);
      int flags = O_RDONLY;
      if (r.kind == Redirect::kOut) flags = O_WRONLY | O_CREAT | O_TRUNC;
      if (r.kind == Redirect::kAppend) flags = O_WRONLY | O_CREAT | O_APPEND;
      source = open(path.c_str(), flags, 0644);
      if (source < 0) {
        perror(path.c_str());
        return false;
      }
    }
    if (source == r.fd) continue;

    if (saved != nullptr) {
      saved->emplace_back(r.fd, fcntl(r.fd, F_DUPFD_CLOEXEC, 10));
    }
    const bool ok = dup2(source, r.fd) >= 0;
    if (!ok) perror(std::to_string(source).c_str());
    if (r.kind != Redirect::kDup) close(source);
    if (!ok) return false;
  }
  return true;
}

void restore_fds(const SavedFds& saved) {
  for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
    if (it->second >= 0) {
      dup2(it->second, it->first);
      close(it->second);
    } else {
      close(it->first);
    }
  }
}

// ---------------- Built-in dispatch ----------------

// Built-ins that change shell state and therefore MUST run in the shell
// process. They are invalid inside pipelines, where they would run in a
// forked child and silently do nothing.
bool is_stateful_builtin(const std::string& name) {
  return name == "cd" || name == "exit" || name == "fg" || name == "bg";
}

bool is_builtin(const std::vector<std::string>& argv) {
  const auto& names = builtin_names();
  if (std::find(names.begin(), names.end(), argv[0]) == names.end()) {
    return false;
  }
  // Options the built-in `ls` doesn't implement go to the system `ls`.
  return argv[0] != "ls" || ls_supports(argv);
}

int execute_exit(const std::vector<std::string>& argv) {
  int status = 0;
  if (argv.size() > 1) {
    char* end = nullptr;
    long value = std::strtol(argv[1].c_str(), &end, 10);
    if (argv[1].empty() || *end != '\0') {
      std::cerr << "exit: " << argv[1] << ": numeric argument required"
                << std::endl;
      status = 2;
    } else {
      status = (int)(value & 0xFF);
    }
  }
  shell_exit_status = status;
  shell_exit_requested = true;  // main() does the cleanup.
  return status;
}

int run_builtin(const std::vector<std::string>& argv) {
  const std::string& name = argv[0];
  if (name == "cd") return execute_cd(argv, std::cout);
  if (name == "exit") return execute_exit(argv);
  if (name == "fg") return execute_fg(argv);
  if (name == "bg") return execute_bg(argv);
  if (name == "echo") return execute_echo(argv, std::cin, std::cout);
  if (name == "pwd") return execute_pwd(argv, std::cout);
  if (name == "ls") return execute_ls(argv, std::cout);
  if (name == "pinfo") return execute_pinfo(argv, std::cout);
  if (name == "history") return execute_history(argv, std::cout);
  if (name == "jobs") return execute_jobs(argv, std::cout);
  if (name == "clear") return execute_clear(argv, std::cout);
  if (name == "search") return execute_search(argv, std::cin, std::cout);
  return 0;
}

// Runs a builtin (or a bare redirection such as `> file`) in the shell
// process, with redirections applied to the shell's own stdio and undone
// afterwards.
void run_in_shell(const Command& command,
                  const std::vector<std::string>& argv) {
  // Flush pending output BEFORE re-pointing the underlying fds, so it lands
  // on the original destination, not the redirect target.
  std::cout.flush();
  SavedFds saved;
  if (apply_redirections(command, &saved) && !argv.empty()) {
    run_builtin(argv);
  }
  std::cout.flush();
  // A failed write (say, to a full disk) must not leave the stream in an
  // error state that swallows all later output.
  std::cout.clear();
  restore_fds(saved);
}

// ---------------- Child processes ----------------

// Runs one command inside an already-forked child. Never returns.
[[noreturn]] void run_child(const Command& command,
                            const std::vector<std::string>& args) {
  for (int signo : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD}) {
    signal(signo, SIG_DFL);
  }

  if (!apply_redirections(command)) _exit(1);
  if (args.empty()) _exit(0);

  if (is_builtin(args)) {
    int rc = run_builtin(args);
    // Flush before _exit(): _exit() skips C++ stream destruction, so any
    // buffered builtin output would be silently lost.
    std::cout.flush();
    _exit(rc);
  }

  std::vector<char*> argv;
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  execvp(argv[0], argv.data());
  // execvp only returns on failure.
  perror(argv[0]);
  _exit(127);  // 127 is the conventional "command not found" status.
}

// ---------------- Jobs ----------------

// Prints a job-state notice. At the prompt the terminal is in raw mode, so
// the line is cleared first and ended with an explicit carriage return.
void print_notice(const std::string& text) {
  if (shell_interactive) {
    std::cout << "\r\x1b[K" << text << "\r\n" << std::flush;
  } else {
    std::cout << text << std::endl;
  }
}

// Removes pid from job's live list; returns true if list became empty.
bool remove_pid(Job& job, pid_t pid) {
  job.pids.erase(std::remove(job.pids.begin(), job.pids.end(), pid),
                 job.pids.end());
  return job.pids.empty();
}

void signal_job(const Job& job, int signo) {
  if (shell_interactive) {
    killpg(job.pgid, signo);
  } else {
    // Without job control the children share the shell's process group.
    for (pid_t pid : job.pids) kill(pid, signo);
  }
}

// Waits for a foreground job, giving it the terminal while it runs. A job
// stopped by Ctrl+Z moves to the job table. `resume` continues a stopped
// job once it owns the terminal (`fg`).
void wait_for_foreground(Job job, bool resume = false) {
  if (shell_interactive) tcsetpgrp(STDIN_FILENO, job.pgid);
  if (resume) signal_job(job, SIGCONT);

  bool stopped = false;
  while (!job.pids.empty()) {
    int status;
    pid_t r = shell_interactive
                  ? waitpid(-job.pgid, &status, WUNTRACED)
                  : waitpid(job.pids.front(), &status, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno != ECHILD) perror("waitpid");
      break;
    }
    if (WIFSTOPPED(status)) {
      stopped = true;
      break;
    }
    remove_pid(job, r);
  }

  if (shell_interactive) tcsetpgrp(STDIN_FILENO, shell_pgid);

  if (stopped) {
    job.is_stopped = true;
    if (job.id == 0) job.id = next_job_id++;
    print_notice("[" + std::to_string(job.id) + "] Stopped " +
                 job.command_name);
    background_jobs.push_back(std::move(job));
  }
}

// Forks one child per command, wired together with pipes, then waits for
// the job or registers it as a background job.
void launch_pipeline(const Pipeline& pipeline,
                     const std::vector<std::vector<std::string>>& argvs) {
  const size_t n = pipeline.commands.size();

  // Children inherit a copy of any buffered output; flush so it is not
  // written twice.
  std::cout.flush();

  Job job;
  job.command_name = argvs[0].empty() ? "" : argvs[0][0];
  int in_fd = -1;  // Read end of the previous stage's pipe.

  for (size_t j = 0; j < n; ++j) {
    int pipefd[2] = {-1, -1};
    if (j < n - 1 && pipe(pipefd) < 0) {
      perror("pipe");
      break;
    }

    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
      break;
    }

    if (pid == 0) {
      if (shell_interactive) {
        // Both parent and child set the group and the terminal owner, so
        // neither can race ahead of the other.
        pid_t pgid = job.pgid != 0 ? job.pgid : getpid();
        setpgid(0, pgid);
        if (!pipeline.background) tcsetpgrp(STDIN_FILENO, pgid);
      }
      if (in_fd >= 0) {
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
      } else if (pipeline.background && !shell_interactive) {
        // Without job control nothing stops a background job from eating
        // the script on stdin; detach it like other shells do.
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
          dup2(null_fd, STDIN_FILENO);
          close(null_fd);
        }
      }
      if (pipefd[1] >= 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
      }
      run_child(pipeline.commands[j], argvs[j]);
    }

    job.pids.push_back(pid);
    if (job.pgid == 0) job.pgid = pid;
    if (shell_interactive) setpgid(pid, job.pgid);

    if (in_fd >= 0) close(in_fd);
    if (pipefd[1] >= 0) close(pipefd[1]);
    in_fd = pipefd[0];
  }

  if (in_fd >= 0) close(in_fd);
  if (job.pids.empty()) return;

  if (pipeline.background) {
    job.id = next_job_id++;
    std::cout << "[" << job.id << "] " << job.pgid << std::endl;
    background_jobs.push_back(std::move(job));
  } else {
    wait_for_foreground(std::move(job));
  }
}

void run_pipeline(const Pipeline& pipeline) {
  std::vector<std::vector<std::string>> argvs;
  for (const Command& command : pipeline.commands) {
    argvs.push_back(expand_argv(command));
  }

  if (argvs.size() > 1) {
    for (const auto& argv : argvs) {
      if (!argv.empty() && is_stateful_builtin(argv[0])) {
        std::cerr << argv[0] << ": cannot be used in a pipeline" << std::endl;
        return;
      }
    }
    launch_pipeline(pipeline, argvs);
    return;
  }

  // A lone builtin runs in the shell itself: no fork needed. Exceptions
  // are a background builtin and one that reads stdin (the shell's own
  // buffered script input must not be consumed by it).
  const auto& argv = argvs[0];
  const bool reads_stdin = !argv.empty() && argv[0] == "search" &&
                           argv.size() == 1;
  if (argv.empty() || is_stateful_builtin(argv[0]) ||
      (is_builtin(argv) && !pipeline.background && !reads_stdin)) {
    run_in_shell(pipeline.commands[0], argv);
  } else {
    launch_pipeline(pipeline, argvs);
  }
}

// Finds the job named by `fg`/`bg` arguments ("%2", "2", or none for the
// most recent). Prints an error and returns end() if there is none.
std::vector<Job>::iterator find_job(const std::vector<std::string>& args) {
  auto it = background_jobs.end();
  if (args.size() < 2) {
    if (!background_jobs.empty()) return std::prev(it);
    std::cerr << args[0] << ": no current job" << std::endl;
    return it;
  }
  const std::string& spec = args[1];
  const int id = std::atoi(spec.c_str() + (spec[0] == '%' ? 1 : 0));
  it = std::find_if(background_jobs.begin(), background_jobs.end(),
                    [id](const Job& job) { return job.id == id; });
  if (it == background_jobs.end()) {
    std::cerr << args[0] << ": " << spec << ": no such job" << std::endl;
  }
  return it;
}

}  // namespace

int execute_fg(const std::vector<std::string>& args) {
  if (!shell_interactive) {
    std::cerr << "fg: no job control" << std::endl;
    return 1;
  }
  auto it = find_job(args);
  if (it == background_jobs.end()) return 1;

  Job job = std::move(*it);
  background_jobs.erase(it);
  job.is_stopped = false;
  std::cout << job.command_name << std::endl;
  wait_for_foreground(std::move(job), true);
  return 0;
}

int execute_bg(const std::vector<std::string>& args) {
  if (!shell_interactive) {
    std::cerr << "bg: no job control" << std::endl;
    return 1;
  }
  auto it = find_job(args);
  if (it == background_jobs.end()) return 1;

  it->is_stopped = false;
  signal_job(*it, SIGCONT);
  std::cout << "[" << it->id << "] " << it->command_name << " &" << std::endl;
  return 0;
}

bool reap_finished_children() {
  // Sweep each known job by PID. Foreground children are owned by
  // wait_for_foreground() and are never in the table while it runs.
  drain_signal_events();
  bool printed = false;
  for (auto it = background_jobs.begin(); it != background_jobs.end();) {
    Job& job = *it;
    const std::vector<pid_t> pids_snapshot = job.pids;
    for (pid_t pid : pids_snapshot) {
      int status = 0;
      pid_t r = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
      if (r == 0) continue;
      if (r < 0) {
        if (errno == ECHILD) remove_pid(job, pid);  // Not ours any more.
        continue;
      }
      if (WIFSTOPPED(status)) {
        job.is_stopped = true;
      } else if (WIFCONTINUED(status)) {
        job.is_stopped = false;
      } else if (remove_pid(job, r)) {
        const std::string detail =
            WIFEXITED(status)
                ? "(Exited with status " +
                      std::to_string(WEXITSTATUS(status)) + ")"
                : "(Killed by signal " + std::to_string(WTERMSIG(status)) +
                      ")";
        print_notice("[" + std::to_string(job.id) + "] Done " +
                     job.command_name + " " + detail);
        printed = true;
      }
    }
    it = job.pids.empty() ? background_jobs.erase(it) : std::next(it);
  }
  return printed;
}

void terminate_background_jobs() {
  for (const Job& job : background_jobs) {
    signal_job(job, SIGHUP);
    signal_job(job, SIGCONT);  // Let stopped jobs receive the SIGHUP.
  }
  background_jobs.clear();
}

void execute(const std::vector<Pipeline>& pipelines) {
  for (const Pipeline& pipeline : pipelines) {
    if (shell_exit_requested) break;  // `exit; echo unreachable`
    run_pipeline(pipeline);
  }
}
