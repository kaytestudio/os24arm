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
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmclip.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmvs.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmtree.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmdrag.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmgeo.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmprf.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmfdlg.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmquery.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmsearch.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmxform.c" >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmprint.c" >/dev/null
    wrc -q -r -bt=os2 -i="$S" "$S/pmowner.rc" -fo=pmowner.res >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmowner.c" >/dev/null && wrc -q -bt=os2 pmowner.res pmowner.exe >/dev/null
    wrc -q -r -bt=os2 -i="$S" "$S/pmhelp.rc" -fo=pmhelp.res >/dev/null
    wcl386 -q -za99 -bt=os2 -l=os2v2_pm "$S/pmhelp.c" >/dev/null && wrc -q -bt=os2 pmhelp.res pmhelp.exe >/dev/null
    # (its help: OpenWatcom's OS/2 wipfc, run here, from its snapshot's archive if it's here; two of its books too)
    [ -f wipfc-os2/binp/wipfc.exe ] || { [ -f "$ROOT/build/os2x86-test/dl/ow.tar.xz" ] && mkdir -p wipfc-os2 &&
        tar xJf "$ROOT/build/os2x86-test/dl/ow.tar.xz" -C wipfc-os2 ./binp/wipfc.exe ./wipfc ./binp/help/cmix.inf ./binp/help/cpplib.inf 2>/dev/null; }
    rm -f pmhelp.hlp
    [ -f wipfc-os2/binp/wipfc.exe ] && cp "$S/pmhelp.ipf" . && WIPFC="$PWD/wipfc-os2/wipfc" ../os2x86 wipfc-os2/binp/wipfc.exe -q pmhelp.ipf >/dev/null 2>&1
    owfail=0
    # pm_api.inc's and dos_api.inc's names: as OS/2's headers (OpenWatcom's) have them
    for f in pm_api dos_api; do
        python3 -I "$T/test/check-api.py" "$W/h/os2" "$T/$f.inc" >apicheck.out && echo "OS/2 names ($f.inc): ok" ||
            { echo "OS/2 names ($f.inc): FAIL"; cat apicheck.out; owfail=1; }
    done
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
    # the clipboard: CF_TEXT set and read back; an entry field's Ctrl+V, a selection (Shift+keys) copied, then typed over; an MLE's Ctrl+V; WinSetPointer
    clipout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="click 100 60; ctrl v; key !; shift vkey home; ctrl c; vkey end; shift vkey left; key ?; click 100 150; ctrl v; enter; ctrl v; close" ../os2x86 pmclip.exe 2>&1 | tr -d '\r')"
    if grep -q 'put: "Hello, clipboard" (have 1, info 400, first format 1)' <<< "$clipout" && grep -q "pointer: set 1, same 1" <<< "$clipout" &&
       grep -q 'entry "Hello, clipboard?", mle "Hello, clipboard!||Hello, clipboard!"' <<< "$clipout" && grep -q 'done: "Hello, clipboard!"' <<< "$clipout"
    then echo "PM clipboard: ok"; else echo "PM clipboard: FAIL"; echo "$clipout"; owfail=1; fi
    # a value set (colours, text: click, double-click, a key) and a circular slider (its + button, a drag round the dial)
    vsout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="click 190 30; dblclick 120 65; vkey right; click 170 199; drag 120 125 150 148; close" ../os2x86 pmvs.exe 2>&1 | tr -d '\r')"
    if grep -q 'item 2,2: "Green" (5), 1,3: 0000ff, attr 2,1 4' <<< "$vsout" && grep -q "VN_SELECT 1,3 (selected 1,3)" <<< "$vsout" &&
       grep -q "VN_ENTER 2,2" <<< "$vsout" && grep -q "VN_SELECT 2,3" <<< "$vsout" && grep -q "CSN_CHANGED 55" <<< "$vsout" &&
       grep -q "CSN_TRACKING 80" <<< "$vsout" && grep -q "done: dial 80, selected 2,3" <<< "$vsout"
    then echo "PM value set, circular slider: ok"; else echo "PM value set, circular slider: FAIL"; echo "$vsout"; owfail=1; fi
    # a container's tree view: children, CMA_FIRSTCHILD/NEXT/PARENT, the + button, editing a name (Alt+click; Enter keeps, Esc drops), double-click, Right/Left/-, removing a subtree
    treeout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="click 26 52; altclick 90 73; key Notes; enter; altclick 110 73; key xx; esc; dblclick 80 73; vkey down; vkey right; vkey right; vkey left; vkey left; vkey left; key -; close" ../os2x86 pmtree.exe 2>&1 | tr -d '\r')"
    if grep -q "first child Drives, next Programs, parent of Games Programs, after Desktop Printer" <<< "$treeout" &&
       [ "$(grep '^CN_' <<< "$treeout" | tr '\n' ' ')" = "CN_EXPANDTREE Desktop CN_EXPANDTREE Programs CN_ENTER Notes CN_EXPANDTREE Games CN_COLLAPSETREE Games CN_COLLAPSETREE Programs " ] &&
       grep -q "selected Programs; removing Programs: 5 left" <<< "$treeout" &&
       [ "$(grep '^EDIT' <<< "$treeout" | tr '\n' ' ')" = "EDIT begin Editor EDIT realloc 6 EDIT end Notes EDIT begin Notes EDIT end Notes " ]
    then echo "PM container tree: ok"; else echo "PM container tree: FAIL"; echo "$treeout"; owfail=1; fi
    # owner drawing: a list box (measured, each item drawn, the selection inverted), a value set item, menu items (measured, lit)
    ownout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="click 40 60; click 20 10; move 40 40; shot owner.ppm; click 40 40; close" ../os2x86 pmowner.exe 2>&1 | tr -d '\r')"
    if grep -q "LN_SELECT 0" <<< "$ownout" && grep -q "WM_COMMAND 11" <<< "$ownout" &&
       grep -q "done: measured 3, list drawn yes, value set drawn yes, menu drawn yes, lit yes" <<< "$ownout" &&
       python3 - owner.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3); w, h = map(int, d[1].split()); px = d[3]
at = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
sys.exit(0 if at(60, 34) == (0, 128, 0) and at(20, 122) == (0, 0, 255) else 1)   # the lit menu item, the Blue swatch
PY
    then echo "PM owner drawing: ok"; else echo "PM owner drawing: FAIL"; echo "$ownout"; owfail=1; fi
    # drag and drop (button 2): a record from one container to another (CN_INITDRAG, DrgDrag, CN_DRAGOVER, CN_DROP), then one refused
    dragout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="rdrag 40 30 220 60; rdrag 40 10 60 60; close" ../os2x86 pmdrag.exe 2>&1 | tr -d '\r')"
    if [ "$(tr '\n' '|' <<< "$dragout")" = "CN_INITDRAG beta.txt from 1|CN_DROP beta.txt on 2, move, 1 items|DrgDrag: the right one|CN_INITDRAG alpha.txt from 1|DrgDrag: nowhere|drag-overs yes|left: alpha.txt gamma.txt|right: beta.txt|" ]
    then echo "PM drag and drop: ok"; else echo "PM drag and drop: FAIL"; echo "$dragout"; owfail=1; fi
    # GPI's geometry: arcs, an area, a path with a pattern, XOR, polygons, a dotted line, a spline, a marker, a rounded box, clipping
    geoout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="shot geo.ppm; close" ../os2x86 pmgeo.exe 2>&1 | tr -d '\r')"
    if grep -q "spline ends at 270 200" <<< "$geoout" && python3 - geo.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3); w, h = map(int, d[1].split()); px = d[3]
at = lambda x, y: tuple(px[((h - 1 - y) * w + x) * 3:((h - 1 - y) * w + x) * 3 + 3])   # (PM's way up)
R, W, K = (255, 0, 0), (255, 255, 255), (0, 0, 0)
want = { (60, 60): R, (98, 60): R, (102, 60): W, (160, 40): (0, 0, 255), (250, 50): (0, 255, 0), (251, 50): W,
         (305, 22): (0, 255, 255), (342, 72): W, (392, 79): (0, 128, 0), (20, 150): K, (21, 150): K, (22, 150): W,
         (90, 200): (255, 0, 255), (60, 230): (255, 0, 255), (350, 200): (0, 0, 128), (21, 279): W, (70, 260): (0, 128, 128),
         (170, 260): (255, 255, 0), (140, 260): W, (270, 200): (128, 128, 0) }
