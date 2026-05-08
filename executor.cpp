#include <fcntl.h>  // For open()
#include <glob.h>
#include <signal.h>
#include <sys/wait.h>  // For waitpid
#include <unistd.h>    // For pipe, dup2, close, fork

#include <iostream>

#include "builtins.h"
#include "executor.h"

// Support for glob strings
std::vector<std::string> expand_globs(const std::vector<std::string>& args) {
  std::vector<std::string> expanded;

  for (const auto& arg : args) {
    glob_t glob_result;
    int ret = glob(arg.c_str(), GLOB_TILDE, nullptr, &glob_result);
    if (ret == 0) {
      for (size_t i = 0; i < glob_result.gl_pathc; i++) {
        expanded.push_back(glob_result.gl_pathv[i]);
      }
    } else {
      // If no match, keep arg same.
      expanded.push_back(arg);
    }
    globfree(&glob_result);
  }

  return expanded;
}

pid_t foreground_pid = 0;
std::vector<Job> background_jobs;

void execute_single_command(const Command& command) {
  // I/O Redirection
  if (!command.stdin_file.empty()) {
    int in_fd = open(command.stdin_file.c_str(), O_RDONLY);
    if (in_fd < 0) {
      perror(command.stdin_file.c_str());
      exit(EXIT_FAILURE);
    }
    dup2(in_fd, STDIN_FILENO);
    close(in_fd);
  }
  if (!command.stdout_file.empty()) {
    int out_flags =
        O_WRONLY | O_CREAT | (command.append_out ? O_APPEND : O_TRUNC);
    int out_fd = open(command.stdout_file.c_str(), out_flags, 0644);
    if (out_fd < 0) {
      perror(command.stdout_file.c_str());
      exit(EXIT_FAILURE);
    }
    dup2(out_fd, STDOUT_FILENO);
    close(out_fd);
  }

  const std::string& cmd_name = command.args[0];

  // Check for built-ins first
  if (cmd_name == "echo") {
    execute_echo(command.args, std::cin, std::cout);
  } else if (cmd_name == "pwd") {
    execute_pwd(command.args, std::cout);
  } else if (cmd_name == "cd") {
    // 'cd' is special and must be run in the parent process.
    // It won't work correctly in a pipeline child.
    // We handle this as a special case in the main execute function.
    // So this block will effectively be skipped in a child.
    execute_cd(command.args, std::cout);
  } else if (cmd_name == "ls") {
    execute_ls(command.args, std::cout);
  } else if (cmd_name == "pinfo") {
    execute_pinfo(command.args, std::cout);
  } else if (cmd_name == "history") {
    execute_history(command.args, std::cout);
  } else if (cmd_name == "jobs") {
    execute_jobs(command.args, std::cout);
  } else if (cmd_name == "exit") {
    execute_exit(command.args, std::cout);
  } else if (cmd_name == "search") {
    execute_search(command.args, std::cin, std::cout);
  } else {
    // System command
    auto expanded_args = expand_globs(command.args);
    std::vector<char*> argv;
    for (const auto& arg : expanded_args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    if (execvp(argv[0], argv.data()) == -1) {
      perror(argv[0]);
      exit(EXIT_FAILURE);
    }
  }
  // If the command was a built - in, exit the child process
  exit(EXIT_SUCCESS);
}

void execute(const std::vector<Command>& all_commands) {
  if (all_commands.empty()) {
    return;
  }

  // This is the process ID of the shell itself.
  pid_t shell_pgid = getpgrp();
  size_t start_index = 0;

  for (size_t i = 0; i < all_commands.size(); ++i) {
    // Check for special ';' marker
    if (all_commands[i].args.size() > 0 && all_commands[i].args[0] == ";") {
      // Create a sub-vector for the current pipeline/command group
      std::vector<Command> command_group(all_commands.begin() + start_index,
                                         all_commands.begin() + i);
      start_index = i + 1;  // Next group starts after the semicolon

      if (command_group.empty()) {
        continue;
      }

      // Handle special built-ins that must run in the parent shell
      if (command_group.size() == 1 && command_group[0].args[0] == "cd") {
        execute_cd(command_group[0].args, std::cout);
        continue;
      }
      if (command_group.size() == 1 && command_group[0].args[0] == "exit") {
        execute_exit(command_group[0].args, std::cout);
        continue;
      }

      // Determine if the entire pipeline is a background job.
      bool is_background =
          !command_group.empty() && command_group.back().background;

      // PIPELINE EXECUTION LOGIC
      int num_commands = command_group.size();
      int in_fd = STDIN_FILENO;  // Input for the first command
      std::vector<pid_t> child_pids;

      // A process group ID for the entire pipeline.
      pid_t pgid = 0;

      for (int j = 0; j < num_commands; ++j) {
        int pipefd[2];
        if (j < num_commands - 1) {  // Don't create a pipe for the last command
          if (pipe(pipefd) < 0) {
            perror("pipe");
            return;
          }
        }

        pid_t pid = fork();
        if (pid < 0) {
          perror("fork");
          return;
        }

        if (pid == 0) {  //  Child Process
          // Establish the process group.
          // If it's the first child, its PID becomes the PGID for the whole
          // group.

          pgid = (j == 0) ? getpid() : pgid;
          setpgid(0, pgid);

          // Restore signal defaults.
          signal(SIGINT, SIG_DFL);
          signal(SIGQUIT, SIG_DFL);
          signal(SIGTSTP, SIG_DFL);
          signal(SIGTTIN, SIG_DFL);
          signal(SIGTTOU, SIG_DFL);

          // Redirect input if it's not the first command
          if (j > 0) {
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
          }

          // Redirect output if it's not the last command
          if (j < num_commands - 1) {
            close(pipefd[0]);  // Child doesn't read from the new pipe
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
          }

          // This function calls execvp or exits
          execute_single_command(command_group[j]);

        } else {  // Parent Process
          child_pids.push_back(pid);

          // Parent also sets the child's process group to avoid race
          // conditions.
          pgid = (j == 0) ? pid : pgid;
          setpgid(pid, pgid);

          // Close the previous read-end in the parent
          if (j > 0) {
            close(in_fd);
          }

          // For the next iteration, the input will be the read-end of the new
          // pipe
          if (j < num_commands - 1) {
            close(pipefd[1]);  // Parent doesn't write to the new pipe
            in_fd = pipefd[0];
          }
        }
      }

      // This only matters if there was a pipeline.
      if (num_commands > 1) {
        close(in_fd);
      }

      if (!is_background) {  // FOREGROUND JOB

        // Give terminal control to the new process group.
        tcsetpgrp(STDIN_FILENO, pgid);

        int status;
        pid_t wait_pid;
        int children_remaining = child_pids.size();

        while (children_remaining > 0) {
          // Wait for ANY child in the process group to change state.
          wait_pid = waitpid(-pgid, &status, WUNTRACED);

          if (wait_pid > 0) {
            // A child was reaped. Decrement the count.
            children_remaining--;

            // If the child that changed state was STOPPED, we stop the whole
            // job.
            if (WIFSTOPPED(status)) {
              std::cout << "\nStopped: " << command_group[0].args[0]
                        << std::endl;
              background_jobs.push_back({pgid, command_group[0].args[0], true});
              // Break the loop now, as the job is considered "done" for now.
              break;
            }
          } else if (errno != EINTR) {
            // An actual error occurred (not just a signal interruption).
            perror("waitpid");
            break;
          }
        }
        // Take back control of the terminal.
        tcsetpgrp(STDIN_FILENO, shell_pgid);
      } else {
        // BACKGROUND JOB
        std::cout << "[" << background_jobs.size() + 1 << "] " << pgid
                  << std::endl;
        background_jobs.push_back({pgid, command_group[0].args[0], false});
      }
    }
  }
}
