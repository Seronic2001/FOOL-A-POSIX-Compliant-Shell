#!/usr/bin/env python3
"""Interactive (pty) tests for the FOOL shell.

Runs FOOL under a pseudo-terminal using only the Python standard library
(pty + os.read/os.write) so Ctrl+C, Ctrl+D, history navigation, and the
line editor are exercised the way a real user would.

Timing note: the shell echoes typed characters as they are redrawn, so an
`expect()` for a marker that also appears inside the echoed command line
would return before the command runs. Tests therefore sleep briefly after
sending each line to let the shell reach a quiescent state before asserting.

Run via tests/run_tests.sh or `make test-interactive`.
"""

import os
import pty
import re
import select
import shutil
import signal
import sys
import tempfile
import time

FOOL = os.environ.get(
    "FOOL_BIN",
    os.path.join(os.path.dirname(__file__), "..", "FOOL"),
)

PASS = 0
FAIL = 0
FAILURES = []


class ShellSession:
    """A FOOL process attached to a pty."""

    def __init__(self, timeout=10, env_extra=None):
        self.timeout = timeout
        pid, fd = pty.fork()
        if pid == 0:
            env = dict(os.environ)
            env.setdefault("TERM", "dumb")
            env.update(env_extra or {})
            os.execvpe(FOOL, [FOOL], env)
            os._exit(127)
        self.pid = pid
        self.fd = fd
        self.buffer = b""

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.fd, data)

    def send_line(self, line=""):
        self.send(line + "\r")

    def read_available(self, seconds):
        """Read whatever arrives within `seconds`. Returns bytes read."""
        got = b""
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            got += chunk
            self.buffer += chunk
        return got

    def expect(self, pattern, timeout=None):
        """Read until `pattern` appears anywhere in the buffer."""
        if isinstance(pattern, str):
            pattern = pattern.encode()
        deadline = time.time() + (timeout or self.timeout)
        while time.time() < deadline:
            if pattern in self.buffer:
                return self.buffer
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            self.buffer += chunk
        return self.buffer if pattern in self.buffer else None

    def quiesce(self, quiet_for=0.35, max_wait=3.0):
        """Wait until no output arrives for `quiet_for` seconds."""
        end = time.time() + max_wait
        last_data = time.time()
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(self.fd, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                self.buffer += chunk
                last_data = time.time()
            elif time.time() - last_data >= quiet_for:
                break

    def visible(self):
        """Buffer with ANSI escapes and cursor-motion noise stripped."""
        text = self.buffer.decode(errors="replace")
        text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)
        text = re.sub(r"\x1b[()][A-Za-z0-9]", "", text)
        return text

    def try_reap(self):
        """Non-blocking reap; returns (exited, status)."""
        try:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
        except ChildProcessError:
            return True, 0  # Already reaped elsewhere.
        return (pid != 0), status

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass
        try:
            os.kill(self.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [ PASS ] {name}")
    else:
        FAIL += 1
        FAILURES.append(name)
        print(f"  [ FAIL ] {name}  {detail}")


TESTS = []


def register(fn):
    TESTS.append(fn)
    return fn


@register
def test_prompt_appears(sess, tmp):
    out = sess.expect("$")
    check("prompt-appears", out is not None, "no prompt seen")


@register
def test_simple_echo(sess, tmp):
    sess.buffer = b""
    sess.send_line("echo hello-interactive")
    sess.quiesce()
    text = sess.visible()
    # Output must contain the marker on its own (not only inside the
    # echoed command line).
    check("echo-interactive", "hello-interactive\r\n" in text,
          text[-200:])


@register
def test_ctrl_c_discards_line(sess, tmp):
    sess.buffer = b""
    sess.send_line("echo typed-but-not-executed")
    time.sleep(0.3)  # Let the characters land while the shell edits.
    sess.send("\x03")  # Ctrl+C while the shell owns the terminal.
    sess.quiesce()
    before = sess.visible()
    check("ctrlc-shows-hint", "^C" in before, before[-200:])
    sess.buffer = b""
    sess.send_line("echo after-ctrlc")
    sess.quiesce()
    text = sess.visible()
    check("ctrlc-discards-line",
          "after-ctrlc\r\n" in text and
          "typed-but-not-executed\r\n" not in text.replace(
              "echo typed-but-not-executed\r", ""),
          text[-200:])


@register
def test_ctrl_d_exits(sess, tmp):
    sess.buffer = b""
    sess.send_line("echo before-exit")
    time.sleep(0.5)  # Let the command finish; prompt is back.
    sess.send("\x04")  # Ctrl+D on an empty line -> exit.
    deadline = time.time() + 5
    exited, status = False, None
    while time.time() < deadline:
        exited, status = sess.try_reap()
        if exited:
            break
        sess.read_available(0.1)
    check("ctrl-d-exits", exited and status == 0,
          f"exited={exited} status={status}")


@register
def test_history_up_arrow(sess, tmp):
    sess.buffer = b""
    sess.send_line("echo hist-uniq-one")
    time.sleep(0.5)
    sess.send_line("echo hist-uniq-two")
    time.sleep(0.5)
    count_before = sess.visible().count("hist-uniq-one")
    sess.send("\x1b[A")  # Up: hist-uniq-two
    time.sleep(0.2)
    sess.send("\x1b[A")  # Up: hist-uniq-one
    time.sleep(0.2)
    sess.send("\r")
    sess.quiesce()
    text = sess.visible()
    count_after = text.count("hist-uniq-one")
    check("history-up", count_after > count_before,
          f"before={count_before} after={count_after} tail={text[-200:]!r}")


@register
def test_foreground_child_gets_ctrl_c(sess, tmp):
    sess.buffer = b""
    sess.send_line("echo ready")
    time.sleep(0.5)
    sess.buffer = b""
    sess.send_line("sleep 10")
    time.sleep(0.6)  # Let the child start and own the terminal.
    sess.send("\x03")  # Should kill the sleeping child, not the shell.
    sess.quiesce()
    sess.send_line("echo still-alive")
    sess.quiesce()
    text = sess.visible()
    check("ctrlc-kills-child", "still-alive\r\n" in text, text[-200:])


@register
def test_foreground_ctrl_z_suspends_child(sess, tmp):
    sess.buffer = b""
    sess.send_line("echo ready")
    time.sleep(0.5)
    sess.buffer = b""
    sess.send_line("sleep 10")
    time.sleep(0.7)  # Child now owns the terminal.
    sess.send("\x1a")  # Ctrl+Z -> child stops, shell regains control.
    sess.quiesce()
    text_after_stop = sess.visible()
    check("stopped-notice", "Stopped" in text_after_stop,
          text_after_stop[-200:])
    sess.send_line("echo resumed")
    sess.quiesce()
    check("shell-usable-after-stop",
          "resumed\r\n" in sess.visible(), sess.visible()[-200:])
    sess.send_line("jobs")
    sess.quiesce()
    jobs_text = sess.visible()
    check("stopped-job-listed",
          "Stopped" in jobs_text and "sleep" in jobs_text,
          jobs_text[-200:])


@register
def test_tab_completion_lists(sess, tmp):
    open(os.path.join(tmp, "unique_file_a.txt"), "w").write("x")
    open(os.path.join(tmp, "unique_file_b.txt"), "w").write("x")
    sess.buffer = b""
    sess.expect("$")
    sess.send("cat un")
    sess.send("\t\t")  # Double-tab: show matches.
    sess.quiesce()
    text = sess.visible()
    check("tab-lists-matches",
          "unique_file_a" in text and "unique_file_b" in text,
          text[-300:])
    sess.send("\x15")  # Ctrl+U: clear the line.
    time.sleep(0.2)


@register
def test_tab_completes_unique_prefix(sess, tmp):
    open(os.path.join(tmp, "single_unique_target.txt"), "w").write("x")
    sess.buffer = b""
    sess.expect("$")
    sess.send("cat single_un")
    sess.send("\t")
    sess.quiesce()
    text = sess.visible()
    check("tab-completes-unique", "single_unique_target.txt" in text,
          text[-300:])
    sess.send("\x15")
    time.sleep(0.2)


@register
def test_background_job_notice(sess, tmp):
    sess.buffer = b""
    sess.send_line("sleep 0.5 &")
    sess.quiesce(quiet_for=0.25)  # Let the notice window pass cleanly.
    sess.buffer = b""
    sess.send_line("sleep 1.5")
    # The notice prints when the background job is reaped after the
    # foreground sleep finishes; a quiesce() would return early during the
    # silent sleep, so read for a fixed window instead.
    sess.read_available(2.5)
    text = sess.visible()
    check("bg-done-notice", "Done" in text, text[-300:])


@register
def test_background_notice_redraws_prompt(sess, tmp):
    sess.expect("$")
    sess.send_line("sleep 0.4 &")
    sess.quiesce(quiet_for=0.2)
    sess.buffer = b""
    sess.read_available(1.2)  # No keys pressed while the job finishes.
    text = sess.visible()
    check("bg-notice-redraws-prompt",
          "Done" in text and "$" in text.split("Done")[-1], text[-200:])


@register
def test_delete_and_modified_keys_do_not_insert_text(sess, tmp):
    sess.expect("$")
    sess.buffer = b""
    # Left, then Delete removes the 'X'; Ctrl+Right is ignored.
    sess.send("echo abX\x1b[D\x1b[3~\x1b[1;5Ccd")
    sess.send("\r")
    sess.quiesce()
    text = sess.visible()
    check("delete-key", "abcd\r\n" in text, text[-200:])


@register
def test_cursor_column_counts_characters_not_bytes(sess, tmp):
    def cursor_column():
        sess.quiesce()
        moves = re.findall(rb"\x1b\[(\d+)C", sess.buffer)
        return int(moves[-1]) if moves else None

    sess.expect("$")
    sess.buffer = b""
    sess.send("echo e")
    ascii_column = cursor_column()
    sess.send("\x15")
    sess.buffer = b""
    sess.send("echo é")  # Two bytes, one column.
    check("utf8-cursor", cursor_column() == ascii_column,
          f"ascii={ascii_column} tail={sess.buffer[-60:]!r}")
    sess.send("\x15")


@register
def test_completion_of_symlinked_command(sess, tmp):
    sess.close()
    bindir = os.path.join(tmp, "bin")
    os.makedirs(bindir, exist_ok=True)
    link = os.path.join(bindir, "zzfoollinkcmd")
    if not os.path.lexists(link):
        os.symlink("/bin/true", link)
    sess = ShellSession(
        env_extra={"PATH": bindir + ":" + os.environ.get("PATH", "")})
    try:
        sess.expect("$")
        sess.send("zzfooll\t")
        # The first Tab scans PATH, which can outlast a quiesce() window.
        found = sess.expect("zzfoollinkcmd", timeout=8)
        check("tab-completes-symlink", found is not None,
              sess.visible()[-200:])
    finally:
        sess.close()


@register
def test_completion_of_command_given_as_path(sess, tmp):
    script = os.path.join(tmp, "runme_unique_script.sh")
    open(script, "w").write("#!/bin/sh\n")
    os.chmod(script, 0o755)
    sess.expect("$")
    sess.buffer = b""
    sess.send("./runme_u\t")
    sess.quiesce()
    check("tab-completes-path-command",
          "./runme_unique_script.sh" in sess.visible(), sess.visible()[-200:])
    sess.send("\x15")


@register
def test_fg_resumes_stopped_job(sess, tmp):
    sess.expect("$")
    sess.send_line("sleep 1.5")
    time.sleep(0.5)
    sess.send("\x1a")
    sess.quiesce()
    check("fg-job-stopped", "Stopped" in sess.visible(), sess.visible()[-200:])
    sess.buffer = b""
    sess.send_line("fg")
    sess.read_available(2.5)  # The resumed sleep runs to completion.
    sess.buffer = b""
    sess.send_line("jobs")
    sess.send_line("echo jobs-listed")
    sess.expect("jobs-listed\r\n")
    check("fg-resumes-and-finishes", "Stopped" not in sess.visible(),
          sess.visible()[-200:])


def main():
    if not os.path.exists(FOOL):
        print(f"FOOL binary not found at {FOOL}; build it first.",
              file=sys.stderr)
        return 1
    tmp = tempfile.mkdtemp(prefix="fool_pty_test_")
    old_cwd = os.getcwd()
    os.chdir(tmp)
    # The shell keeps its history file in $HOME; never touch the real one.
    os.environ["HOME"] = tmp
    try:
        for fn in TESTS:
            print(f"* {fn.__name__}")
            sess = ShellSession()
            try:
                fn(sess, tmp)
            except Exception as exc:  # noqa: BLE001
                check(fn.__name__, False, f"EXC {exc}")
            finally:
                sess.close()
    finally:
        os.chdir(old_cwd)
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{PASS} checks passed, {FAIL} failed")
    if FAILURES:
        print("Failed:", ", ".join(FAILURES))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
