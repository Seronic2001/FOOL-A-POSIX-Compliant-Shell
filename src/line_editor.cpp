#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
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

namespace {

// Read one byte, retrying on EINTR. Returns: 1 = byte read, 0 = EOF,
// -1 = SIGINT arrived (caller should abort the current line).
int read_byte(char& out) {
  while (true) {
    ssize_t n = read(STDIN_FILENO, &out, 1);
    if (n == 1) return 1;
    if (n < 0 && errno == EINTR) {
      // A child changed state mid-edit; update job notices now.
      reap_finished_children();
      // Ctrl+C may have interrupted this very read. Abort the line so the
      // shell can show a fresh prompt immediately (bash behavior).
      if (got_sigint.exchange(false)) return -1;
      continue;
    }
    return 0;  // EOF or real error
  }
}

// Number of columns a UTF-8 character occupies (approximation: 1 per code
// point). Continuation bytes (10xxxxxx) do not advance the cursor.
bool is_utf8_continuation(unsigned char c) {
  return (c & 0xC0) == 0x80;
}

// Number of terminal columns the string occupies, ignoring ANSI escapes.
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

// Returns true if, scanning `line`, all quote characters are closed.
// Backslash escapes are honored outside quotes.
static bool quotes_balanced(const std::string& line) {
  char quote = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
    } else if (c == '\\') {
      ++i;  // Skip the escaped character.
    }
  }
  return quote == 0;
}

std::string run_batch_line() {
  std::string line;
  int c;
  while ((c = getchar()) != EOF) {
    if (c == '\n' || c == '\r') break;
    line += (char)c;
  }
  bool hit_eof = (c == EOF);
  // A batch line with an open quote continues onto the next physical line,
  // like a real shell's secondary prompt. Loop until balanced or EOF.
  while (!hit_eof && !quotes_balanced(line)) {
    line += '\n';
    while ((c = getchar()) != EOF) {
      if (c == '\n' || c == '\r') break;
      line += (char)c;
    }
    hit_eof = (c == EOF);
  }
  if (hit_eof && line.empty()) {
    shell_exit_requested = true;  // Ctrl+D in batch mode
  }
  return line;
}

}  // namespace

std::string read_command_line() {
  struct termios orig_termios;
  bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

  if (!interactive) {
    return run_batch_line();
  }

  enable_raw_mode(orig_termios);

  std::string line;
  size_t cursor_pos = 0;
  static bool last_key_was_tab = false;

  // State for history navigation.
  const auto& history = get_history();
  size_t history_index = history.size();
  std::string current_buffer;

  auto redraw_line = [&]() {
    std::string prompt = generate_prompt_string();
    size_t prompt_len = visible_width(prompt);
    std::cout << "\r\x1b[K" << std::flush;
    display_prompt();
    std::cout << line << std::flush;
    if (prompt_len + cursor_pos > 0) {
      std::cout << "\r\x1b[" << (prompt_len + cursor_pos) << "C" << std::flush;
    }
  };

  redraw_line();

  while (true) {
    char c;
    int rr = read_byte(c);
    if (rr == -1) {
      // Interrupted by Ctrl+C: discard the line and let main() redraw.
      disable_raw_mode(orig_termios);
      std::cout << "^C\r\n";
      got_sigint = true;
      return "";
    }
    if (rr == 0) {
      // EOF (Ctrl+D): only exit if the line is empty, like canonical shells.
      if (line.empty()) {
        disable_raw_mode(orig_termios);
        shell_exit_requested = true;
        std::cout << "exit" << std::endl;
        return "";
      }
      break;  // Treat as end of line.
    }

    if (c == '\n' || c == '\r') break;

    if (c == '\t') {
      if (cursor_pos == 0 && line.empty()) {
        // Nothing to complete on an empty line.
        continue;
      }
      const bool repeat_tab = last_key_was_tab;
      last_key_was_tab = true;
      size_t safe_cursor = cursor_pos;
      std::vector<std::string> matches =
          handle_autocomplete(line, safe_cursor);
      cursor_pos = safe_cursor;
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

    if (c == 127) {  // Backspace
      if (cursor_pos > 0) {
        // Erase one full UTF-8 character, not one byte.
        size_t start = cursor_pos - 1;
        while (start > 0 && is_utf8_continuation((unsigned char)line[start])) {
          start--;
        }
        line.erase(start, cursor_pos - start);
        cursor_pos = start;
      }
      history_index = history.size();
      current_buffer = line;
    } else if (c == 3) {  // Ctrl+C
      if (foreground_job.pgid != 0) {
        // Forward the interrupt to the whole foreground process group.
        killpg(foreground_job.pgid, SIGINT);
      } else {
        // Discard the line; read_byte()'s interrupt path handles it, but
        // the raw byte can also arrive buffered.
        disable_raw_mode(orig_termios);
        std::cout << "^C\r\n";
        got_sigint = true;
        return "";
      }
    } else if (c == 26) {  // Ctrl+Z
      if (foreground_job.pgid != 0) {
        killpg(foreground_job.pgid, SIGTSTP);
      }
      // No foreground job: ignore (the shell itself ignores SIGTSTP).
    } else if (c == 4) {  // Ctrl+D
      if (line.empty()) {
        disable_raw_mode(orig_termios);
        std::cout << "exit" << std::endl;
        shell_exit_requested = true;
        return "";
      }
      // Non-empty line: EOF does nothing (bash behavior).
    } else if (c == 12) {  // Ctrl+L (clear screen)
      std::cout << "\x1b[H\x1b[2J\x1b[3J" << std::flush;
      redraw_line();
    } else if (c == '\x1b') {
      char seq[2];
      if (!read_byte(seq[0])) continue;
      if (!read_byte(seq[1])) continue;

      if (seq[0] == '[') {
        if (seq[1] == 'D') {  // Left
          if (cursor_pos > 0) {
            cursor_pos--;
            while (cursor_pos > 0 &&
                   is_utf8_continuation((unsigned char)line[cursor_pos])) {
              cursor_pos--;
            }
          }
        } else if (seq[1] == 'C') {  // Right
          if (cursor_pos < line.length()) {
            cursor_pos++;
            while (cursor_pos < line.length() &&
                   is_utf8_continuation((unsigned char)line[cursor_pos])) {
              cursor_pos++;
            }
          }
        } else if (seq[1] == 'A') {  // Up
          if (history_index > 0) {
            if (history_index == history.size()) {
              current_buffer = line;
            }
            history_index--;
            line = history[history_index];
            cursor_pos = line.length();
          }
        } else if (seq[1] == 'B') {  // Down
          if (history_index < history.size()) {
            history_index++;
            if (history_index == history.size()) {
              line = current_buffer;
            } else {
              line = history[history_index];
            }
            cursor_pos = line.length();
          }
        }
      }
    } else {  // Regular character
      line.insert(cursor_pos, 1, c);
      cursor_pos++;
      history_index = history.size();
      current_buffer = line;
    }

    redraw_line();
  }

  disable_raw_mode(orig_termios);
  std::cout << "\r\n";
  return line;
}
