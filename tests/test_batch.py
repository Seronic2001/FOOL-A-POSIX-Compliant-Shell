#!/usr/bin/env python3
"""Batch-mode integration tests for the FOOL shell.

Each test feeds a script on stdin and asserts on the captured output.
Run via tests/run_tests.sh or `make test`.
"""

import os
import shutil
import subprocess
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


def run_shell(script, timeout=15, env_extra=None):
    """Run FOOL in batch mode with `script` as stdin. Returns (out, err, rc)."""
    env = dict(os.environ)
    env.setdefault("TERM", "dumb")
    if env_extra:
        env.update(env_extra)
    proc = subprocess.run(
        [FOOL],
        input=script.encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        env=env,
    )
    return proc.stdout.decode(errors="replace"), proc.stderr.decode(
        errors="replace"
    ), proc.returncode


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  [ PASS ] {name}")
    else:
        FAIL += 1
        FAILURES.append(name)
        print(f"  [ FAIL ] {name}  {detail}")


def test(name):
    def deco(fn):
        fn._is_test = True
        return fn

    return name  # simple decorator passthrough keeping readability


TESTS = []


def register(fn):
    TESTS.append(fn)
    return fn


# ---------------------------------------------------------------- basics

@register
def test_echo_and_semicolons(tmp):
    out, err, rc = run_shell("echo one; echo two; echo three\nexit\n")
    check("echo-semicolon-order", "one\ntwo\nthree" in out.replace("\n\n", "\n"),
          f"out={out!r}")
    # Strict order check:
    lines = [l for l in out.splitlines() if l.strip()]
    check("echo-semicolon-lines", lines == ["one", "two", "three"], f"{lines!r}")


@register
def test_pwd(tmp):
    out, err, rc = run_shell("pwd\nexit\n")
    check("pwd", out.strip() == tmp, f"{out.strip()!r} != {tmp!r}")


@register
def test_exit_code_of_shell(tmp):
    out, err, rc = run_shell("exit\n")
    check("exit-rc", rc == 0, f"rc={rc}")


@register
def test_quotes_preserve_operators(tmp):
    out, err, rc = run_shell('echo "a|b;c&d"\nexit\n')
    check("quoted-operators", out.strip() == "a|b;c&d", f"{out.strip()!r}")


@register
def test_empty_lines_ignored(tmp):
    out, err, rc = run_shell("\n\n   \nexit\n")
    check("empty-lines", rc == 0, f"rc={rc} err={err!r}")


# ---------------------------------------------------------- redirections

@register
def test_output_redirect(tmp):
    out, err, rc = run_shell(
        "echo hello > r1.txt; cat r1.txt; exit\n", timeout=15
    )
    check("redirect-out", "hello" in out and "hello" not in
          [l for l in out.splitlines() if l.strip()][0:0], f"{out!r}")
    with open(os.path.join(tmp, "r1.txt")) as f:
        check("redirect-out-file", f.read().strip() == "hello")


@register
def test_append_redirect(tmp):
    run_shell("echo a > r2.txt\necho b >> r2.txt\nexit\n")
    with open(os.path.join(tmp, "r2.txt")) as f:
        check("redirect-append", f.read() == "a\nb\n")


@register
def test_stderr_redirect(tmp):
    out, err, rc = run_shell("ls /definitely_missing_xyz 2> r3.txt; exit\n")
    with open(os.path.join(tmp, "r3.txt")) as f:
        content = f.read()
    check("redirect-stderr", "definitely_missing" in content and
          "definitely_missing" not in err, f"file={content!r} err={err!r}")


@register
def test_stderr_append(tmp):
    run_shell("ls /nope1 2> r4.txt\nls /nope2 2>> r4.txt\nexit\n")
    with open(os.path.join(tmp, "r4.txt")) as f:
        content = f.read()
    check("redirect-stderr-append",
          "nope1" in content and "nope2" in content, f"{content!r}")


@register
def test_input_redirect(tmp):
    with open(os.path.join(tmp, "in5.txt"), "w") as f:
        f.write("line-one\n")
    out, err, rc = run_shell("cat < in5.txt\nexit\n")
    check("redirect-in", out.strip() == "line-one", f"{out!r}")