bad = [(p, at(*p), c) for p, c in want.items() if at(*p) != c]
if bad: print(*bad, sep='\n')
sys.exit(1 if bad else 0)
PY
    then echo "PM geometry: ok"; else echo "PM geometry: FAIL"; echo "$geoout"; owfail=1; fi
    # profiles: written by one run (the user's OS2.INI, a private .INI), read back by another; the help manager's instance
    rm -rf prfhome PRIVATE.INI; mkdir prfhome
    prfout="$(OS2X86_HOME="$PWD/prfhome" OS2X86_PM=headless ../os2x86 pmprf.exe write 2>&1; OS2X86_HOME="$PWD/prfhome" OS2X86_PM=headless ../os2x86 pmprf.exe read 2>&1)"
    prfout="$(tr -d '\r' <<< "$prfout")"
    if [ "$(tr '\n' '|' <<< "$prfout")" = "private: opened|help: instance, rc 0, associated 1|written|Name Warp, Count 42, Missing -1|Big read 123456|Name's size 5|Gone: deleted|apps: OS2X86 Test|OS2X86 Test: Name Count Big|private Colour blue|" ]
    then echo "PM profiles, help instance: ok"; else echo "PM profiles, help instance: FAIL"; echo "$prfout"; owfail=1; fi
    # the file dialog: an Open on a filter (into a directory, back up, a file double-clicked), a Save As with a name typed; the font dialog (Courier, 14, bold)
    rm -rf fdtest; mkdir -p fdtest/sub fdtest/docs; touch fdtest/a.txt fdtest/B.TXT fdtest/c.dat fdtest/sub/inner.txt
    fdout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="dblclick 40 138; sleep 600; dblclick 40 122; sleep 600; dblclick 260 138; key new.txt; enter; click 60 62; click 290 147; click 365 45; enter" ../os2x86 pmfdlg.exe 2>&1 | tr -d '\r')"
    if grep -q 'open: shown, OK, "C:.*\\fdtest\\B.TXT", 1$' <<< "$fdout" && grep -q 'save: OK, "C:.*\\fdtest\\sub\\new.txt"$' <<< "$fdout" &&
       grep -q 'font: OK, Courier 14 pt, weight 7, face "Courier Bold", selection 20, use c' <<< "$fdout"
    then echo "PM file and font dialogs: ok"; else echo "PM file and font dialogs: FAIL"; echo "$fdout"; owfail=1; fi
    # IPF help: F1 on a control (the help table's panel), on the client (HM_HELPSUBITEM_NOT_FOUND, the extended help),
    # an inform link, a link, the program's HM_ messages; VIEW (--view) on a book, a link into another book
    if [ -f pmhelp.hlp ]; then
        hlpout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="click 50 295; vkey f1; shot help1.ppm; vkey esc; click 300 100; vkey f1; vkey tab; enter; vkey tab; enter; shot help2.ppm; vkey esc; vkey esc; key c; vkey esc; key i; shot help3.ppm; vkey esc; key n; vkey esc; key r; key q" ../os2x86 pmhelp.exe 2>&1 | tr -d '\r')"
        viewout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="click 160 420; click 20 20; shot view1.ppm; vkey esc; shot view2.ppm; close" ../os2x86 --view pmhelp.hlp button 2>&1)"
        [ -f wipfc-os2/binp/help/cmix.inf ] && viewout="$viewout
$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="vkey tab; enter; shot view3.ppm; close; close" ../os2x86 --view wipfc-os2/binp/help/cmix.inf "Header Files" 2>&1)"
        if [ "$(grep -v '^\[pm\]' <<< "$hlpout" | tr '\n' '|')" = "instance made, associated 1|not found: window 1, item 32776|inform 77|contents: 0|index: 0|by name: 0|error 2010|panel 999: 2010|" ] &&
           grep -q '"Help test - The button")' <<< "$hlpout" && grep -q '"Help test - Second panel")' <<< "$hlpout" && grep -q '"Help test - Index")' <<< "$hlpout" &&
           grep -q 'view1.ppm.*"Help test - Using the test")' <<< "$viewout" && grep -q 'view2.ppm.*"Help test - Contents")' <<< "$viewout" &&
           { [ ! -f wipfc-os2/binp/help/cmix.inf ] || grep -q 'view3.ppm.*"Open Watcom 2.0 C++ Class Library Reference - Header Files")' <<< "$viewout"; }
        then echo "PM help (IPF): ok"; else echo "PM help (IPF): FAIL"; echo "$hlpout"; echo "$viewout"; owfail=1; fi
    else echo "PM help (IPF): skipped (no wipfc)"; fi
    # the pointer and points between windows: key state (synchronous), pointer position, mapping, window from point, class, capture
    qout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="shift click 30 40; close" ../os2x86 pmquery.exe 2>&1 | tr -d '\r')"
    if [ "$(tr '\n' '|' <<< "$qout")" = "down 1, shift 1; at 30 79, mapped back 30 79|from point: the button, the client; class #3; child 1|capture ours|thread 1|" ]
    then echo "PM pointer, points, key state: ok"; else echo "PM pointer, points, key state: FAIL"; echo "$qout"; owfail=1; fi
    # a list box's search: the whole item, case or not, prefix, substring, after an item, none
    lssout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf ../os2x86 pmsearch.exe 2>&1 | tr -d '\r')"
    if [ "$lssout" = "0 4 0 1 0 3 4 -1" ]; then echo "PM list box search: ok"; else echo "PM list box search: FAIL"; echo "$lssout"; owfail=1; fi
    # page units (LOENGLISH) and transforms (translate, scale, rotate; GpiConvert)
    xfout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="shot xform.ppm; close" ../os2x86 pmxform.exe 2>&1 | tr -d '\r')"
    if grep -q "^units LOENGLISH$" <<< "$xfout" && grep -q "^device: 96 48, 240 0$" <<< "$xfout" && python3 - xform.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3); w, h = map(int, d[1].split()); px = d[3]
