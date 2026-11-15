#!/bin/bash
# userland/tests/rexx/run.sh - builds userland/apps/rexx.c for this machine
# and runs test.cmd: its output must be test.out's, its exit code 3
set -u
T="$(cd "$(dirname "$0")" && pwd)"; OUT="${TMPDIR:-/tmp}/rexx-test.$$"
mkdir -p "$OUT" && cd "$OUT" || exit 1
cc -std=c11 -O2 -o rexx "$T/../../apps/rexx.c" -lm || exit 1
REXX_SHELL=/bin/sh ./rexx "$T/test.cmd" > out.txt 2>&1; st=$?
if [ $st = 3 ] && diff -u "$T/test.out" out.txt; then echo "rexx: ok"; r=0; else echo "rexx: FAIL (exit $st)"; r=1; fi
cd / && rm -rf "$OUT"
exit $r
