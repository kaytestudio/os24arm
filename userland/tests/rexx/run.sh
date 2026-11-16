#!/bin/bash
# userland/tests/rexx/run.sh - builds userland/apps/rexx.c for this machine
# and runs test.cmd (its output must be test.out's, its exit code 3) and
# util.cmd (RexxUtil: util.out's) and traps.cmd (conditions: traps.out's) and
# ext.cmd (external routines: ext.out's) and queue.cmd (queue.out's)
set -u
T="$(cd "$(dirname "$0")" && pwd)"; OUT="${TMPDIR:-/tmp}/rexx-test.$$"
mkdir -p "$OUT" && cd "$OUT" || exit 1
cc -std=c11 -O2 -o rexx "$T/../../apps/rexx.c" -lm || exit 1
REXX_SHELL=/bin/sh ./rexx "$T/test.cmd" > out.txt 2>&1; st=$?
if [ $st = 3 ] && diff -u "$T/test.out" out.txt; then echo "rexx: ok"; r=0; else echo "rexx: FAIL (exit $st)"; r=1; fi
./rexx "$T/util.cmd" > util.txt 2>&1
if diff -u "$T/util.out" util.txt; then echo "rexx RexxUtil: ok"; else echo "rexx RexxUtil: FAIL"; r=1; fi
REXX_SHELL=/bin/sh ./rexx "$T/traps.cmd" > traps.txt 2>/dev/null
if diff -u "$T/traps.out" traps.txt; then echo "rexx conditions: ok"; else echo "rexx conditions: FAIL"; r=1; fi
./rexx "$T/ext.cmd" > ext.txt 2>&1
if diff -u "$T/ext.out" ext.txt; then echo "rexx external routines: ok"; else echo "rexx external routines: FAIL"; r=1; fi
REXX_SHELL=/bin/sh ./rexx "$T/queue.cmd" > queue.txt 2>&1
if diff -u "$T/queue.out" queue.txt; then echo "rexx queue, NOTREADY: ok"; else echo "rexx queue, NOTREADY: FAIL"; r=1; fi
cd / && rm -rf "$OUT"
exit $r
