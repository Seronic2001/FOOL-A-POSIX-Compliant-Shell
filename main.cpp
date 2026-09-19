#include <limits.h>
#include <signal.h>
#include <unistd.h>

#include <iostream>

#include "autocomplete.h"
#include "executor.h"
#include "history.h"
#include "line_editor.h"
#include "parser.h"
#include "prompt.h"
#include "shell_signals.h"

int main() {
  char path_buffer[PATH_MAX];
  if (getcwd(path_buffer, sizeof(path_buffer)) != nullptr) {
    shell_home_dir = path_buffer;
  } else {
    perror("Failed to get initial directory");
    return 1;
  }

  // Ignore job-control signals in the shell process itself.
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);

  pid_t shell_pid = getpid();
  if (setpgid(shell_pid, shell_pid) < 0) {
    // Failing is fine when we are already a group leader (e.g. launched by a
    // script); don't abort the shell for it.
    if (errno != EPERM && errno != EACCES) {
      perror("Couldn't put the shell in its own process group");
      return 1;
    }
  }
  shell_pgid = getpgrp();

  if (tcsetpgrp(STDIN_FILENO, shell_pgid) != 0) {
    // Non-interactive or already-foreground launches can fail here; the
    // shell still works for batch input.
  }

  setup_signal_handlers();

  initialize_command_cache();
  initialize_history();

  while (!shell_exit_requested) {
    std::string input_line = read_command_line();
    if (shell_exit_requested) break;

    if (got_sigint) {
      got_sigint = false;
      continue;  // Discard the line and redraw a fresh prompt.
    }

    if (input_line.empty()) continue;

    add_to_history(input_line);

    ParseResult parsed = parse_input(input_line);
    if (!parsed.ok()) {
      std::cerr << "FOOL: " << parsed.error << std::endl;
      continue;
    }

    reap_finished_children();  // Fresh job table before running.
    execute(parsed.commands);
    reap_finished_children();
  }

  terminate_background_jobs();
  save_history_on_exit();
  return 0;
}