at = lambda x, y: tuple(px[((h - 1 - y) * w + x) * 3:((h - 1 - y) * w + x) * 3 + 3])
R, B, G, W = (255, 0, 0), (0, 0, 255), (0, 255, 0), (255, 255, 255)
want = { (80, 80): R, (95, 95): R, (99, 50): W, (30, 20): G, (5, 20): R, (400, 20): B, (431, 47): B, (380, 20): W, (436, 20): W }
bad = [(p, at(*p), c) for p, c in want.items() if at(*p) != c]
if bad: print(*bad, sep='\n')
sys.exit(1 if bad else 0)
PY
    then echo "PM page units, transforms: ok"; else echo "PM page units, transforms: FAIL"; echo "$xfout"; owfail=1; fi
    # printing: the PDF queue, a printer DC (A4, 150 dpi), two pages in PU_LOENGLISH - the PDF read back, its first page's pixels
    rm -rf printed
    prout="$(OS2X86_PRINT="$PWD/printed" OS2X86_PM=headless ../os2x86 pmprint.exe 2>&1 | tr -d '\r')"
    if grep -q "^queue: PDF$" <<< "$prout" && grep -q "^page: 1240 x 1754 pels, form A4 210 x 297 mm$" <<< "$prout" && grep -q "^job 1$" <<< "$prout" &&
       python3 - printed/test_page.pdf <<'PY'
import re, sys
d = open(sys.argv[1], 'rb').read()
pages = int(re.search(rb'/Type /Pages /Count (\d+)', d).group(1))
m = re.search(rb'/Width (\d+) /Height (\d+) .*?/Length (\d+) >>\nstream\n', d)
w, h, n = int(m.group(1)), int(m.group(2)), int(m.group(3))
s, o, i = d[m.end():m.end() + n], bytearray(), 0
while s[i] != 128:                                   # RunLengthDecode
    c = s[i]
    if c < 128: o += s[i + 1:i + 2 + c]; i += 2 + c
    else: o += bytes([s[i + 1]]) * (257 - c); i += 2
