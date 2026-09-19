#!/usr/bin/env bash
# Runs the full FOOL test suite: unit tests, batch-mode integration tests,
# and interactive pty tests. Exits non-zero if anything fails.
set -u

cd "$(dirname "$0")/.." || exit 1

overall=0

echo "=== Building FOOL and test binaries ==="
if ! make -s all tests; then
  echo "BUILD FAILED"
  exit 1
fi
echo

echo "=== Unit tests ==="
# Unit tests must never read the live terminal: feed them /dev/null so a
# stray stdin read can never hang the suite on an interactive terminal.
./build/test_parser < /dev/null || overall=1
echo
./build/test_builtins < /dev/null || overall=1
echo

echo "=== Batch-mode integration tests ==="
python3 tests/test_batch.py || overall=1
echo

echo "=== Interactive (pty) tests ==="
python3 tests/test_interactive.py || overall=1
echo

if [ "$overall" -eq 0 ]; then
  echo "ALL TEST SUITES PASSED"
else
  echo "SOME TESTS FAILED"
fi
exit "$overall"
