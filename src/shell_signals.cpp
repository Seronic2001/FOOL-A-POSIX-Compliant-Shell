#include "shell_signals.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

std::atomic<bool> got_sigint(false);
std::atomic<bool> shell_exit_requested(false);
int shell_exit_status = 0;

static int event_pipe[2] = {-1, -1};

// Async-signal-safe: a single write(). Best effort -- if the pipe is full,
// a notification is already pending and the main thread will wake anyway.
static void notify_event() {
  int saved_errno = errno;
  ssize_t rc = write(event_pipe[1], "x", 1);
  (void)rc;
  errno = saved_errno;
}

static void sigchld_handler(int) { notify_event(); }

static void sigint_handler(int) {
  got_sigint.store(true, std::memory_order_relaxed);
  notify_event();
}

static void install(int signo, void (*handler)(int), int flags) {
  struct sigaction sa = {};
  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = flags;
  if (sigaction(signo, &sa, nullptr) == -1) {
    perror("sigaction");
    _exit(1);
  }
}

void setup_signal_handlers(bool interactive) {
  if (pipe(event_pipe) < 0) {
    perror("pipe");
    _exit(1);
  }
  // Both ends non-blocking: the handler must never block, and draining must
  // never block on an empty pipe. Close-on-exec keeps exec'd children from
  // holding the pipe open.
  for (int fd : event_pipe) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
  }

  // SA_RESTART: a child exiting must not make a blocked read() of script
  // input fail with EINTR (the editor is woken through the pipe instead).
  // SA_NOCLDSTOP: stopped children are discovered via waitpid(WUNTRACED).
  install(SIGCHLD, sigchld_handler, SA_RESTART | SA_NOCLDSTOP);
  if (interactive) install(SIGINT, sigint_handler, SA_RESTART);
}

int signal_event_fd() { return event_pipe[0]; }

void drain_signal_events() {
  char buf[64];
  while (true) {
    ssize_t r = read(event_pipe[0], buf, sizeof(buf));
    if (r > 0 || (r < 0 && errno == EINTR)) continue;
    break;  // EAGAIN (empty) or unexpected error
  }
}
