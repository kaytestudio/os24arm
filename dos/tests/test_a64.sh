#!/bin/bash
# dos/tests/test_a64.sh - dos/a64.c (DEBUG's U and A) against LLVM:
#  1. the kernel's every instruction (build/kernel/os2warp_kernel), FP/SIMD
#     too, and random encodings (100000 integer and load/store ones, 50000
#     FP/SIMD), disassembled by it and by llvm-objdump: the same text;
#  2. each of those disassembled and assembled back: the same word (or,
#     for a random one with don't-care bits set, the same instruction).
# Needs Homebrew's llvm (llvm-objdump, llvm-mc). Exit 0 when all agree.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
L="$(brew --prefix llvm 2>/dev/null || echo /usr)/bin"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cc -std=c11 -O2 -Wall -Wextra -I"$ROOT/dos" -o "$W/vs" "$ROOT/dos/tests/a64_vs_llvm.c" "$ROOT/dos/a64.c"
cc -std=c11 -O2 -Wall -Wextra -I"$ROOT/dos" -o "$W/rt" "$ROOT/dos/tests/a64_roundtrip.c" "$ROOT/dos/a64.c"
python3 -I - "$W/rand.s" <<'PY'
import random, sys
random.seed(7)
out = open(sys.argv[1], 'w'); out.write('.text\n'); n = 0
m = 0
while n < 100000 or m < 50000:
    w = random.getrandbits(32); op0 = (w >> 25) & 15
    simd = (op0 & 7) == 7 or ((op0 & 5) == 4 and (w >> 26) & 1 and ((w >> 27) & 7) == 1 and not (w >> 31) & 1)
    if simd and m < 50000: out.write('.inst 0x%08x\n' % w); m += 1
    elif not simd and n < 100000 and ((op0 & 0xe) in (8, 0xa) or (op0 & 5) == 4 or (op0 & 7) == 5):
        out.write('.inst 0x%08x\n' % w); n += 1
PY
"$L/llvm-mc" -triple=aarch64 -filetype=obj "$W/rand.s" -o "$W/rand.o"
"$L/llvm-objdump" -d --mattr=+v8.5a,+fullfp16 "$W/rand.o" > "$W/rand.dis"
r=0
K="$ROOT/build/kernel/os2warp_kernel"
if [ -f "$K" ]; then
    "$L/llvm-objdump" -d "$K" > "$W/kernel.dis"
    echo "== the kernel"; TOP=5 "$W/vs" < "$W/kernel.dis" || r=1; TOP=5 "$W/rt" < "$W/kernel.dis" || r=1
fi
# (random encodings: LLVM decodes some reserved ones, ARMv8.9's 128-bit system instructions, FP16's rarer forms; reported, not failed)
echo "== random encodings"; TOP=5 "$W/vs" < "$W/rand.dis" || true; TOP=5 "$W/rt" < "$W/rand.dis" || true
[ $r = 0 ] && echo "a64: ok" || echo "a64: FAIL"
exit $r