at = lambda x, y: tuple(o[((h - 1 - y) * w + x) * 3:((h - 1 - y) * w + x) * 3 + 3])   # (from the bottom)
ok = pages == 2 and len(o) == w * h * 3 and at(300, 300) == (0, 0, 255) and at(140, 300) == (255, 255, 255) and at(460, 300) == (255, 255, 255)
sys.exit(0 if ok else 1)
PY
    then echo "PM printing (PDF): ok"; else echo "PM printing (PDF): FAIL"; echo "$prout"; owfail=1; fi
    # a 16-bit OS/2 1.x program (NE), OpenWatcom's 16-bit C runtime (its libraries unpacked from the snapshot, when needed)
    if [ ! -d "$W/lib286/os2" ] && [ -f "$OUT/dl/ow.tar.xz" ]; then (cd "$W" && tar xJf "$OUT/dl/ow.tar.xz" ./lib286) 2>/dev/null; fi
    if [ -x "$B/wcl" ] && [ -d "$W/lib286/os2" ]; then
        INCLUDE="$W/h:$W/h/os21x" wcl -q -bt=os2 -l=os2 -fpi "$T/test/ow16/test16.c" >/dev/null
        out16="$(../os2x86 test16.exe one "two words" 2>&1; echo "exit $?")"
        if grep -q "^passed" <<< "$out16" && grep -q "^exit 42" <<< "$out16"; then echo "16-bit (NE): ok"; else echo "16-bit (NE): FAIL"; echo "$out16"; owfail=1; fi
        # a 16-bit DLL of its own (its data, the caller's DS put back), DosLoadModule/GetProcAddr, DosExecPgm of test16
        INCLUDE="$W/h:$W/h/os21x" wcl -q -bt=os2 -bd -ml -zu -zdp -l=os2_dll -fe=dll16.dll "$T/test/ow16/dll16.c" >/dev/null
        wlib -q -n dll16.lib +dll16.dll >/dev/null
        INCLUDE="$W/h:$W/h/os21x" wcl -q -bt=os2 -l=os2 "$T/test/ow16/use16.c" dll16.lib >/dev/null
        dll16out="$(../os2x86 use16.exe 2>&1 | tr -d '\r')"
        if grep -q "^imported: 42, 1001 1002, inited 1$" <<< "$dll16out" && grep -q "^by name: 1003$" <<< "$dll16out" &&
           grep -q "^module: DLL16.DLL$" <<< "$dll16out" && grep -q "^NOSUCH: not found (NOSUCH)$" <<< "$dll16out" && grep -q "^child: 0 42$" <<< "$dll16out"
        then echo "16-bit DLL, DosExecPgm: ok"; else echo "16-bit DLL, DosExecPgm: FAIL"; echo "$dll16out"; owfail=1; fi
        # 16-bit threads: two workers sharing a counter under a RAM semaphore, a system semaphore for their end, DosMuxSemWait
        INCLUDE="$W/h:$W/h/os21x" wcl -q -s -bt=os2 -l=os2 "$T/test/ow16/thread16.c" >/dev/null
        thr16out="$(../os2x86 thread16.exe 2>&1 | tr -d '\r')"
        if [ "$(tr '\n' '|' <<< "$thr16out")" = "threads 2 3 4|counter 2000, finished 2, mux 0|lock free|" ]
        then echo "16-bit threads, semaphores: ok"; else echo "16-bit threads, semaphores: FAIL"; echo "$thr16out"; owfail=1; fi
        # 16-bit Presentation Manager: its window procedure called back, painting, a timer, a key, a click, a message box
        INCLUDE="$W/h:$W/h/os21x" wcl -q -s -bt=os2 -l=os2_pm "$T/test/ow16/pm16.c" >/dev/null
        pm16out="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="sleep 60; sleep 60; sleep 60; sleep 60; sleep 60; sleep 60; shot pm16.ppm; key a; click 50 40; close; enter" ../os2x86 pm16.exe 2>&1 | tr -d '\r')"
        if grep -q "^WM_CREATE$" <<< "$pm16out" && grep -q "^WM_PAINT$" <<< "$pm16out" && grep -q "^WM_TIMER x3$" <<< "$pm16out" &&
           grep -q "^WM_CHAR a$" <<< "$pm16out" && grep -q "^WM_BUTTON1DOWN 50 119$" <<< "$pm16out" && grep -q "^WM_CLOSE, message box 6$" <<< "$pm16out" &&
           grep -q "^done$" <<< "$pm16out" && python3 - pm16.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3); w, h = map(int, d[1].split()); px = d[3]
at = lambda x, y: tuple(px[((h - 1 - y) * w + x) * 3:((h - 1 - y) * w + x) * 3 + 3])
sys.exit(0 if at(60, 40) == (255, 0, 0) and at(200, 40) == (255, 255, 255) else 1)
PY
        then echo "16-bit Presentation Manager: ok"; else echo "16-bit Presentation Manager: FAIL"; echo "$pm16out"; owfail=1; fi
        # its resources, in the NE: a menu, a string table, a dialog (its 16-bit procedure, a list box filled with 16:16 pointers)
        wrc -q -r -bt=os2 -i="$T/test/ow16" "$T/test/ow16/pm16r.rc" -fo=pm16r.res >/dev/null
        INCLUDE="$W/h:$W/h/os21x" wcl -q -s -bt=os2 -l=os2_pm "$T/test/ow16/pm16r.c" >/dev/null && wrc -q -bt=os2 pm16r.res pm16r.exe >/dev/null
        pm16rout="$(OS2X86_PM=headless OS2X86_FONT=fonts/DejaVuSans.ttf OS2X86_PM_SCRIPT="click 20 10; click 40 32; sleep 200; enter; click 20 10; click 40 54" ../os2x86 pm16r.exe 2>&1 | tr -d '\r')"
        if [ "$(tr '\n' '|' <<< "$pm16rout")" = 'string: "Hello from a 16-bit string table"|DosGetResource: a selector|menu: there|WM_INITDLG "create params"|list: 2 items, the second "second"|WinDlgBox 1, typed "Warp"|done|' ]
        then echo "16-bit PM resources: ok"; else echo "16-bit PM resources: FAIL"; echo "$pm16rout"; owfail=1; fi
    fi
    [ "$owfail" = 0 ] || st=1
else
    echo "(no OpenWatcom: its programs not tried)"
fi
exit $st
