#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <functional>
#include <iostream>

#include "autocomplete.h"
#include "executor.h"
#include "history.h"
#include "line_editor.h"
#include "parser.h"
#include "prompt.h"
#include "shell_signals.h"

namespace {

void enable_raw_mode(struct termios& orig_termios) {
  tcgetattr(STDIN_FILENO, &orig_termios);
  struct termios raw = orig_termios;
  raw.c_iflag &= ~(IXON | INPCK | ISTRIP | BRKINT);
  raw.c_oflag &= ~(OPOST);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  // TCSANOW (not TCSAFLUSH): typed-ahead input must survive the switch to
  // raw mode, like in bash. TCSAFLUSH silently discarded it.
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void disable_raw_mode(const struct termios& orig_termios) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// Reads one byte of keyboard input. Returns: 1 = byte read, 0 = EOF,
// -1 = SIGINT arrived (caller should abort the current line).
//
// Waits on the keyboard and the signal self-pipe together, so a background
// job finishing mid-edit is reported immediately; `redraw` then repaints
// the prompt that the notice overwrote.
int read_byte(char& out, const std::function<void()>& redraw) {
  while (true) {
    if (got_sigint) return -1;

    struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0},
                            {signal_event_fd(), POLLIN, 0}};
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      return 0;
    }
    if (fds[1].revents != 0 && reap_finished_children()) redraw();
    if (fds[0].revents != 0) {
      ssize_t n = read(STDIN_FILENO, &out, 1);
      if (n == 1) return 1;
      if (n < 0 && errno == EINTR) continue;
      return 0;  // EOF or real error
    }
  }
}

// Continuation bytes (10xxxxxx) belong to the preceding UTF-8 character.
bool is_utf8_continuation(unsigned char c) {
  return (c & 0xC0) == 0x80;
}

// Number of terminal columns the string occupies (approximation: one per
// code point), ignoring ANSI color escapes.
size_t visible_width(const std::string& s) {
  size_t width = 0;
  bool in_escape = false;
  for (char ch : s) {
    if (in_escape) {
      if (ch == 'm') in_escape = false;
      continue;
    }
    if (ch == '\033') {
      in_escape = true;
      continue;
    }
    if (!is_utf8_continuation((unsigned char)ch)) width++;
  }
  return width;
}

// Reads one command from non-terminal input. A line with an open quote
// continues onto the next physical line, like a real shell's secondary
// prompt.
std::string read_batch_line() {
  std::string line;
  while (true) {
    int c;
    while ((c = getchar()) != EOF && c != '\n' && c != '\r') {
      line += (char)c;
    }
    if (c == EOF) {
      if (line.empty()) shell_exit_requested = true;
      return line;
    }
    if (!parse_input(line).incomplete) return line;
    line += '\n';
  }
}

}  // namespace

