#pragma once

#include <atomic>
#include <csignal>
#include <sys/types.h>

// Signal handling uses the "self-pipe" trick: the handlers do no reaping
// and touch no data structures -- they just write one byte to a pipe. The
// line editor poll()s that pipe alongside stdin, so a signal can never be
// missed between checking for it and blocking. All waitpid() calls happen
// on the main thread, in the executor, so there is exactly one reaper and
// statuses are never stolen or lost.

// Set (async-signal-safely) when SIGINT is delivered to the shell itself.
// Consumed and cleared by the line editor.
extern std::atomic<bool> got_sigint;

// Set when the `exit` builtin or end of input requests shell shutdown;
// shell_exit_status is what main() then returns.
extern std::atomic<bool> shell_exit_requested;
extern int shell_exit_status;

// Installs handlers and creates the self-pipe. Must be called once before
// any child can be spawned. SIGINT is only caught by an interactive shell;
// a script run is interrupted like any other process.
void setup_signal_handlers(bool interactive);

// Read end of the self-pipe: readable after SIGCHLD or SIGINT.
int signal_event_fd();

// Drains all pending wake notifications from the self-pipe. Non-blocking.
void drain_signal_events();