@register
def test_builtin_redirect_child_flush(tmp):
    # Regression: forked children ran builtins writing to buffered std::cout
    # and then called _exit(), discarding the buffer. Verify content lands.
    out, err, rc = run_shell("pwd > r6.txt\nexit\n")
    with open(os.path.join(tmp, "r6.txt")) as f:
        check("child-builtin-flush", f.read().strip() == tmp, f"{f.read()!r}")


# ------------------------------------------------------------ pipelines

@register
def test_pipeline_two_stage(tmp):
    out, err, rc = run_shell("printf 'b\\na\\nb\\n' | sort\nexit\n")
    check("pipeline-sort", out == "a\nb\nb\n", f"{out!r}")


@register
def test_pipeline_builtin_end(tmp):
    out, err, rc = run_shell("echo hello | cat\nexit\n")
    check("pipeline-builtin", out.strip() == "hello", f"{out!r}")


@register
def test_pipeline_three_stage(tmp):
    out, err, rc = run_shell(
        "printf 'x\\ny\\nx\\n' | sort | head -2\nexit\n")
    check("pipeline-3stage", out == "x\nx\n", f"{out!r}")


@register
def test_pipeline_with_semicolons(tmp):
    out, err, rc = run_shell(
        "echo a | cat; echo b | cat\nexit\n")
    lines = [l for l in out.splitlines() if l.strip()]
    check("pipeline-semicolon", lines == ["a", "b"], f"{lines!r}")


@register
def test_pipeline_stateful_builtin_rejected(tmp):
    # cd/exit mutate shell state, so they are rejected inside pipelines;
    # pwd is child-safe and must work.
    out, err, rc = run_shell("pwd | cat\ncd / | cat\nexit\n")
    check("pipeline-stateful-rejected",
          "cannot be used in a pipeline" in err and out.strip() == tmp,
          f"err={err!r} out={out!r}")


# ---------------------------------------------------------- globbing

@register
def test_glob_expansion(tmp):
    for name in ("g1.txt", "g2.txt", "g3.dat"):
        open(os.path.join(tmp, name), "w").write("x")
    out, err, rc = run_shell("echo *.txt\nexit\n")
    check("glob", out.split() == ["g1.txt", "g2.txt"], f"{out!r}")


@register
def test_glob_no_match_stays_literal(tmp):
    out, err, rc = run_shell("echo *.zzz\nexit\n")
    check("glob-nomatch", out.strip() == "*.zzz", f"{out!r}")


@register
def test_question_mark_glob(tmp):
    open(os.path.join(tmp, "q1.txt"), "w").write("x")
    out, err, rc = run_shell("echo q?.txt\nexit\n")
    check("glob-qmark", out.strip() == "q1.txt", f"{out!r}")


# ------------------------------------------------------ builtins basics

@register
def test_cd_and_pwd(tmp):
    out, err, rc = run_shell("cd /\npwd\nexit\n")
    check("cd-pwd", out.strip().endswith("/"), f"{out!r}")


@register
def test_cd_missing_dir(tmp):
    out, err, rc = run_shell("cd /no_such_dir_xyz\npwd\nexit\n")
    check("cd-missing", ("No such file" in err) or
          ("No such file" in out), f"err={err!r}")


@register
def test_ls_lists_files(tmp):
    open(os.path.join(tmp, "l1.txt"), "w").write("x")
    out, err, rc = run_shell("ls\nexit\n")
    check("ls", "l1.txt" in out, f"{out!r}")


@register
def test_ls_missing_path(tmp):
    out, err, rc = run_shell("ls /no_such_dir_qq\nexit\n")
    check("ls-missing", "No such file" in err or "No such file" in out,
          f"err={err!r} out={out!r}")


@register
def test_history_builtin(tmp):
    out, err, rc = run_shell("echo aaa\necho bbb\nhistory 2\nexit\n")
    check("history-shows-recent",
          "aaa" in out and "bbb" in out and "history" in out, f"{out!r}")


@register
def test_search_builtin(tmp):
    open(os.path.join(tmp, "needle.txt"), "w").write("x")
    out, err, rc = run_shell("search needle.txt\nexit\n")
    check("search-found", out.strip() == "True", f"out={out!r} err={err!r}")


