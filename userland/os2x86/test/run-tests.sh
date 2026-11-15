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
    wrc -q -r -bt=os2 -i="$S" "$S/pmctl.rc" -fo=pmctl.res >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm -i="$S" "$S/pmctl.c" >/dev/null && wrc -q -bt=os2 pmctl.res pmctl.exe >/dev/null
    wrc -q -r -bt=os2 -i="$S" "$S/pmmore.rc" -fo=pmmore.res >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm -i="$S" "$S/pmmore.c" >/dev/null && wrc -q -bt=os2 pmmore.res pmmore.exe >/dev/null
    wrc -q -r -bt=os2 -i="$S" "$S/pmedit.rc" -fo=pmedit.res >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm -i="$S" "$S/pmedit.c" >/dev/null && wrc -q -bt=os2 pmedit.res pmedit.exe >/dev/null
    python3 -I "$S/mkbmp.py" . && wrc -q -r -bt=os2 -i="$S" "$S/pmbmp.rc" -fo=pmbmp.res >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm -i="$S" "$S/pmbmp.c" >/dev/null && wrc -q -bt=os2 pmbmp.res pmbmp.exe >/dev/null
    # (a real OS/2 icon: OpenWatcom's own sample, from its snapshot's archive if it's here; else mkbmp.py's again)
    [ -f owobject.ico ] || { [ -f "$ROOT/build/os2x86-test/dl/ow.tar.xz" ] && mkdir -p ow-src &&
        tar xJf "$ROOT/build/os2x86-test/dl/ow.tar.xz" -C ow-src ./samples/os2/som/wps/owobject.ico 2>/dev/null &&
        cp ow-src/samples/os2/som/wps/owobject.ico .; } || cp trans.ico owobject.ico
    wrc -q -r -bt=os2 -i="$S" "$S/pmicon.rc" -fo=pmicon.res >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm -i="$S" "$S/pmicon.c" >/dev/null && wrc -q -bt=os2 pmicon.res pmicon.exe >/dev/null
    wrc -q -r -bt=os2 -i="$S" "$S/pmfont.rc" -fo=pmfont.res >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm -i="$S" "$S/pmfont.c" >/dev/null && wrc -q -bt=os2 pmfont.res pmfont.exe >/dev/null
    wrc -q -r -bt=os2 -i="$S" "$S/pmbook.rc" -fo=pmbook.res >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm -i="$S" "$S/pmbook.c" >/dev/null && wrc -q -bt=os2 pmbook.res pmbook.exe >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmcnr.c" >/dev/null
    owfail=0
    echo "== OpenWatcom programs"
    ../os2x86 owtest.exe </dev/null | grep -E "^FAIL|passed|failed" || owfail=1
    ../os2x86 cpptest.exe </dev/null | grep -E "^FAIL|passed|failed" || owfail=1
    printf 'xy' | ../os2x86 conio.exe >/dev/null && echo "conio: ok" || { echo "conio: FAIL"; owfail=1; }
    ../os2x86 usedll.exe | grep -q "through the pointer: 1060" && echo "DLL: ok" || { echo "DLL: FAIL"; owfail=1; }
    printf '\033[<0;10;5Mk\033[<32;12;6M\033[<0;12;6m\033[<2;3;3M' | ../os2x86 mouse.exe | grep "mouse:" || owfail=1
    # Presentation Manager, headless: a script for its input, a picture to check
    D="$ROOT/build/userland/dl/dejavu-fonts-ttf-2.37.tar.bz2"
    for f in DejaVuSans DejaVuSans-Bold DejaVuSansMono DejaVuSansMono-Bold; do
        [ -f fonts/$f.ttf ] || { [ -f "$D" ] && mkdir -p fonts && tar xjf "$D" -C fonts --strip-components=2 dejavu-fonts-ttf-2.37/ttf/$f.ttf; }
    done
    pmout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf \
        OS2X86_PM_SCRIPT="sleep 150; sleep 150; sleep 150; sleep 150; click 100 100; key ab; shot pm.ppm; key q; esc; key q; key y" \
        ../os2x86 pmhello.exe 2>/dev/null)"
    if grep -q "WM_TIMER x3" <<< "$pmout" && grep -q "done: 1 clicks, 4 keys" <<< "$pmout" &&
       python3 -I - pm.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3); w, h = map(int, d[1].split()); px = d[3]
at = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
red = at(60, h - 1 - 40) == (255, 0, 0)                          # GpiBox, PM's coordinates
blue = any(at(x, y)[2] > 200 and at(x, y)[0] < 100 for x in range(20, 200) for y in range(25, 45))   # the blue text
sys.exit(0 if red and blue else 1)
PY
    then echo "PM: ok"; else echo "PM: FAIL"; echo "$pmout"; owfail=1; fi
    # menus, a dialog from its template, its controls (by the mouse, then the keys), a string table
    ctlout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf \
        OS2X86_PM_SCRIPT="click 10 10; click 30 36; click 150 30; key abc; click 30 65; click 40 140; click 220 127; click 50 230; \
click 10 10; click 30 36; key xyz; enter; click 10 10; click 30 36; key q; esc; click 55 10; click 60 36; enter; click 10 10; click 30 68" \
        ../os2x86 pmctl.exe 2>&1)"
    if grep -q 'WinDlgBox: 1, name "Warpabc", check 1, radio 2, item 2' <<< "$ctlout" &&
       grep -q 'WinDlgBox: 1, name "Warpxyz", check 0, radio 1, item -1' <<< "$ctlout" &&
       grep -q 'WinDlgBox: 2, name "Warpxyz"' <<< "$ctlout" && grep -q "string: Hello from a string table" <<< "$ctlout" &&
       grep -q "IDM_EXIT" <<< "$ctlout"
    then echo "PM controls: ok"; else echo "PM controls: FAIL"; echo "$ctlout"; owfail=1; fi
    # scroll bars (the frame's: arrow, track, thumb dragged; a dialog's), a combo box, accelerators, the menu by keys
    moreout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf \
        OS2X86_PM_SCRIPT="click 471 28; click 471 312; click 471 200; drag 471 120 471 250; vkey f5; ctrl d; click 236 32; click 120 80; \
click 250 130; click 220 130; enter; vkey f10; vkey down; vkey enter; alt f; key x" ../os2x86 pmmore.exe 2>&1)"
    if grep -q "top 11" <<< "$moreout" && grep -q "top 35" <<< "$moreout" && grep -q "IDM_REFRESH (source 3)" <<< "$moreout" &&
       grep -q 'combo "Cherry", item 2' <<< "$moreout" && grep -q "WM_HSCROLL 4 -> 9" <<< "$moreout" &&
       grep -q "IDM_REFRESH (source 2)" <<< "$moreout" && grep -q "IDM_EXIT" <<< "$moreout"
    then echo "PM scroll bars, combo box, keys: ok"; else echo "PM scroll bars, combo box, keys: FAIL"; echo "$moreout"; owfail=1; fi
    # the multi-line edit control and spin buttons
    editout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf \
        OS2X86_PM_SCRIPT="click 200 40; key !; enter; key Line3; click 93 200; click 93 200; click 30 207; vkey down; click 236 200; click 50 250" \
        ../os2x86 pmedit.exe 2>&1)"
    if grep -q 'WinDlgBox 1: text "HelloRNWorld!RNLine3", export "HelloRNWorld!RNLine3", 3 lines, some changes, spin 4, day Tue' <<< "$editout" &&
       grep -q "line 2 is 6 long" <<< "$editout"
    then echo "PM MLE, spin buttons: ok"; else echo "PM MLE, spin buttons: FAIL"; echo "$editout"; owfail=1; fi
    # bitmaps: three formats from resources, stretched, a memory PS blitted; the pixels checked
    bmpout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="shot bmp.ppm; close" ../os2x86 pmbmp.exe 2>&1)"
    if grep -q "b1 64x48, b2 32x32, b3 16x16" <<< "$bmpout" && grep -q "(5,5) ffff00, (0,0) 000000" <<< "$bmpout" &&
       python3 -I - bmp.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3); w, h = map(int, d[1].split()); px = d[3]
at = lambda x, y: tuple(px[((h - 1 - y) * w + x) * 3:((h - 1 - y) * w + x) * 3 + 3])   # PM's coordinates, y up
ok = at(10 + 5, 10 + 5) == (20, 25, 128) and at(10 + 63, 10 + 47) == (252, 235, 128)    # the gradient, its corners
ok = ok and at(102, 12) == (255, 0, 0) and at(102 + 16, 12) == (0, 0, 255)               # the checkerboard, stretched
ok = ok and at(205, 15) == (0, 200, 0) and at(260, 20) == (255, 255, 0) and at(250, 10) == (0, 0, 0)
sys.exit(0 if ok else 1)
PY
    then echo "PM bitmaps: ok"; else echo "PM bitmaps: FAIL"; echo "$bmpout"; owfail=1; fi
    # icons: a real one (array, colour), masks (transparent, inverting), DP_MINI, a system icon, SS_ICON, a message box's
    icoout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="shot ico.ppm; key m; enter; close" ../os2x86 pmicon.exe 2>&1)"
    if grep -q "pointers yes yes yes" <<< "$icoout" && grep -q "WinMessageBox 1" <<< "$icoout" &&
       python3 -I - ico.ppm owobject.ico <<'PY'
