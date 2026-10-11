#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include "parser.h"

// A job tracked by the shell: running in the background, or stopped.
struct Job {
  int id = 0;                // Job number; 0 until the job enters the table.
  pid_t pgid = 0;            // Process group (pid of the first process).
  std::string command_name;  // Display name (argv[0] of first command).
  bool is_stopped = false;   // True after Ctrl+Z (job stopped).
  std::vector<pid_t> pids;   // PIDs still alive in this job.
};

// True when stdin and stdout are a terminal. Only then does the shell do
// job control (own process groups, terminal handoff) and line editing.
extern bool shell_interactive;

// The shell's own process group (for handing the terminal back).
extern pid_t shell_pgid;

// The job table.
extern std::vector<Job> background_jobs;

// Executes a parsed line. Blocks until each foreground pipeline finishes.
void execute(const std::vector<Pipeline>& pipelines);

// Reaps finished background children and updates background_jobs. Returns
// true if a job notice was printed.
bool reap_finished_children();

// `fg` / `bg`: resume a job (default: the most recent) in the foreground
// or background. Stateful, so they run in the shell process.
int execute_fg(const std::vector<std::string>& args);
int execute_bg(const std::vector<std::string>& args);

// Sends SIGHUP+SIGCONT to all remaining jobs (shell exit).
void terminate_background_jobs();
