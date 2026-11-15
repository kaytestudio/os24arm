#!/bin/bash
# userland/os2x86/test/run-tests.sh - builds os2x86 for this machine and
# runs os2test.c (made an x86 LX .EXE by elf2lx.py) through it.
# Output in build/os2x86-test/. Exit status: the test's.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
T="$ROOT/userland/os2x86"
OUT="$ROOT/build/os2x86-test"
LLVM="$(brew --prefix llvm 2>/dev/null || echo /usr)/bin"
mkdir -p "$OUT"
cc -std=c11 -O2 -Wall -Wextra -pthread -o "$OUT/os2x86" "$T/os2x86.c" "$T/cpu.c"
"$LLVM/clang" --target=i386-unknown-none-elf -march=pentiumpro -mno-sse -mno-mmx \
    -ffreestanding -fno-pic -fno-builtin -O1 -c "$T/test/os2test.c" -o "$OUT/os2test.o"
python3 "$T/test/elf2lx.py" "$OUT/os2test.o" "$OUT/OS2TEST.EXE"
cd "$OUT"
OS2TEST=yes ./os2x86 OS2TEST.EXE one "two three" | grep -E "^FAIL|passed|failed"
st=$?
# real OpenWatcom programs, when OpenWatcom is there (WATCOM, or build/os2x86-test/ow)
W="${WATCOM:-$ROOT/build/os2x86-test/ow}"
if [ -x "$W/armo64/wcl386" ] || [ -x "$W/binl64/wcl386" ]; then
    B="$W/armo64"; [ -x "$B/wcl386" ] || B="$W/binl64"
    mkdir -p "$OUT/ow" && cd "$OUT/ow"
    export WATCOM="$W" PATH="$B:$PATH" INCLUDE="$W/h:$W/h/os2"
    S="$T/test/ow"
    wcl386 -q -za99 -bt=os2 -l=os2v2 -bm "$S/owtest.c" >/dev/null
    wcl386 -q -xs -bt=os2 -l=os2v2 "$S/cpptest.cpp" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2 "$S/conio.c" >/dev/null
    wcl386 -q -bt=os2 -bd -l=os2v2_dll "$S/mydll.c" -fe=mydll.dll >/dev/null
    wlib -q -n mydll.lib +mydll.dll >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2 "$S/usedll.c" mydll.lib >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2 "$S/mouse.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmhello.c" >/dev/null
    owfail=0
    echo "== OpenWatcom programs"
    ../os2x86 owtest.exe </dev/null | grep -E "^FAIL|passed|failed" || owfail=1
    ../os2x86 cpptest.exe </dev/null | grep -E "^FAIL|passed|failed" || owfail=1
    printf 'xy' | ../os2x86 conio.exe >/dev/null && echo "conio: ok" || { echo "conio: FAIL"; owfail=1; }
    ../os2x86 usedll.exe | grep -q "through the pointer: 1060" && echo "DLL: ok" || { echo "DLL: FAIL"; owfail=1; }
    printf '\033[<0;10;5Mk\033[<32;12;6M\033[<0;12;6m\033[<2;3;3M' | ../os2x86 mouse.exe | grep "mouse:" || owfail=1
    # Presentation Manager, headless: a script for its input, a picture to check
    D="$ROOT/build/userland/dl/dejavu-fonts-ttf-2.37.tar.bz2"
    [ -f fonts/DejaVuSans.ttf ] || { [ -f "$D" ] && mkdir -p fonts && tar xjf "$D" -C fonts --strip-components=2 dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf; }
    pmout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf \
        OS2X86_PM_SCRIPT="sleep 150; sleep 150; sleep 150; sleep 150; click 100 100; key ab; shot pm.ppm; key q; esc; key q; key y" \
        ../os2x86 pmhello.exe 2>/dev/null)"
    if echo "$pmout" | grep -q "WM_TIMER x3" && echo "$pmout" | grep -q "done: 1 clicks, 4 keys" &&
       python3 -I - pm.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3); w, h = map(int, d[1].split()); px = d[3]
at = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
red = at(60, h - 1 - 40) == (255, 0, 0)                          # GpiBox, PM's coordinates
blue = any(at(x, y)[2] > 200 and at(x, y)[0] < 100 for x in range(20, 200) for y in range(25, 45))   # the blue text
sys.exit(0 if red and blue else 1)
PY
    then echo "PM: ok"; else echo "PM: FAIL"; echo "$pmout"; owfail=1; fi
    [ "$owfail" = 0 ] || st=1
else
    echo "(no OpenWatcom: its programs not tried)"
fi
exit $st
