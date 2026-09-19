#include "executor.h"

#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

#include "builtins.h"
#include "history.h"
#include "shell_signals.h"

pid_t shell_pgid = 0;
std::vector<Job> background_jobs;
ForegroundJob foreground_job;

static int next_job_id = 1;

namespace {

// ---------------- Glob expansion ----------------

bool has_glob_meta(const std::string& s) {
  return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

// Expands an argument to possibly MULTIPLE words (real glob behavior).
std::vector<std::string> expand_glob_arg(const std::string& arg,
                                         bool may_glob) {
  if (!may_glob || !has_glob_meta(arg)) {
    return {arg};
  }
  glob_t glob_result;
  memset(&glob_result, 0, sizeof(glob_result));
  int ret = glob(arg.c_str(), GLOB_TILDE, nullptr, &glob_result);
  std::vector<std::string> out;
  if (ret == 0 && glob_result.gl_pathc > 0) {
    for (size_t i = 0; i < glob_result.gl_pathc; i++) {
      out.push_back(glob_result.gl_pathv[i]);
    }
  } else {
    out.push_back(arg);  // No match: keep literal.
  }
  globfree(&glob_result);
  return out;
}

std::vector<std::string> expand_args_impl(const Command& command) {
  std::vector<std::string> expanded;
  for (size_t i = 0; i < command.args.size(); ++i) {
    bool may_glob = i < command.arg_is_globbed.size() && command.arg_is_globbed[i];
    auto words = expand_glob_arg(command.args[i], may_glob);
    expanded.insert(expanded.end(), words.begin(), words.end());
  }
  return expanded;
}

// ---------------- Built-in dispatch ----------------

// Built-ins that change shell state and therefore MUST run in the parent.
// They are invalid inside pipelines: POSIX gives them no defined pipe
// semantics for state changes, and silently forking them (as the old code
// did) made `echo hi | cd /tmp` a silent no-op.
bool is_stateful_builtin(const std::string& name) {
  return name == "cd" || name == "exit";
}

bool is_builtin(const std::string& name) {
  return name == "echo" || name == "pwd" || name == "cd" || name == "ls" ||
         name == "pinfo" || name == "history" || name == "jobs" ||
         name == "clear" || name == "exit" || name == "search";
}

// Runs a built-in with argv already glob-expanded. Returns its exit status.
int run_builtin(const Command& command, std::istream& in, std::ostream& out) {
  const std::vector<std::string> args = expand_args_impl(command);
  const std::string& cmd_name = args[0];

  if (cmd_name == "echo") return execute_echo(args, in, out);
  if (cmd_name == "pwd") return execute_pwd(args, out);
  if (cmd_name == "ls") return execute_ls(args, out);
  if (cmd_name == "pinfo") return execute_pinfo(args, out);
  if (cmd_name == "history") return execute_history(args, out);
  if (cmd_name == "jobs") return execute_jobs(args, out);
  if (cmd_name == "clear") return execute_clear(args, out);
  if (cmd_name == "search") return execute_search(args, in, out);
  return 0;
}

// Applies the command's redirections to the current process. Returns false
// (after printing an error) on failure.
bool apply_redirections(const Command& command) {
  if (!command.stdin_file.empty()) {
    int in_fd = open(command.stdin_file.c_str(), O_RDONLY);
    if (in_fd < 0) {
      perror(command.stdin_file.c_str());
      return false;
    }
    if (dup2(in_fd, STDIN_FILENO) < 0) {
      perror("dup2");
      close(in_fd);
      return false;
    }
    close(in_fd);
  }
  if (!command.stdout_file.empty()) {
    int out_flags =
        O_WRONLY | O_CREAT | (command.append_out ? O_APPEND : O_TRUNC);
    int out_fd = open(command.stdout_file.c_str(), out_flags, 0644);
    if (out_fd < 0) {
      perror(command.stdout_file.c_str());
      return false;
    }
    if (dup2(out_fd, STDOUT_FILENO) < 0) {
      perror("dup2");
      close(out_fd);
      return false;
    }
    close(out_fd);
  }
  if (!command.stderr_file.empty()) {
    int err_flags =
        O_WRONLY | O_CREAT | (command.append_err ? O_APPEND : O_TRUNC);
    int err_fd = open(command.stderr_file.c_str(), err_flags, 0644);
    if (err_fd < 0) {
      perror(command.stderr_file.c_str());
      return false;
    }
    if (dup2(err_fd, STDERR_FILENO) < 0) {
      perror("dup2");
      close(err_fd);
      return false;
    }
    close(err_fd);
  }
  return true;
}

// Runs a single command inside an already-forked child. Applies
// redirections, then either execs or runs a non-stateful builtin. Never
// returns.
[[noreturn]] void child_exec(const Command& command) {
  // Restore default signal dispositions for the child.
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  signal(SIGTTIN, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);
  signal(SIGCHLD, SIG_DFL);

  if (!apply_redirections(command)) {
    _exit(1);
  }

  if (command.args.empty()) {
    _exit(0);
  }

  const std::string cmd_name = command.args[0];

  if (is_stateful_builtin(cmd_name)) {
    // Stateful builtins must never run here (see is_stateful_builtin).
    std::cerr << cmd_name << ": no job control in pipeline" << std::endl;
    _exit(1);
  }

  if (is_builtin(cmd_name)) {
    int rc = run_builtin(command, std::cin, std::cout);
    // Flush before _exit(): _exit() skips C++ stream destruction, so any
    // buffered builtin output would be silently lost.
    std::cout.flush();
    std::cerr.flush();
    _exit(rc);
  }

  // System command: expand globs, build argv, exec.
  std::vector<std::string> expanded_args = expand_args_impl(command);
  std::vector<char*> argv;
  for (const auto& arg : expanded_args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  execvp(argv[0], argv.data());
  // execvp only returns on failure.
  perror(argv[0]);
  _exit(127);  // 127 is the conventional "command not found" status.
}

// Prints a job-state change notice for a background job.
void print_job_status(const Job& job, const std::string& status,
                      const std::string& detail) {
  std::cout << "\r\x1b[K[" << job.id << "] " << status << " "
            << job.command_name << " " << detail << std::endl;
}

// Removes pid from job's live list; returns true if list became empty.
bool remove_pid(Job& job, pid_t pid) {
  job.pids.erase(std::remove(job.pids.begin(), job.pids.end(), pid),
                 job.pids.end());
  return job.pids.empty();
}

struct GroupResult {
  bool should_exit = false;  // A top-level `exit` builtin ran.
};

GroupResult run_command_group(const std::vector<Command>& group);

// ---------------- Single-command fast path ----------------

GroupResult run_single_command(const Command& command) {
  GroupResult result;

  // Top-level stateful builtins run in the parent, with redirections applied
  // to the SHELL's own stdio (and restored afterwards).
  if (is_stateful_builtin(command.args[0])) {
    // Flush pending C++ stream data BEFORE re-pointing the underlying fds,
    // so it lands on the original destination, not the redirect target.
    std::cout.flush();
    std::cerr.flush();
    int saved_in = -1, saved_out = -1, saved_err = -1;
    if (!command.stdin_file.empty()) {
      saved_in = dup(STDIN_FILENO);
      int fd = open(command.stdin_file.c_str(), O_RDONLY);
      if (fd < 0) {
        perror(command.stdin_file.c_str());
        return result;
      }
      dup2(fd, STDIN_FILENO);
      close(fd);
    }
    if (!command.stdout_file.empty()) {
      saved_out = dup(STDOUT_FILENO);
      int flags =
          O_WRONLY | O_CREAT | (command.append_out ? O_APPEND : O_TRUNC);
      int fd = open(command.stdout_file.c_str(), flags, 0644);
      if (fd < 0) {
        perror(command.stdout_file.c_str());
        if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
        return result;
      }
      dup2(fd, STDOUT_FILENO);
      close(fd);
    }
    if (!command.stderr_file.empty()) {
      saved_err = dup(STDERR_FILENO);
      int flags =
          O_WRONLY | O_CREAT | (command.append_err ? O_APPEND : O_TRUNC);
      int fd = open(command.stderr_file.c_str(), flags, 0644);
      if (fd < 0) {
        perror(command.stderr_file.c_str());
        if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
        if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        return result;
      }
      dup2(fd, STDERR_FILENO);
      close(fd);
    }

    if (command.args[0] == "cd") {
      execute_cd(expand_args_impl(command), std::cout);
    } else {  // exit
      std::cout.flush();
      save_history_on_exit();
      terminate_background_jobs();
      std::cout.flush();
      _exit(0);
    }
    std::cout.flush();

    // Restore the shell's stdio.
    if (saved_in >= 0) { dup2(saved_in, STDIN_FILENO); close(saved_in); }
    if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
    if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
    return result;
  }

  // Everything else runs as a one-child "pipeline".
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return result;
  }
  if (pid == 0) {
    setpgid(0, 0);
    child_exec(command);
  }
  setpgid(pid, pid);  // Parent-side setpgid wins the race.
  if (command.background) {
    Job job;
    job.id = next_job_id++;
    job.pgid = pid;
    job.command_name = command.args[0];
    job.is_stopped = false;
    job.pids = {pid};
    std::cout << "[" << job.id << "] " << job.pgid << std::endl;
    background_jobs.push_back(std::move(job));
    return result;
  }
  wait_for_foreground({pid}, command.args[0]);
  return result;
}

GroupResult run_pipeline_group(const std::vector<Command>& group) {
  GroupResult result;
  const size_t n = group.size();

  // A whole pipeline of a stateful builtin is invalid (state would be lost).
  if (n > 1 && (is_stateful_builtin(group[0].args[0]) ||
                is_stateful_builtin(group[n - 1].args[0]))) {
    std::cerr << group[0].args[0] << ": cannot be used in a pipeline"
              << std::endl;
    return result;
  }
  // Single stateful builtin reaching the pipeline path (backgrounded).
  if (n == 1 && is_stateful_builtin(group[0].args[0])) {
    return run_single_command(group[0]);
  }

  int in_fd = STDIN_FILENO;
  std::vector<pid_t> child_pids;
  pid_t pgid = 0;

  for (size_t j = 0; j < n; ++j) {
    int pipefd[2] = {-1, -1};
    if (j < n - 1) {
      if (pipe(pipefd) < 0) {
        perror("pipe");
        break;
      }
    }

    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
      break;
    }

    if (pid == 0) {
      pgid = (j == 0) ? getpid() : pgid;
      setpgid(0, pgid);
      if (j > 0) {
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
      }
      if (j < n - 1) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
      }
      child_exec(group[j]);
    }

    child_pids.push_back(pid);
    pgid = (j == 0) ? pid : pgid;
    setpgid(pid, pgid);

    if (j > 0 && in_fd != STDIN_FILENO) close(in_fd);
    if (j < n - 1) {
      close(pipefd[1]);
      in_fd = pipefd[0];
    }
  }

