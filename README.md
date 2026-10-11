# FOOL : A POSIX-Style Shell

## Overview

FOOL is a custom interactive shell program written in C++ that emulates the core functionalities of a standard POSIX shell. It can parse user input, manage foreground and background processes, handle I/O redirection and pipelines, and provides advanced features like tab-completion and command history. It is not a full POSIX `sh`: there are no variables, `$?`, `&&`/`||`, command substitution or script arguments. The entire project is written in a modular fashion for clarity and maintainability.

-----

## Execution Flow
The following diagram illustrates the high-level execution flow of the shell, from reading user input to executing the command and displaying the next prompt.

```mermaid
graph TD
    A[Start Shell] --> B{Main Loop};
    B --> C[Display Prompt];
    C --> D["Read Command Line <br><i>line_editor.cpp</i>"];
    D --> E["Parse Input String <br><i>parser.cpp</i>"];
    E --> F{For each command group...};
    F --> G{"Built-in Command? <br> (e.g., cd, exit)"};
    G -- Yes --> H["Execute in Parent Shell <br><i>builtins.cpp</i>"];
    H --> B;
    G -- No --> I["Execute Pipeline <br><i>executor.cpp</i>"];
    I --> J{Fork Process};
    J --> K{"Setup Pipes & <br> Redirection"};
    K --> L{"execvp() or <br> Built-in in Child"};
    L --> M["Parent Waits for <br> Foreground Job"];
    M --> F;
    F -- All groups done --> B;
```
-----

## Features Implemented

This shell supports a wide range of features as per the assignment requirements:

### 1\. Dynamic Shell Prompt

  * The shell displays a dynamic prompt in the format `username@system_name:current_directory$`.
  * The directory from which the shell is launched is treated as the home directory and is represented by `~`.
  * The prompt correctly reflects changes in the current working directory.

### 2\. Command Parsing & Execution

  * **Sequential Commands**: Supports a semicolon-separated list of commands.
  * **Background Processes**: Any command ending with `&` is executed as a background process, and its process ID is printed. `a & b` starts `a` in the background and then runs `b`.
  * **System Commands**: Can execute any system command (like `vi`, `gedit`, etc.) in either the foreground or background.
  * **Robust Tokenizing**: Handles random spaces and tabs in user input appropriately.
  * **Expansion**: Unquoted `*`, `?` and `[...]` glob against file names, even in a partly quoted word (`"my dir"/*.c`); a leading `~` expands to `$HOME`. A backslash makes the next character literal (`echo \;`).

### 3\. Built-in Commands

The following commands are implemented internally without using `execvp`:

  * `cd`: Changes the current directory. Supports `.` (current), `..` (parent), `~` (home), and `-` (previous) arguments. `cd` with no arguments changes to the home directory.
  * `pwd`: Prints the absolute path of the current working directory.
  * `echo`: Prints its arguments to standard output, handling spaces and tabs correctly.
  * `ls`: Lists directory contents. Supports `-l` (long format) and `-a` (show all files) flags, which can be combined (e.g., `ls -la`). It can also list specific files or directories. Any other option (e.g. `ls -t`) is handed to the system `ls`.
  * `pinfo`: Displays process information, including process status, memory usage, and executable path for the shell itself (`pinfo`) or a given PID (`pinfo <pid>`).
  * `search`: Recursively searches for a file or folder in the current directory and prints `True` or `False`.
  * `history`: Displays the last 10 commands entered. `history <num>` displays the last `<num>` commands. The shell stores up to 1000 commands, which persist across sessions in `~/.my_shell_history`.
  * `exit`: Terminates the shell session; `exit <n>` sets the shell's exit status.

A built-in that runs on its own in the foreground executes inside the shell process (no `fork`); inside a pipeline or in the background it runs in a child.

### 4\. I/O Redirection and Pipelines

  * **Redirection**: Supports input redirection (`<`), output redirection (`>`), and append output redirection (`>>`). Output files are created with permissions `0644` if they don't exist. Any descriptor can be named (`2> errors.txt`, `1> out.txt`) or duplicated (`2>&1`, `>&2`); redirections apply left to right.
  * **Piping**: Supports chaining any number of commands together using pipes (`|`) to direct the output of one command to the input of the next.
  * **Combined Usage**: Handles I/O redirection within a pipeline of commands.

### 5\. Job Control and Signal Handling

  * `Ctrl+C`: Interrupts the current foreground process by sending it a `SIGINT` signal. It has no effect if no foreground process is running.
  * `Ctrl+Z`: Pushes the current foreground process into the background in a stopped state. It has no effect if no foreground process is running.
  * `fg [%n]` / `bg [%n]`: Resume a stopped or background job (the most recent one by default) in the foreground or background.
  * `Ctrl+D`: Logs out of the shell if the input line is empty.

Job control is only active when the shell is used interactively. When it reads a script from a file or pipe it stays in its caller's process group, so `Ctrl+C` interrupts the whole run.