import sys, struct
d = open(sys.argv[1], 'rb').read().split(b'\n', 3); w, h = map(int, d[1].split()); px = d[3]
at = lambda x, y: tuple(px[((h - 1 - y) * w + x) * 3:((h - 1 - y) * w + x) * 3 + 3])
ok = at(60, 10) == (204, 204, 204) and at(76, 18) == (0, 255, 0) and at(75, 25) == (51, 51, 51)   # transparent, green, inverted
ok = ok and at(266, 18) == (0, 255, 0) and at(250, 10) == (204, 204, 204)                         # the SS_ICON
# the real icon: its colours as the file has them (the device-independent image, 4 bits, bottom row first)
f = open(sys.argv[2], 'rb').read()
o = 14 if f[:2] == b'BA' else 0
cm = struct.unpack_from('<I', f, o + 14)[0]
c = o + 14 + cm + 2 * (3 if cm == 12 else 4)
coff = struct.unpack_from('<I', f, c + 10)[0]
cinfo = c + 14; ccm = struct.unpack_from('<I', f, cinfo)[0]
bpp = struct.unpack_from('<H', f, cinfo + (10 if ccm == 12 else 14))[0]
pal = cinfo + ccm; pe = 3 if ccm == 12 else 4
for (x, y) in ((5, 5), (16, 16), (20, 26), (10, 28)):
    if bpp != 4: break
    b = f[coff + y * 16 + x // 2]; i = (b >> 4) if x % 2 == 0 else (b & 15)
    want = (f[pal + pe * i + 2], f[pal + pe * i + 1], f[pal + pe * i])
    ok = ok and at(10 + x, 10 + y) == want
sys.exit(0 if ok else 1)
PY
    then echo "PM icons: ok"; else echo "PM icons: FAIL"; echo "$icoout"; owfail=1; fi
    # fonts: GpiQueryFonts, logical fonts (bitmap by lMatch, outline by the char box), metrics, presentation parameters
    fontout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="close" ../os2x86 pmfont.exe 2>&1)"
    if grep -q "12 point bitmap at [0-9]" <<< "$fontout" && grep -q "GpiCreateLogFont 2 2" <<< "$fontout" &&
       awk '/^mono:/ { exit !($3 == "1," && $5 == $8) }' <<< "$fontout" && grep -q "outline bold: baseline extent 40, bold 1" <<< "$fontout" &&
       grep -q 'static font "14.Helv"' <<< "$fontout" && grep -q 'dialog item font "18.Helv Bold"' <<< "$fontout" &&
       awk '/^default:/ { exit !($3 > $6) }' <<< "$fontout"
    then echo "PM fonts: ok"; else echo "PM fonts: FAIL"; echo "$fontout"; owfail=1; fi
    # a notebook (pages from dialog resources, tabs, the page arrows) and a slider (shaft, buttons, keys, the arm dragged)
    bookout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf \
        OS2X86_PM_SCRIPT="click 213 183; click 325 183; vkey right; drag 265 183 82 183; click 100 12; click 150 153; key Warp; click 449 305; close" \
        ../os2x86 pmbook.exe 2>&1)"
    if [ "$(grep '^SLN_CHANGE' <<< "$bookout" | tr -d '\r' | tr '\n' ' ')" = "SLN_CHANGE 3 SLN_CHANGE 7 SLN_CHANGE 8 SLN_CHANGE 9 SLN_CHANGE 2 " ] &&
       grep -q "BKN_PAGESELECTED -> page 2" <<< "$bookout" && grep -q 'done: slider 2, name "Warp", on page 1, 2 selections' <<< "$bookout"
    then echo "PM notebook, slider: ok"; else echo "PM notebook, slider: FAIL"; echo "$bookout"; owfail=1; fi
    # a container: details view (columns, titles), select, double-click, sort (the program's compare), remove, icon view
    cnrout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="click 60 90; dblclick 60 68; key s; key d; key i; close" ../os2x86 pmcnr.exe 2>&1 | tr -d '\r')"
    if grep -q "fields 4, records 5" <<< "$cnrout" && grep -q "CN_EMPHASIS autoexec.bat selected" <<< "$cnrout" &&
       grep -q "CN_ENTER config.sys" <<< "$cnrout" && grep -q "sorted: autoexec.bat config.sys os2.ini readme.txt startup.cmd" <<< "$cnrout" &&
       grep -q "now: autoexec.bat os2.ini readme.txt startup.cmd" <<< "$cnrout" && grep -q "done: 4 records, view icon" <<< "$cnrout"
    then echo "PM container: ok"; else echo "PM container: FAIL"; echo "$cnrout"; owfail=1; fi
    [ "$owfail" = 0 ] || st=1
else
    echo "(no OpenWatcom: its programs not tried)"
fi
exit $st
