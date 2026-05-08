
#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <iostream>

#include "autocomplete.h"
#include "executor.h"
#include "history.h"
#include "line_editor.h"
#include "prompt.h"
#include "shell_signals.h"

void enable_raw_mode(struct termios& orig_termios) {
  tcgetattr(STDIN_FILENO, &orig_termios);
  struct termios raw = orig_termios;
  // c_iflag: Input flags
  // Disable software flow control (Ctrl-S, Ctrl-Q) and other input processing
  raw.c_iflag &= ~(IXON | INPCK | ISTRIP | BRKINT);

  // c_oflag: Output flags
  // Disable all output processing (like converting '\n' to '\r\n')
  raw.c_oflag &= ~(OPOST);

  // c_lflag: Local flags
  // Disable echo, canonical mode, and signal characters (Ctrl-C, Ctrl-Z)
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void disable_raw_mode(const struct termios& orig_termios) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

std::string read_command_line() {
  struct termios orig_termios;
  enable_raw_mode(orig_termios);

  std::string line;
  size_t cursor_pos = 0;
  static bool last_key_was_tab = false;

  // state for history navigation
  const auto& history = get_history();
  size_t history_index = history.size();
  std::string current_buffer = "";

  auto redraw_line = [&]() {
    std::string prompt = generate_prompt_string();
    size_t prompt_len = calculate_visible_length(prompt);
    // Move cursor to beginning of line, then clear to the end
    std::cout << "\r\x1b[K" << std::flush;
    // Reprint the prompt and the current command line buffer
    display_prompt();
    std::cout << line << std::flush;
    // Move cursor back to the correct position
    std::cout << "\r\x1b[" << (prompt_len + cursor_pos) << "C" << std::flush;
  };

  // Initial prompt display
  redraw_line();

  while (true) {
    char c;
    ssize_t bytes_read = read(STDIN_FILENO, &c, 1);

    if (bytes_read < 0 && errno == EINTR) {
      // The read was interrupted by a signal (like SIGCHLD).
      redraw_line();
      continue;  // Go back to waiting for the next keypress
    }

    if (bytes_read <= 0 || c == '\n' || c == '\r') {
      // EOF (Ctrl+D) or Enter key
      break;
    }

    if (c == '\t') {  // Tab for autocomplete
      std::vector<std::string> matches = handle_autocomplete(line, cursor_pos);

      if (matches.size() > 1 && last_key_was_tab) {
        // This is the SECOND tab press with multiple options
        std::cout << "\r\n";
        for (const auto& match : matches) {
          std::cout << match << "\t";
        }
        std::cout << "\r\n";
      }
      last_key_was_tab = true;
      redraw_line();

    }  // auto complete done.
    else {
      last_key_was_tab = false;  // Reset on any other keypress
      if (c == 127) {            // Backspace
        if (cursor_pos > 0) {
          line.erase(cursor_pos - 1, 1);
          cursor_pos--;
        }
        // User is editing, so reset history navigation
        history_index = history.size();
        current_buffer = line;
      }  // Backspace done.
      else if (c == 3) {  // Ctrl+C
        if (foreground_pid != 0) {
          // If a foreground process is running, send it the SIGINT signal
          kill(foreground_pid, SIGINT);
        } else {
          write(STDOUT_FILENO, "^C", 2);
          got_sigint = true;
          break;
        }
      } else if (c == 26) {  // Ctrl+Z
        if (foreground_pid != 0) {
          // If a foreground process is running, send it the SIGTSTP signal to
          // stop it
          kill(foreground_pid, SIGTSTP);
        }
        // If no process is running, do nothing.
      } else if (c == 4) {  // Ctrl+D
        if (line.empty()) {
          // If the line is empty, treat as EOF and exit the shell
          disable_raw_mode(orig_termios);
          std::cout << "exit" << std::endl;
          exit(0);
        }
      } else if (c == '\x1b') {
        char seq[2];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
          continue;
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
          continue;
        }

        if (seq[0] == '[') {
          if (seq[1] == 'D') {  // Left Arrow
            if (cursor_pos > 0) {
              cursor_pos--;
            }
          } else if (seq[1] == 'C') {  // Right Arrow
            if (cursor_pos < line.length()) {
              cursor_pos++;
            }
          } else if (seq[1] == 'A') {  // Up Arrow
            if (history_index > 0) {
              if (history_index == history.size()) {
                // Save current typing before navigating away
                current_buffer = line;
              }
              history_index--;
              line = history[history_index];
              cursor_pos = line.length();
            }
          } else if (seq[1] == 'B') {  // Down Arrow
            if (history_index < history.size()) {
              history_index++;
              if (history_index == history.size()) {
                // Reached the end, restore what user was typing
                line = current_buffer;
              } else {
                line = history[history_index];
              }
              cursor_pos = line.length();
            }
          }
          // Handling arrow keys done.
        }
      } else {  // Any other character
        line.insert(cursor_pos, 1, c);
        cursor_pos++;
        // User is editing, so reset history navigation
        history_index = history.size();
        current_buffer = line;
      }

      if (c != 3) {  // Redraw the line for any key EXCEPT Ctrl+C
        redraw_line();
      }
    }
  }
  disable_raw_mode(orig_termios);
  std::cout << "\r\n";
  return line;
}