@register
def test_search_recursive(tmp):
    os.makedirs(os.path.join(tmp, "sub7"), exist_ok=True)
    open(os.path.join(tmp, "sub7", "deep7.txt"), "w").write("x")
    out, err, rc = run_shell("search deep7.txt\nexit\n")
    check("search-recursive", out.strip() == "True", f"{out!r}")


@register
def test_echo_n_flag(tmp):
    out, err, rc = run_shell("echo -n xyz\nexit\n")
    check("echo-n", out.strip() == "xyz" and not out.endswith("xyz\n\n"),
          f"{out!r}")


# ---------------------------------------------------------- job control

@register
def test_background_job_registered(tmp):
    out, err, rc = run_shell(
        "sleep 1 &\njobs\nsleep 1.5\nexit\n", timeout=20)
    check("bg-registered", "[1]" in out and "sleep" in out, f"{out!r}")


@register
def test_background_job_completion_notice(tmp):
    out, err, rc = run_shell(
        "sleep 0.3 &\nsleep 1\njobs\nexit\n", timeout=20)
    # After it finishes, jobs must NOT list it anymore (reaped).
    lines = [l for l in out.splitlines() if l.startswith("[1]")]
    running = [l for l in lines if "Running" in l]
    check("bg-reaped", len(running) <= 1, f"{lines!r}")


@register
def test_background_then_exit_terminates_children(tmp):
    start = time.time()
    out, err, rc = run_shell("sleep 30 &\nexit\n", timeout=20)
    elapsed = time.time() - start
    check("exit-kills-bg", elapsed < 10, f"elapsed={elapsed:.1f}s")


@register
def test_multiple_background_jobs(tmp):
    out, err, rc = run_shell(
        "sleep 1 &\nsleep 1 &\nsleep 1 &\njobs\nsleep 2\nexit\n",
        timeout=25)
    ids = [l.split("]")[0] + "]" for l in out.splitlines()
           if l.startswith("[") and "Running" in l]
    check("bg-multiple", len(ids) >= 3, f"{out!r}")


# ------------------------------------------------------- error handling

@register
def test_syntax_error_leading_pipe(tmp):
    out, err, rc = run_shell("| ls\nexit\n")
    check("syntax-err-pipe", "syntax error" in err, f"err={err!r}")


@register
def test_syntax_error_double_amp(tmp):
    out, err, rc = run_shell("a && b\nexit\n")
    check("syntax-err-amp", "syntax error" in err, f"err={err!r}")


@register
def test_unterminated_quote_batch_continues(tmp):
    # A line with an open quote continues onto the next line in batch mode.
    out, err, rc = run_shell('echo "a\nb"\nexit\n')
    check("multiline-quote", "a\nb" in out, f"out={out!r} err={err!r}")


@register
def test_command_not_found(tmp):
    out, err, rc = run_shell("no_such_command_zz\nexit\n")
    check("cmd-not-found", ("not found" in err) or ("No such file" in err),
          f"err={err!r}")


@register
def test_signal_ignored_does_not_kill_shell(tmp):
    # SIGQUIT (Ctrl+\) is ignored by the shell itself.
    out, err, rc = run_shell("echo alive\nexit\n")
    check("shell-alive", "alive" in out and rc == 0, f"out={out!r}")


# ---------------------------------------------------------------- runner

def main():
    if not os.path.exists(FOOL):
        print(f"FOOL binary not found at {FOOL}; build it first.", file=sys.stderr)
        return 1
    tmp = tempfile.mkdtemp(prefix="fool_test_")
    try:
        old_cwd = os.getcwd()
        for fn in TESTS:
            print(f"* {fn.__name__}")
            # Each test gets a fresh subdirectory so tests cannot pollute
            # each other's files (important for glob tests).
            test_dir = os.path.join(tmp, fn.__name__)
            os.makedirs(test_dir, exist_ok=True)
            os.chdir(test_dir)
            try:
                fn(test_dir)
            except subprocess.TimeoutExpired:
                check(fn.__name__, False, "TIMEOUT")
            except Exception as exc:  # noqa: BLE001
                check(fn.__name__, False, f"EXC {exc}")
        os.chdir(old_cwd)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{PASS} checks passed, {FAIL} failed")
    if FAILURES:
        print("Failed:", ", ".join(FAILURES))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