### 6\. Advanced Line Editing Features

  * **Tab Autocomplete**: Provides command and file/directory completion when the `TAB` key is pressed.
      * Completes the word if there is a single match.
      * Completes the longest common prefix for multiple matches.
      * Displays all possible matches on a second `TAB` press.
  * **History Navigation**: Users can navigate through previous commands using the `Up Arrow` and `Down Arrow` keys. The retrieved command can be edited before execution.
  * **Editing Keys**: `Left`/`Right`, `Home`/`End` (or `Ctrl+A`/`Ctrl+E`), `Backspace`, `Delete`, `Ctrl+U` (erase to start of line) and `Ctrl+L` (clear screen).

-----

## Additional Features

Beyond the core requirements, this shell implements several quality-of-life improvements:

  * **`jobs` Command**: A built-in `jobs` command is provided to list all currently running background processes, along with their job number, state (Running/Stopped), command name, and PID.
  * **`clear` Command**: Includes a `clear` command to clear the terminal screen.
  * **Colored Output**: The shell prompt and the output of the `ls` command use ANSI color codes for enhanced readability, distinguishing between directories, executables, and regular files. `ls` only colors its output when writing to a terminal, so `ls | grep` and `ls > file` see plain names.
  * **Robust Quote Handling**: The custom parser correctly handles arguments enclosed in single (`'`) or double (`"`) quotes, allowing for filenames and arguments with spaces. This is a significant improvement over the suggested `strtok` method.
  * **Human-Readable `ls`**: The built-in `ls` command supports an additional `-h` flag, which, when used with `-l`, displays file sizes in a human-readable format (e.g., `4.0K`, `1.2M`, `2.0G`).

-----

## Code Structure

The project is organized into several modular files, each with a specific responsibility. All sources live in `src/`:

  * **`src/main.cpp`**: The entry point of the shell. It contains the main loop, initializes history, and sets up signal handlers and (when interactive) job control.
  * **`src/parser.cpp`**: Responsible for tokenizing the raw input string into a list of pipelines, each a sequence of commands with their redirections, handling quotes, escapes, semicolons, pipes, and `&`.
  * **`src/executor.cpp`**: The execution engine. It takes parsed pipelines and manages word expansion, forking, piping, I/O redirection, the job table (`fg`/`bg`), and running both built-in and system commands.
  * **`src/builtins.cpp`**: Contains the implementation for all built-in commands like `cd`, `ls`, `pinfo`, `history`, etc.
  * **`src/line_editor.cpp`**: Manages the interactive command-line interface. It enables raw terminal mode to handle keypresses for history navigation, tab completion, and signal shortcuts.
  * **`src/autocomplete.cpp`**: Implements the logic for tab completion for both system commands and local files/directories. `PATH` is scanned on the first `TAB`, not at startup.
  * **`src/history.cpp`**: Manages the command history, including loading from and saving to a file to ensure persistence across sessions.
  * **`src/prompt.cpp`**: Contains the logic for generating and displaying the dynamic shell prompt.
  * **`src/shell_signals.cpp`**: Implements the signal handlers. They use the self-pipe trick: a handler is async-signal-safe (a single `write()` of a wake byte), the line editor `poll()`s that pipe alongside the keyboard, and all reaping happens on the main thread, so child statuses are never lost or double-reaped.
  * **`src/colors.h`**: A utility header defining ANSI color codes for styled output.
  * **`tests/`**: The test suite (unit, batch integration, and interactive pty tests) — see [Testing](#testing).

-----

## How to Compile and Run

1.  **Compile the code:**

    ```bash
    make
    ```

2.  **Run the shell:**

    ```bash
    ./FOOL
    ```

-----

## Testing

The project ships a three-layer test suite (138 checks total):

1.  **Unit tests** (`tests/test_parser.cpp`, `tests/test_builtins.cpp`):
    in-process C++ tests built on a minimal single-header framework
    (`tests/test_framework.h`). They cover parsing (quoting, operators,
    pipeline splitting, redirections, error cases), history trimming/dedup,
    and the pure builtins (`echo`, `search`, `pwd`, `jobs`).

2.  **Batch-mode integration tests** (`tests/test_batch.py`): a Python
    suite that feeds scripts to the compiled shell on stdin and asserts on
    output, files, and exit codes. Covers redirections, pipelines,
    globbing, background jobs, error handling, and regressions (e.g. the
    child-builtin `_exit()` flush bug).

3.  **Interactive pty tests** (`tests/test_interactive.py`): runs the
    shell under a pseudo-terminal (stdlib `pty` only, no external deps) and
    exercises the line editor the way a real user would: Ctrl+C/Ctrl+Z
    delivery, `fg`, Ctrl+D exit, history navigation, editing keys, and tab
    completion.

Run everything with:

```bash
make test              # full suite: unit + batch + interactive
make test-unit         # C++ unit tests only
make test-batch        # batch integration tests only
make test-interactive  # interactive pty tests only
# or equivalently:
bash tests/run_tests.sh
```

The integration/interactive suites require `python3` (standard library
only; no pip packages needed). The interactive suite needs a Unix-like
system with pty support and will skip nothing — it runs real keypresses
against a real terminal.

-----

## Design Notes

  * Instead of using `strtok` as suggested for tokenizing, this shell implements a more robust, custom character-by-character parser. This allows for better handling of special cases like quoted arguments and avoids the destructive nature of `strtok`.