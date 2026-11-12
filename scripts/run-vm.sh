#!/bin/bash
# scripts/run-vm.sh — build the kernel and boot it in QEMU with EL2, so the
# built-in hypervisor (kernel/src/hyp.c, VM command) is usable.
#
# The kernel only gets EL2 when QEMU emulates it (-machine
# virt,virtualization=on), which means TCG: HVF can't hand a guest EL2 on
# Apple M1/M2. cortex-a72 keeps EL2 plain ARMv8.0 (no VHE). Guest RAM is
# carved from host RAM at 0x80000000, so -m must stay at 2G or more.
#
# Usage: scripts/run-vm.sh [options]
#   --no-build   don't (re)build, just run what's in build/
#   --no-disk    skip the BOOT/C: test disk (shell works, DIR etc. don't)
#   --test       non-interactive: run VM TEST, type into the guest, check
#                the output, exit 0/1 (log kept in build/run-vm-test.log)
#   --gdb        wait for a debugger on :1234
#   --usb        add an xHCI controller + USB keyboard (drivers/usb/xhci.c)
#   --wayland    a display window (virtio-gpu), USB keyboard + tablet, and a
#                USB stick (HPFS, build/userland/stick.img, (re)made from
#                build/userland/bin and tests when they change) with the
#                compositors: at the prompt, MOUNT usb0p1 \h then
#                RUNELF \h\cage /h/wlclient  (Esc ends wlclient and cage)
#                or RUNELF \h\tinywl -s /h/wlclient  (Alt+Esc quits it)
#   -h, --help
#
# Interactive: type VM TEST at the [C:\]> prompt. Inside the guest,
# Ctrl-D powers it off and Ctrl-] stops it from the host side. Ctrl-A X
# quits QEMU.

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
BUILD_IT=1
USE_DISK=1
TEST_MODE=0
GDB_OPT=()
USB_OPT=()
WAYLAND=0

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) BUILD_IT=0 ;;
        --no-disk)  USE_DISK=0 ;;
        --test)     TEST_MODE=1 ;;
        --gdb)      GDB_OPT=(-s -S) ;;
        --usb)      USB_OPT=(-device qemu-xhci -device usb-kbd) ;;
        --wayland)  WAYLAND=1 ;;
        -h|--help)  sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
    shift
done

command -v qemu-system-aarch64 >/dev/null 2>&1 || {
    echo "qemu-system-aarch64 not found (macOS: brew install qemu)" >&2; exit 1; }

# ── Build ───────────────────────────────────────────────────────────────────
if [ "$BUILD_IT" = 1 ]; then
    if [ ! -f "$BUILD/CMakeCache.txt" ]; then
        # A fresh configure picks AppleClang on macOS, which can't build a
        # freestanding aarch64 kernel - point it at LLVM clang explicitly.
        CC_ARGS=()
        if command -v brew >/dev/null 2>&1 && [ -x "$(brew --prefix llvm 2>/dev/null)/bin/clang" ]; then
            LLVM="$(brew --prefix llvm)/bin/clang"
            CC_ARGS=(-DCMAKE_C_COMPILER="$LLVM" -DCMAKE_ASM_COMPILER="$LLVM")
        fi
        cmake -S "$ROOT" -B "$BUILD" "${CC_ARGS[@]}"
    fi
    cmake --build "$BUILD" --target os2warp.img -j8
    if [ "$USE_DISK" = 1 ] && ! cmake --build "$BUILD" --target mktestdisk; then
        echo "warning: test disk build failed - booting without it" >&2
        USE_DISK=0
    fi
fi

KERNEL="$BUILD/os2warp.img"
[ -f "$KERNEL" ] || { echo "$KERNEL missing - run without --no-build" >&2; exit 1; }
DISK_OPT=()
if [ "$USE_DISK" = 1 ] && [ -f "$BUILD/testdisk.img" ]; then
    DISK_OPT=(-device "loader,file=$BUILD/testdisk.img,addr=0x44000000,force-raw=on")
fi