std::string read_command_line() {
  if (!shell_interactive) return read_batch_line();

  got_sigint = false;  // A stale Ctrl+C must not discard this line.

  struct termios orig_termios;
  enable_raw_mode(orig_termios);

  // The prompt cannot change while a line is being edited.
  const std::string prompt = generate_prompt_string();
  const size_t prompt_width = visible_width(prompt);

  std::string line;
  size_t cursor_pos = 0;  // Byte offset into `line`.
  bool last_key_was_tab = false;

  // State for history navigation.
  const auto& history = get_history();
  size_t history_index = history.size();
  std::string current_buffer;

  auto redraw_line = [&]() {
    std::string out = "\r\x1b[K" + prompt + line + "\r";
    size_t column = prompt_width + visible_width(line.substr(0, cursor_pos));
    if (column > 0) out += "\x1b[" + std::to_string(column) + "C";
    std::cout << out << std::flush;
  };

  auto finish = [&](const char* trailer) {
    disable_raw_mode(orig_termios);
    std::cout << trailer << std::flush;
  };

  // Called after the user changes the text: it is no longer a history
  // entry being browsed.
  auto edited = [&]() {
    history_index = history.size();
    current_buffer = line;
  };

  // Start of the UTF-8 character before `pos` / end of the one at `pos`.
  auto prev_char = [&](size_t pos) {
    if (pos > 0) pos--;
    while (pos > 0 && is_utf8_continuation((unsigned char)line[pos])) pos--;
    return pos;
  };
  auto next_char = [&](size_t pos) {
    if (pos < line.size()) pos++;
    while (pos < line.size() &&
           is_utf8_continuation((unsigned char)line[pos])) {
      pos++;
    }
    return pos;
  };

  auto recall_history = [&](size_t index) {
    if (history_index == history.size()) current_buffer = line;
    history_index = index;
    line = (index == history.size()) ? current_buffer : history[index];
    cursor_pos = line.size();
  };

  redraw_line();

  while (true) {
    char c;
    int rr = read_byte(c, redraw_line);
    if (rr == -1 || (rr == 1 && c == 3)) {  // SIGINT or Ctrl+C
      finish("^C\r\n");
      return "";
    }
    // EOF or Ctrl+D: exit on an empty line, otherwise do nothing (bash
    // behavior).
    if (rr == 0 || c == 4) {
      if (line.empty()) {
        shell_exit_requested = true;
        finish("exit\r\n");
        return "";
      }
      if (rr == 0) break;  // Input is gone: run what we have.
      continue;
    }

    if (c == '\n' || c == '\r') break;

    if (c == '\t') {
      if (line.empty()) continue;  // Nothing to complete.
      const bool repeat_tab = last_key_was_tab;
      last_key_was_tab = true;
      std::vector<std::string> matches = handle_autocomplete(line, cursor_pos);
      if (matches.size() > 1 && repeat_tab) {
        // Second consecutive Tab: show the possibilities.
        std::cout << "\r\n";
        for (const auto& match : matches) {
          std::cout << match << "  ";
        }
        std::cout << "\r\n";
      }
      current_buffer = line;  // Completion edits are user edits too.
      redraw_line();
      continue;
    }

    last_key_was_tab = false;

    if (c == 127 || c == 8) {  // Backspace
      size_t start = prev_char(cursor_pos);
      line.erase(start, cursor_pos - start);
      cursor_pos = start;
      edited();
    } else if (c == 21) {  // Ctrl+U: erase to start of line
      line.erase(0, cursor_pos);
      cursor_pos = 0;
      edited();
    } else if (c == 1) {  // Ctrl+A
      cursor_pos = 0;
    } else if (c == 5) {  // Ctrl+E
      cursor_pos = line.size();
    } else if (c == 12) {  // Ctrl+L (clear screen)
      std::cout << "\x1b[H\x1b[2J\x1b[3J";
    } else if (c == '\x1b') {
      // Escape sequence: CSI is "ESC [ params final", SS3 is "ESC O final".
      // Reading to the final byte keeps unhandled keys (F-keys, Ctrl+arrow)
      // from leaking their tail into the line as text.
      std::string params;
      char final_byte = 0;
      char b;
      if (read_byte(b, redraw_line) != 1) continue;
      if (b == '[') {
        while (read_byte(b, redraw_line) == 1) {
          if (b >= 0x40 && b <= 0x7E) {
            final_byte = b;
            break;
          }
          params += b;
        }
      } else if (b == 'O') {
        if (read_byte(b, redraw_line) == 1) final_byte = b;
      }

      if (final_byte == '~') {
        if (params == "3") {  // Delete
          line.erase(cursor_pos, next_char(cursor_pos) - cursor_pos);
          edited();
        } else if (params == "1" || params == "7") {  // Home
          cursor_pos = 0;
        } else if (params == "4" || params == "8") {  // End
          cursor_pos = line.size();
        }
      } else if (params.empty()) {
        if (final_byte == 'D') {  // Left
          cursor_pos = prev_char(cursor_pos);
        } else if (final_byte == 'C') {  // Right
          cursor_pos = next_char(cursor_pos);
        } else if (final_byte == 'A') {  // Up
          if (history_index > 0) recall_history(history_index - 1);
        } else if (final_byte == 'B') {  // Down
          if (history_index < history.size()) {
            recall_history(history_index + 1);
          }
        } else if (final_byte == 'H') {  // Home
          cursor_pos = 0;
        } else if (final_byte == 'F') {  // End
          cursor_pos = line.size();
        }
      }
    } else if ((unsigned char)c >= 32) {  // Regular character
      line.insert(cursor_pos, 1, c);
      cursor_pos++;
      edited();
    }
    // Any other control character (including Ctrl+Z, which the shell
    // itself ignores) is dropped.

    redraw_line();
  }

  finish("\r\n");
  return line;
}
