#!/bin/bash
# scripts/desktop.sh - build the whole system and boot it into the desktop.
#
# Builds, in order - each step skipped when what it makes is already there
# (--rebuild: all of them again):
#   1. the kernel (os2warp.img) and the C: test disk (CMake, LLVM clang)
#   2. the musl sysroot                         userland/build-musl.sh
#   3. the libraries (wayland, wlroots, ...)    userland/build-libs.sh
#   4. busybox                                  userland/build-busybox.sh
#   5. foot, fuzzel, cage, dwl, ...             userland/build-compositor.sh
#   6. the packages the Workplace Shell needs   userland/build-rpms.sh musl-devel llvm
#      (LLVM's libc++: the long one, an hour or more the first time;
#       --all-packages: every package in userland/rpm, for \OS2\RPMS)
#   7. the Workplace Shell                      userland/build-wps.sh
#   8. the test programs, os2cmd, os2x86        userland/build-tests.sh (always)
#   9. the USB stick image                      userland/make-stick.sh (always)
# then boots QEMU with a display window, the stick, a USB keyboard and
# tablet and the network (scripts/run-vm.sh --wayland). The kernel starts
# the desktop by itself (CONFIG.SYS: SET RUNWORKPLACE=C:\OS2\PMSHELL.EXE);
# a key on the serial console in the first seconds keeps the console.
#
# Usage: scripts/desktop.sh [options]
#   --hvf            run natively (Apple's Hypervisor.framework): much faster
#   --rebuild        every step again, even those already built
#   --all-packages   build every UNIXROOT package, not just LLVM's
#   --os2-demos      x86 OS/2 programs on the stick, in \OS2DEMOS: the
#                    OpenWatcom test programs (PM ones too; userland/os2x86/
#                    test/run-tests.sh builds them, needs OpenWatcom: WATCOM
#                    or its snapshot unpacked in build/os2x86-test/ow)
#   --build-only     build, don't boot
#   --res WxH        the display's size to start with (1920x1080)
#   -h, --help
#
# Needs (macOS): brew install llvm qemu lld rpm (and what each step's own
# script lists at its top). The desktop: Alt+O a Command Prompt, Alt+P the
# programs, Ctrl+Esc the Window List, Alt+Shift+Q back to the console;
# Ctrl+A X (in this terminal) quits QEMU.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build"
W="$B/userland"
U="$ROOT/userland"

HVF=0 REBUILD=0 ALLPKG=0 DEMOS=0 RUN=1
while [ $# -gt 0 ]; do
    case "$1" in
        --hvf) HVF=1 ;;
        --rebuild) REBUILD=1 ;;
        --all-packages) ALLPKG=1 ;;
        --os2-demos) DEMOS=1 ;;
        --build-only) RUN=0 ;;
        --res) shift; export RES="$1" ;;
        -h|--help) sed -n '2,37p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
    shift
done

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
# step NAME DONE-TEST COMMAND...: the command, unless the test says it's done
step() {
    local name="$1" done="$2"
    shift 2
    if [ "$REBUILD" = 0 ] && eval "$done"; then
        echo "-- $name: already built"
        return
    fi
    say "$name"
    local t0=$SECONDS
    "$@"
    echo "-- $name: done in $(( (SECONDS - t0) / 60 )) min $(( (SECONDS - t0) % 60 )) s"
}