DISPLAY_OPT=(-display none -nographic)
if [ "$WAYLAND" = 1 ]; then
    STICK="$BUILD/userland/stick.img"
    if [ ! -f "$STICK" ] || [ -n "$(find "$BUILD/userland/bin" "$BUILD/userland/tests" -maxdepth 1 \
                                    -type f -newer "$STICK" 2>/dev/null | head -1)" ]; then
        [ -x "$BUILD/userland/bin/tinywl" ] || {
            echo "build the compositor first: userland/build-libs.sh && userland/build-compositor.sh" >&2; exit 1; }
        STAGE="$(mktemp -d)"
        cp "$BUILD"/userland/bin/* "$STAGE"/
        find "$BUILD/userland/tests" -maxdepth 1 -type f -perm -u+x -exec cp {} "$STAGE"/ \;
        python3 "$ROOT/scripts/mkhpfs.py" "$STICK" --size 32M --from "$STAGE" --mbr >/dev/null
        rm -rf "$STAGE"
        echo "made $STICK"
    fi
    USB_OPT=(-device qemu-xhci -device usb-kbd -device usb-tablet
             -device usb-storage,drive=stick -drive "if=none,id=stick,file=$STICK,format=raw")
    DISPLAY_OPT=(-global virtio-mmio.force-legacy=false -device virtio-gpu-device -display default)
fi

QEMU=(qemu-system-aarch64
      -machine virt,gic-version=3,virtualization=on
      -cpu cortex-a72
      -accel tcg
      -m 2G
      -smp 1
      -kernel "$KERNEL"
      "${DISK_OPT[@]}"
      -serial mon:stdio
      "${DISPLAY_OPT[@]}"
      "${USB_OPT[@]}"
      "${GDB_OPT[@]}")

if [ "$TEST_MODE" = 0 ]; then
    echo "Booting with EL2 (TCG). At the prompt: VM TEST.  Ctrl-A X quits QEMU."
    exec "${QEMU[@]}"
fi

# ── --test: drive the console through a FIFO and check what comes back ──────
LOG="$BUILD/run-vm-test.log"
TMP="$(mktemp -d)"
QEMU_PID=
cleanup() {
    exec 3>&- 2>/dev/null || true
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT

mkfifo "$TMP/in"
: > "$LOG"
"${QEMU[@]}" < "$TMP/in" > "$LOG" 2>&1 &
QEMU_PID=$!
exec 3> "$TMP/in"

# wait_for <fixed string> <seconds>
wait_for() {
    local i=0 limit=$(( $2 * 5 ))
    while ! grep -qF -- "$1" "$LOG"; do
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "FAIL: QEMU exited while waiting for: $1" >&2; return 1
        fi
        i=$(( i + 1 ))
        if [ "$i" -ge "$limit" ]; then
            echo "FAIL: timed out after $2s waiting for: $1" >&2; return 1
        fi
        sleep 0.2
    done
}

fail() { echo; echo "---- last 40 lines of $LOG ----"; tail -40 "$LOG"; exit 1; }

echo "Booting (TCG)..."
wait_for "Type HELP for available commands" 120 || fail
sleep 1
printf 'VM TEST\r' >&3
wait_for "type something" 60 || fail
printf 'hello vm\r' >&3
wait_for "hello vm" 20 || fail
printf '\004' >&3
wait_for "[VM] exits:" 20 || fail

STATUS=0
check() {
    if grep -qF -- "$1" "$LOG"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1"; STATUS=1
    fi
}
echo "Checking guest output:"
check "[HYP]    EL2 hypervisor : ready"
check "Hello from the guest!"
check "Guest is running at EL1"
check "PSCI_VERSION over hvc: 1.0"
check "tick 3"          # reached only through host preemption
check "tick 6"          # WFI wakeups by the timer
check "hello vm"        # UART RX interrupt -> echo
check "guest powered off via PSCI"
echo
sed -n '/\[VM\] exits:/p' "$LOG"
[ "$STATUS" = 0 ] && echo "PASS" || { echo "FAIL (log: $LOG)"; fail; }
