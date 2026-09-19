#include "shell_signals.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

std::atomic<bool> got_sigint(false);
std::atomic<bool> shell_exit_requested(false);

// Matches the definition below; needed because setup_signal_handlers()
// takes its address before the definition is seen.
void sigchld_handler(int signo);

// Self-pipe: the SIGCHLD handler writes a single byte; the main thread
// drains the pipe wherever it is safe to call waitpid(). The handler itself
// performs NO reaping -- there is exactly one reaper (the main thread), so
// child statuses are never stolen between the handler and the executor.
static int event_pipe[2] = {-1, -1};

void setup_signal_handlers() {
  if (pipe(event_pipe) < 0) {
    perror("pipe");
    _exit(1);
  }
  // Both ends non-blocking: the handler must never block, and draining must
  // never block on an empty pipe. Close-on-exec keeps exec'd children from
  // holding the pipe open.
  for (int fd : event_pipe) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(fd, F_GETFD, 0);
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
  // Small pipe is fine: one byte per notification, and the main thread
  // drains after every command and inside foreground waits.

  struct sigaction sa;
  sa.sa_handler = &sigchld_handler;
  sigemptyset(&sa.sa_mask);
  // No SA_RESTART: read() in the line editor should be interruptible by
  // signals so it can re-check its state.
  // SA_NOCLDSTOP: stopped/continued children are discovered via
  // waitpid(WUNTRACED) in the executor, not through the handler.
  sa.sa_flags = SA_NOCLDSTOP;
  if (sigaction(SIGCHLD, &sa, nullptr) == -1) {
    perror("sigaction");
    _exit(1);
  }

  struct sigaction si;
  si.sa_handler = &sigint_handler;
  sigemptyset(&si.sa_mask);
  si.sa_flags = 0;
  if (sigaction(SIGINT, &si, nullptr) == -1) {
    perror("sigaction");
    _exit(1);
  }
}

void sigint_handler(int signo) {
  (void)signo;
  got_sigint.store(true, std::memory_order_relaxed);
}

void sigchld_handler(int signo) {
  (void)signo;
  int saved_errno = errno;
  notify_child_event();
  errno = saved_errno;
}

void notify_child_event(void) {
  // Async-signal-safe: a single write(). Best effort -- if the pipe is full,
  // a notification is already pending and the main thread will reap anyway.
  ssize_t rc = write(event_pipe[1], "c", 1);
  (void)rc;
}

size_t drain_child_events(void) {
  char buf[64];
  size_t n = 0;
  while (true) {
    ssize_t r = read(event_pipe[0], buf, sizeof(buf));
    if (r > 0) {
      n += (size_t)r;
      continue;
    }
    if (r < 0 && errno == EINTR) continue;
    break;  // EAGAIN (empty) or unexpected error
  }
  return n;
}