# ── what it needs ────────────────────────────────────────────────────────
missing=()
LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || true)"
[ -x "$LLVM_PREFIX/bin/clang" ] || missing+=("llvm (brew install llvm)")
command -v ld.lld >/dev/null 2>&1 || [ -x "$LLVM_PREFIX/bin/ld.lld" ] || missing+=("lld (brew install lld)")
command -v qemu-system-aarch64 >/dev/null 2>&1 || missing+=("qemu (brew install qemu)")
command -v cmake >/dev/null 2>&1 || missing+=("cmake (brew install cmake)")
command -v python3 >/dev/null 2>&1 || missing+=("python3")
if [ ${#missing[@]} -gt 0 ]; then
    echo "missing: ${missing[*]}" >&2
    exit 1
fi

# ── 1. the kernel ────────────────────────────────────────────────────────
say "kernel"
if [ ! -f "$B/CMakeCache.txt" ]; then
    # a fresh configure would pick AppleClang: the LLVM one, explicitly
    cmake -S "$ROOT" -B "$B" -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" -DCMAKE_ASM_COMPILER="$LLVM_PREFIX/bin/clang"
fi
cmake --build "$B" --target os2warp.img -j8
cmake --build "$B" --target mktestdisk || echo "(no C: test disk: the desktop doesn't need it)"

# ── 2-7. the userland the desktop is made of ─────────────────────────────
step "musl sysroot" '[ -f "$W/sysroot/lib/libc.a" ]' "$U/build-musl.sh"
step "libraries (wayland, wlroots, ...)" '[ -f "$W/sysroot/lib/pkgconfig/wlroots-0.19.pc" ] && [ -f "$W/sysroot/lib/pkgconfig/fcft.pc" ]' "$U/build-libs.sh"
step "busybox" '[ -x "$W/bin/busybox" ]' "$U/build-busybox.sh"
step "compositor programs (foot, fuzzel, cage, dwl)" '[ -x "$W/bin/foot" ] && [ -x "$W/bin/fuzzel" ] && [ -x "$W/bin/cage" ]' "$U/build-compositor.sh"
if [ "$ALLPKG" = 1 ]; then
    step "UNIXROOT packages (all)" 'false' "$U/build-rpms.sh"
else
    step "packages for the Workplace Shell (musl-devel, llvm: libc++)" \
        'ls "$W"/rpms/libcxx-devel-[0-9]*.rpm >/dev/null 2>&1 && ls "$W"/rpms/llvm-[0-9]*.rpm >/dev/null 2>&1' \
        "$U/build-rpms.sh" musl-devel llvm
fi
step "Workplace Shell" '[ -x "$W/bin/wps" ] && [ "$W/bin/wps" -nt "$U/wps/wps.cpp" ]' "$U/build-wps.sh"

# ── 8. the programs (quick: always) ──────────────────────────────────────
say "test programs, os2cmd, os2x86"
"$U/build-tests.sh"

# ── the x86 OS/2 demos, if asked ─────────────────────────────────────────
EXTRA=""
if [ "$DEMOS" = 1 ]; then
    say "x86 OS/2 programs (OpenWatcom's) for \\OS2DEMOS"
    if "$U/os2x86/test/run-tests.sh" </dev/null >"$B/os2x86-test.log" 2>&1; then echo "(their tests passed)"; else echo "(their tests: see $B/os2x86-test.log)"; fi
    O="$B/os2x86-test/ow"
    EXTRA="$W/os2demos/OS2DEMOS"
    rm -rf "$W/os2demos"; mkdir -p "$EXTRA"
    n=0
    for f in "$O"/*.exe "$O"/*.dll "$B"/os2x86-test/OS2TEST.EXE; do
        [ -f "$f" ] || continue
        b="$(basename "$f" | tr a-z A-Z)"
        cp "$f" "$EXTRA/$b"; chmod +x "$EXTRA/$b"; n=$((n + 1))
    done
    if [ "$n" = 0 ]; then
        echo "no OpenWatcom programs: is OpenWatcom there (WATCOM, or build/os2x86-test/ow)?" >&2
        EXTRA=""
    else
        echo "$n programs: double-click them in Drive D, \\OS2DEMOS"
    fi
fi

# ── 9. the stick ─────────────────────────────────────────────────────────
say "USB stick image"
STICK_EXTRA="$EXTRA" "$U/make-stick.sh" "$W/stick.img"

[ "$RUN" = 1 ] || { say "built - boot it with: scripts/run-vm.sh --wayland --no-build"; exit 0; }

# ── boot: the display, the stick, the desktop ────────────────────────────
say "booting (the desktop starts by itself; Ctrl+A X quits QEMU)"
opts=(--wayland --no-build)
[ "$HVF" = 1 ] && opts+=(--hvf)
touch "$W/stick.img"                               # (newer than the programs: run-vm.sh keeps it)
exec "$ROOT/scripts/run-vm.sh" "${opts[@]}"
