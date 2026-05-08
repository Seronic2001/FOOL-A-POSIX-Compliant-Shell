#pragma once

#include <atomic>

// global flag is used to communicate a SIGINT (Ctrl+C) to the main loop.
extern std::atomic<bool> got_sigint;

// The Ctrl+C handler
void sigint_handler(int signo);

// Handler that cleans up terminated child processes
void sigchld_handler(int signo);