#include <limits.h>  // For PATH_MAX
#include <signal.h>  // For signal()
#include <unistd.h>  // For getcwd, STDIN_FILENO, getpid, setpgid

#include <iostream>

#include "autocomplete.h"
#include "executor.h"
#include "history.h"
#include "line_editor.h"
#include "parser.h"
#include "shell_signals.h"

// Define a global variable to hold the shell's "HOME" path.
std::string shell_home_dir;

int main() {
  // set current path as "HOME"
  char path_buffer[PATH_MAX];
  if (getcwd(path_buffer, PATH_MAX) != nullptr) {
    shell_home_dir = path_buffer;
  } else {
    perror("Failed to get initial directory");
    return 1;
  }

  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  signal(SIGINT, sigint_handler);  // Ignore Ctrl+C in the shell
  signal(SIGTSTP, SIG_IGN);        // Ignore Ctrl+Z in the shell

  pid_t shell_pid = getpid();

  // Put the shell in its own process group.
  if (setpgid(shell_pid, shell_pid) < 0) {
    perror("Couldn't put the shell in its own process group");
    return 1;
  }

  // Take control of the terminal.
  tcsetpgrp(STDIN_FILENO, shell_pid);

  struct sigaction sa;
  sa.sa_handler = &sigchld_handler;  // Set the handler function
  sigemptyset(&sa.sa_mask);          // Clear the mask

  // Do NOT set the SA_RESTART flag. This ensures that system
  // calls like read() will be interrupted by the signal.
  sa.sa_flags = 0;

  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("sigaction");
    return 1;
  }

  initialize_command_cache();
  initialize_history();

  while (true) {
    std::string input_line = read_command_line();

    if (got_sigint) {
      got_sigint = false;
      continue;  // redraw prompt cleanly, discarding current input
    }

    if (std::cin.eof()) {  // Handle Ctrl+D
      break;
    }

    add_to_history(input_line);

    // Parse the input
    std::vector<Command> parsed_commands = parse_input(input_line);

    execute(parsed_commands);
  }

  save_history_on_exit();
  return 0;
}