  if (in_fd != STDIN_FILENO) close(in_fd);

  if (child_pids.empty()) return result;

  if (group.back().background) {
    Job job;
    job.id = next_job_id++;
    job.pgid = pgid;
    job.command_name = group[0].args[0];
    job.is_stopped = false;
    job.pids = child_pids;
    std::cout << "[" << job.id << "] " << job.pgid << std::endl;
    background_jobs.push_back(std::move(job));
  } else {
    wait_for_foreground(child_pids, group[0].args[0]);
    // If the job was stopped (moved to background by wait_for_foreground),
    // the remaining children of the group must be accounted for too.
    // wait_for_foreground handles the whole pgrp via SIGCHLD events.
  }
  return result;
}

GroupResult run_command_group(const std::vector<Command>& group) {
  if (group.size() == 1) return run_single_command(group[0]);
  return run_pipeline_group(group);
}

}  // namespace

// ---------------- Foreground waiting (single reaper) ----------------

void wait_for_foreground(const std::vector<pid_t>& pids,
                         const std::string& name) {
  std::vector<pid_t> remaining = pids;
  const pid_t pgid = getpgid(pids[0]);

  // Own the terminal while the job runs.
  // Block SIGCHLD while we set up: no handler can run mid-setup, so no
  // wake byte can be missed before we start waitpid()ing.
  sigset_t block_chld, old_mask;
  sigemptyset(&block_chld);
  sigaddset(&block_chld, SIGCHLD);
  sigprocmask(SIG_BLOCK, &block_chld, &old_mask);
  drain_child_events();  // Discard stale wake bytes from earlier children.
  tcsetpgrp(STDIN_FILENO, pgid);
  foreground_job.pgid = pgid;
  foreground_job.command_name = name;

  // All reaping happens here on the main thread; the handler only writes
  // wake bytes. SIGCHLD stays blocked for the whole wait so no signal can
  // interrupt the waitpid() loop.
  while (!remaining.empty()) {
    int status;
    pid_t r = waitpid(-pgid, &status, WUNTRACED);
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno == ECHILD) break;  // No children left in this group.
      perror("waitpid");
      break;
    }
    if (r == 0) continue;

    if (WIFSTOPPED(status)) {
      std::cout << "\r\x1b[KStopped: " << name << std::endl;
      Job job;
      job.id = next_job_id++;
      job.pgid = pgid;
      job.command_name = name;
      job.is_stopped = true;
      job.pids = remaining;  // Everything still alive belongs to the job.
      background_jobs.push_back(std::move(job));
      remaining.clear();
      break;
    }

    remaining.erase(std::remove(remaining.begin(), remaining.end(), r),
                    remaining.end());
  }

  // Discard any pending wake bytes so they don't mislead the next wait.
  // Background job completion is handled by reap_finished_children().
  drain_child_events();

  foreground_job.pgid = 0;
  foreground_job.command_name.clear();
  // Take the terminal back, then unblock SIGCHLD.
  tcsetpgrp(STDIN_FILENO, shell_pgid);
  sigprocmask(SIG_SETMASK, &old_mask, nullptr);
}

