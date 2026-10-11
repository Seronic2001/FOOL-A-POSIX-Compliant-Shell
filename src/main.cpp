#include <limits.h>
#include <signal.h>
#include <unistd.h>

#include <iostream>

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

  shell_interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

  // Job control is for interactive use only. Reading a script, the shell
  // stays in its parent's process group with default signal handling, so
  // Ctrl+C interrupts it and its children like any other program.
  if (shell_interactive) {
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    setpgid(0, 0);  // Fails harmlessly if we already lead a group.
    shell_pgid = getpgrp();
    tcsetpgrp(STDIN_FILENO, shell_pgid);
  }

  setup_signal_handlers(shell_interactive);
  initialize_history();

  while (!shell_exit_requested) {
    // Empty on a blank line, Ctrl+C, or end of input (which also requests
    // exit).
    std::string input_line = read_command_line();
    if (input_line.empty()) continue;

    add_to_history(input_line);

    ParseResult parsed = parse_input(input_line);
    if (!parsed.ok()) {
      std::cerr << "FOOL: " << parsed.error << std::endl;
      continue;
    }

    reap_finished_children();  // Fresh job table before running.
    execute(parsed.pipelines);
    reap_finished_children();
  }

  terminate_background_jobs();
  save_history_on_exit();
  return shell_exit_status;
}
