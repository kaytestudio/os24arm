#!/bin/bash
# userland/os2x86/test/run-tests.sh - builds os2x86 for this machine and
# runs os2test.c (made an x86 LX .EXE by elf2lx.py) through it.
# Output in build/os2x86-test/. Exit status: the test's.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
T="$ROOT/userland/os2x86"
OUT="$ROOT/build/os2x86-test"
LLVM="$(brew --prefix llvm 2>/dev/null || echo /usr)/bin"
mkdir -p "$OUT"
cc -std=c11 -O2 -Wall -Wextra -o "$OUT/os2x86" "$T/os2x86.c" "$T/cpu.c"
"$LLVM/clang" --target=i386-unknown-none-elf -march=pentiumpro -mno-sse -mno-mmx \
    -ffreestanding -fno-pic -fno-builtin -O1 -c "$T/test/os2test.c" -o "$OUT/os2test.o"
python3 "$T/test/elf2lx.py" "$OUT/os2test.o" "$OUT/OS2TEST.EXE"
cd "$OUT"
OS2TEST=yes ./os2x86 OS2TEST.EXE one "two three"