void reap_finished_children() {
  // A pending wake byte means at least one child may have changed state.
  // Sweep each known background job by PID; foreground children, if any, are
  // owned by wait_for_foreground() and are never touched here.
  drain_child_events();
  for (auto it = background_jobs.begin(); it != background_jobs.end();) {
    Job& job = *it;
    const std::vector<pid_t> pids_snapshot = job.pids;
    for (pid_t pid : pids_snapshot) {
      int status = 0;
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r <= 0) continue;
      if (remove_pid(job, r)) {
        if (WIFEXITED(status)) {
          print_job_status(job, "Done",
                           "(Exited with status " +
                               std::to_string(WEXITSTATUS(status)) + ")");
        } else if (WIFSIGNALED(status)) {
          print_job_status(job, "Done",
                           "(Killed by signal " +
                               std::to_string(WTERMSIG(status)) + ")");
        }
      }
    }
    it = job.pids.empty() ? background_jobs.erase(it) : std::next(it);
  }
}

void terminate_background_jobs() {
  for (auto& job : background_jobs) {
    killpg(job.pgid, SIGHUP);
  }
  // Give them a moment, then force.
  for (auto& job : background_jobs) {
    killpg(job.pgid, SIGCONT);  // Allow delivery of SIGHUP to stopped jobs.
  }
  background_jobs.clear();
}

void execute(const std::vector<Command>& commands) {
  std::vector<Command> group;
  for (const auto& cmd : commands) {
    if (cmd.is_group_end) {
      if (!group.empty()) {
        run_command_group(group);
        group.clear();
      }
      continue;
    }
    if (cmd.args.empty()) continue;
    group.push_back(cmd);
  }
  if (!group.empty()) {
    run_command_group(group);
  }
}
