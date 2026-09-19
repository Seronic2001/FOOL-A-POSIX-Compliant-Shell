#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include "parser.h"

// A background job tracked by the shell.
struct Job {
  int id;                      // Stable, monotonically increasing job number.
  pid_t pgid;                  // Process group of the job.
  std::string command_name;    // Display name (argv[0] of first command).
  bool is_stopped;             // True after Ctrl+Z (job stopped).
  std::vector<pid_t> pids;     // PIDs still alive in this job.
};

// Info about the foreground job while it runs (used by the editor to forward
// Ctrl+C / Ctrl+Z). pgid == 0 means "no foreground job".
struct ForegroundJob {
  pid_t pgid = 0;
  std::string command_name;
};

// The shell's own process group (for handing the terminal back).
extern pid_t shell_pgid;

// The global list of background jobs and the current foreground job.
extern std::vector<Job> background_jobs;
extern ForegroundJob foreground_job;

// Executes a parsed line. Blocks until each foreground group finishes.
void execute(const std::vector<Command>& commands);

// Drains finished-child notifications and updates background_jobs /
// foreground bookkeeping. Safe to call from the main loop or the editor.
void reap_finished_children();

// Waits (synchronously, as the single reaper) for the given foreground
// children, handling Ctrl+Z (WUNTRACED) and terminal handoff.
void wait_for_foreground(const std::vector<pid_t>& pids,
                         const std::string& name);

// Sends SIGHUP+SIGCONT to all remaining background jobs (shell exit).
void terminate_background_jobs();
