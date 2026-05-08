#include <sys/wait.h>

#include <algorithm>

#include "executor.h"
#include "shell_signals.h"

std::atomic<bool> got_sigint(false);

// Handler for SIGINT (Ctrl+C)
void sigint_handler(int signo) {
  (void)signo;
  got_sigint = true;
  // The string "\n^C" is 3 bytes.
  write(STDOUT_FILENO, "\n^C", 3);
}

void sigchld_handler(int signo) {
  (void)signo;
  int saved_errno = errno;
  pid_t child_pid;
  int status;

  while ((child_pid = waitpid(-1, &status, WNOHANG)) > 0) {
    char msg[256];
    int len = 0;

    for (const auto& job : background_jobs) {
      if (job.pid == child_pid) {
        // snprintf and write are async-signal-safe
        // 1. \r       -> Move cursor to start of the line
        // 2. \x1b[K   -> Erase from cursor to end of line
        // 3. Print the message
        // 4. \n       -> Move to the next line for the new prompt
        if (WIFEXITED(status)) {
          len = snprintf(msg, sizeof(msg),
                         "\r\x1b[K[%d] Done '%s' (Exited with status %d)\n",
                         child_pid, job.command_name.c_str(),
                         WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
          len = snprintf(msg, sizeof(msg),
                         "\r\x1b[K[%d] Done '%s' (Killed by signal %d)\n",
                         child_pid, job.command_name.c_str(), WTERMSIG(status));
        }
        write(STDERR_FILENO, msg, len);
        break;
      }
    }

    background_jobs.erase(
        std::remove_if(
            background_jobs.begin(), background_jobs.end(),
            [child_pid](const Job& job) { return job.pid == child_pid; }),
        background_jobs.end());
  }
  errno = saved_errno;
}
