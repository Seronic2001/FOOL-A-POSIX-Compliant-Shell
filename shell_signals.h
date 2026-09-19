#pragma once

#include <atomic>
#include <csignal>
#include <sys/types.h>

// SIGCHLD handling uses the "self-pipe" trick: the signal handler does no
// reaping and touches no data structures -- it just writes one byte to a
// pipe. All waitpid() calls happen on the main thread, in the executor, so
// there is exactly one reaper and statuses are never stolen or lost.

// Set (async-signal-safely) when SIGINT is delivered while the shell itself
// owns the terminal. Consumed and cleared by the line editor / main loop.
extern std::atomic<bool> got_sigint;

// Set when the `exit` builtin or Ctrl+D requests shell shutdown.
extern std::atomic<bool> shell_exit_requested;

// The Ctrl+C handler.
void sigint_handler(int signo);

// Installs handlers and creates the self-pipe. Must be called once before
// any child can be spawned.
void setup_signal_handlers();

// Wakes the main thread after a child state change (written by the SIGCHLD
// handler; async-signal-safe).
void notify_child_event(void);

// Drains all pending wake notifications from the self-pipe. Non-blocking.
// Returns the number of notifications drained. Call only from the main
// thread, before doing waitpid() work that the notifications signal.
size_t drain_child_events(void);
