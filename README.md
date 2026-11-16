# OS/2 Warp 4.52 for ARM64

An OS/2-style operating system for 64-bit ARM, written from scratch in C11
and AArch64 assembly. It boots in QEMU's `virt` machine (and on a
Raspberry Pi 3) to an OS/2 command prompt, and from there into a desktop
in the style of OS/2 Warp 4: the Workplace Shell, with folders, the
WarpCenter, Command Prompt windows, a text editor, a web browser, and
programs of three kinds:

- **OS/2 programs built for ARM64** (LX executables: `CMD.EXE`, ...), on
  an OS/2 API of its own (`DOSCALLS.DLL`);
- **Linux programs**: AArch64 Linux ELF binaries (static, or dynamically
  linked against musl) run unmodified on
  a Linux system-call layer, so musl, busybox, wlroots, foot, GNU
  binutils and clang/LLVM all run here;
- **real x86 OS/2 programs**: 32-bit OS/2 2.x-4.x ones and 16-bit OS/2
  1.x ones, text mode and Presentation Manager, through a built-in
  emulator (`os2x86`).

Smaller ports boot to a serial prompt on the BeagleBone Black (ARMv7),
the BeagleV-Ahead (RISC-V) and QEMU's RISC-V `virt` machine.

```
                 ┌──────────────────────── the desktop ──────────────────────────┐
                 │ Workplace Shell (wlroots compositor, C++)   foot   fuzzel  ...  │
                 │ OS/2 Command Prompts (os2cmd)    x86 OS/2 PM and text programs │
                 │                                   (os2x86: i386 + OS/2 API + PM)│
EL0  ────────────┼── Linux ABI (svc #0) ──────────────── LX programs (lx_services) ─┤
                 │ musl, busybox, wayland, wlroots, binutils, clang      CMD.EXE   │
                 └────────────────────────────────────────────────────────────────┘
EL1  kernel: processes, threads, demand paging, signals, epoll, Unix sockets, ptys,
     /proc, fbdev, evdev  ·  OS/2 shell and API (DosOpen ...), LX loader
     VFS: HPFS, FAT16/32, ext4, btrfs, ramfs  ·  lwIP (IPv4 + IPv6)
     drivers: PL011, GICv3 timer, virtio-gpu/ramfb, virtio-net, virtio-input,
              xHCI (USB keyboard, mouse/tablet, mass storage)
EL2  a small hypervisor (VM command) when QEMU gives the kernel EL2
```

---

## Contents

1. [Quick start](#1-quick-start)
2. [Prerequisites](#2-prerequisites)
3. [Building](#3-building)
4. [Running in QEMU](#4-running-in-qemu)
5. [The console shell](#5-the-console-shell)
6. [The desktop](#6-the-desktop)
7. [Programs](#7-programs)
8. [File systems and storage](#8-file-systems-and-storage)
9. [Networking](#9-networking)
10. [The hypervisor](#10-the-hypervisor)
11. [Other boards](#11-other-boards)
12. [BASIC, DEBUG and AI](#12-basic-debug-and-ai)
13. [Tests](#13-tests)
14. [Repository layout](#14-repository-layout)
15. [Status and what's missing](#15-status-and-whats-missing)
16. [License](#16-license)

---

## 1. Quick start

```bash
brew install llvm lld qemu cmake rpm meson ninja pkg-config bison gperf make
scripts/desktop.sh --hvf
```

`scripts/desktop.sh` builds everything and boots QEMU into the desktop:

| step | script | |
|---|---|---|
| kernel, C: test disk | CMake | `build/os2warp.img` |
| musl sysroot | `userland/build-musl.sh` | |
| libraries (wayland, wlroots, pixman, xkbcommon, fcft, ...) | `userland/build-libs.sh` | |
| musl's `libc.so` / dynamic linker, the dynamic-linking test | `userland/build-dynamic.sh` | `build/userland/dyn` |
| busybox | `userland/build-busybox.sh` | |
| foot, fuzzel, cage, dwl | `userland/build-compositor.sh` | |
| packages: musl-devel, LLVM (libc++ for the shell) | `userland/build-rpms.sh` | an hour or more, the first time |
| the Workplace Shell | `userland/build-wps.sh` | |
| test programs, os2cmd, os2x86 | `userland/build-tests.sh` | always |
| the USB stick (HPFS) | `userland/make-stick.sh` | always |

Finished steps are skipped on the next run. The options:

| option | |
|---|---|
| `--hvf` | run natively with Apple's Hypervisor.framework: much faster (without it QEMU emulates the CPU) |
| `--rebuild` | every step again |
| `--all-packages` | build every package in `userland/rpm`, for `\OS2\RPMS` on the stick |
| `--os2-demos` | put the x86 OS/2 test programs (built with OpenWatcom) on the stick, in `\OS2DEMOS` |
| `--build-only` | build, don't boot |
| `--res WxH` | the display's starting size (1920x1080; the window can be resized, up to 2560x1600) |

The kernel starts the desktop by itself (`CONFIG.SYS`: `SET
RUNWORKPLACE=C:\OS2\PMSHELL.EXE`); pressing a key on the serial console
in the first seconds keeps the console instead. On the desktop: **Alt+O** a
Command Prompt, **Alt+P** the programs, **Ctrl+Esc** the Window List,
**Alt+Shift+Q** back to the console. **Ctrl+A X** in the terminal quits
QEMU; Shut Down on the desktop unmounts everything cleanly first.

---

## 2. Prerequisites

The build machine is a Mac (Apple silicon or Intel) with Homebrew; Linux
works for the kernel alone.

```bash
# macOS
brew install llvm lld qemu cmake rpm meson ninja pkg-config bison gperf make
export PATH="$(brew --prefix llvm)/bin:$PATH"
clang --version        # must be LLVM's clang, not Apple's

# Ubuntu / Debian (the kernel)
sudo apt install clang lld llvm qemu-system-arm cmake
```

A fresh `cmake -B build` on macOS picks Apple's clang, which can't build
the kernel: pass LLVM's explicitly (`desktop.sh` does this for you):

```bash
cmake -B build -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
               -DCMAKE_ASM_COMPILER="$(brew --prefix llvm)/bin/clang"
```

---

## 3. Building

### The kernel

```bash
cmake --build build --target os2warp.img   # build/os2warp.img: what QEMU boots
cmake --build build                        # everything: also CMD.EXE, DOSCALLS.DLL,
                                           # the host LX tools (lxbuild, makeexe, makedll), kcc
```

Outputs: `build/os2warp_kernel` (ELF with symbols), `build/os2warp.img`
(the raw image), `build/CMD.EXE` and the other LX programs and DLLs.

The cross-compiler flags live in the root `CMakeLists.txt` (no toolchain
file): `-target aarch64-none-elf -mcpu=cortex-a72 -ffreestanding
-nostdlib -fno-builtin -fno-pie`. `TARGET_PLATFORM` picks the board:

| `TARGET_PLATFORM` | builds | image |
|---|---|---|
| `QEMU` (default) | the full system | `os2warp.img` |
| `RPI3` | the full kernel, Pi entry code | `kernel8.img` |
| `BBB` | a small serial-console kernel (ARMv7) | `os2warp-bbb.img` + `uEnv.txt` |
| `BEAGLEV_AHEAD` | a small serial-console kernel (RV64) | `os2warp-beaglev-ahead.img` + `uEnv.txt` |
| `QEMU_RISCV` | the BeagleV kernel for QEMU's riscv64 `virt` | `os2warp-qemu-riscv.img` |

### The userland

Everything above the kernel is cross-compiled with the same clang against
a musl sysroot that `userland/build-musl.sh` builds in
`build/userland/sysroot`. `userland/build-libs.sh` adds the libraries and
writes the cross toolchain the rest uses
(`build/userland/toolchain/aarch64-linux-musl-cc`, `cross.meson`):
static `ET_EXEC` binaries linked at 0x60000000, where the kernel loads
them. Each `userland/build-*.sh` script lists what it needs at its top;
`userland/README.md` describes them all.

---

## 4. Running in QEMU

`scripts/run-vm.sh` boots what's in `build/`:

| option | |
|---|---|
| `--wayland` | a display window (virtio-gpu), USB keyboard and tablet, and the USB stick with the desktop |
| `--hvf` | native speed with Hypervisor.framework (no EL2 for the kernel's own hypervisor) |
| `--usb` | an xHCI controller and a USB keyboard, without the display |
| `--no-disk` | without the C: test disk |
| `--no-build` | don't rebuild first |
| `--gdb` | wait for a debugger on `:1234` |
| `--smp N` | N CPUs (default 4, up to 8; or `SMP=N`) |
| `--test` | run `VM TEST` non-interactively and check its output |

Every run has a network card (QEMU's user networking: the guest is
10.0.2.15, the host 10.0.2.2); `NET_FWD=",hostfwd=tcp::8080-:80"`
forwards host ports in. `RES=1600x900` sets the display size.

CMake has targets too: `run` (serial console only), `run-gui` (a
display, for the in-kernel `GUI`), `run-testdisk` (with the C: test disk),
`mktestdisk`, `debug` (paused for GDB).

```bash
# debugging: QEMU waits on :1234
scripts/run-vm.sh --gdb
lldb build/os2warp_kernel -o "gdb-remote 1234"
```

---

## 5. The console shell

The kernel's shell (`kernel/src/main.c`, `dos/dos_commands.c`) runs on the
serial console and, through `/dev/os2cmd`, in every OS/2 Command Prompt
window on the desktop:

```
[C:\]> dir
[C:\]> mount usb0p1 \h
usb0p1 is drive D: (HPFS)
[C:\]> d:
[D:\]> ping example.com
```

| command | |
|---|---|
| `DIR`, `TREE`, `TYPE`, `MORE`, `FIND` | list and show files (`/A` shows hidden and system ones) |
| `COPY`, `MOVE`, `DEL`/`ERASE`, `REN`, `MD`, `RD`, `CD`, `ATTRIB` | files and directories (wildcards) |
| `COMP`, `FC` | compare files, by byte or by line |
| `VOL`, `CHKDSK` | the volume, its size and free space; for HPFS, a full check |
| `D:` | change drive: C: is the system volume, D:, E:, ... the volumes mounted after it |
| `MOUNT`, `UMOUNT` | `MOUNT` lists the block devices; `MOUNT usb0p1 \h` mounts an HPFS or FAT volume as the next drive |
| `USB` | the USB devices |
| `DESKTOP [layout [variant]]` | start the desktop (`DESKTOP de nodeadkeys`) |
| `SHUTDOWN` | end the programs, unmount every drive cleanly, power off |
| `RUN prog.EXE` | an OS/2 LX program (`RUN CMD.EXE`) |
| `RUNELF \path\prog args` | a Linux program (`RUNELF \h\busybox sh`) |
| `PING`, `IFCONFIG`, `HOST` | the network |
| `VM STATUS`, `VM TEST` | the hypervisor |
| `MEM`, `IFS`, `VER`, `DATE`, `TIME`, `ECHO`, `CLS` | |
| `DOSKEY` | history and macros (arrows, F7/F8/F9, `name=text`, `$1`-`$9`, `$*`, `$T`) |
| `BASIC`, `DEBUG`, `AI` | see §12 |
| `GUI` | the older in-kernel compositor (four switchable workplaces), a fallback |
| `HELP` | the list |

REXX: `.CMD` files that begin with a `/*` comment are REXX programs, as on
OS/2 - type their name (`HELLO` finds `HELLO.CMD`) and the kernel runs
them with `userland/apps/rexx.c`, a classic REXX interpreter (all the
instructions - DO, SELECT, PARSE templates, PROCEDURE EXPOSE, SIGNAL,
INTERPRET -, stems, REXX arithmetic, about 60 built-in functions with
LINEIN/LINEOUT file I/O) and the common RexxUtil functions (RxFuncAdd and
SysLoadFuncs, SysFileTree, SysFileSearch, SysMkDir/SysRmDir/SysFileDelete,
SysTempFileName, SysSearchPath, SysDriveMap, SysSleep, SysGetKey, SysCls,
SysIni - OS/2's binary .INI profiles, the same USER/SYSTEM ones os2x86's PM
programs use - ...), with OS/2 paths (`D:\NOTES\*.TXT`), and condition traps (SIGNAL ON
SYNTAX/NOVALUE/ERROR/HALT, CALL ON ERROR, CONDITION()); a routine that
isn't internal or built in is another .CMD file; PUSH/QUEUE/PULL and
`'command | RXQUEUE'` for the data queue; other clauses are commands for the
OS/2 shell, RC their result. `rexx -e 'say 2**10'` runs a line.

In a Command Prompt window, a name the shell doesn't know runs as a
program in the current directory (`ls -l`, `vi notes.txt`, busybox
applets, anything on `$PATH`); `START prog` opens it in a new window and
`START /PM prog` starts a graphical one. Commands that take over the
console (`RUN`, `RUNELF`, `DEBUG`, `BASIC`, `UMOUNT`, ...) stay on the
serial console.

---

## 6. The desktop

The Workplace Shell (`userland/wps`) is a Wayland compositor in C++17 on
wlroots, built with the system's own libc++. It looks and works like OS/2
Warp 4:

- **The desktop**: OS/2 System, the Assistance Center, Connections,
  Drives, Command Prompt, Drive D, Programs, Window List, Shut Down and
  the Shredder; objects can be moved and the layout is kept
  (`\OS2\WPS.INI` on the stick).
- **The WarpCenter** along the top: the system menu, the Window List,
  Command Prompt, Drives, Programs, the active window and the clock.
- **OS/2 frames** on every window: the title bar, the system menu, hide,
  maximize and close buttons, a 3D border to resize by.
- **Folders** of the real directories, in Icon, Tree and Details views,
  sorted by name, type, size or date. Drag and drop moves on the same drive
  and copies across drives, as OS/2 does (Shift forces a move, Ctrl a copy).
  There are also shadows, a Startup folder, rename, delete through the
  Shredder, multiple selection and keyboard navigation.
- **Settings notebooks**: the background colour and pattern, title bar
  colours, and the screen's scale (2x for Retina displays).
- **The System Editor**: a text editor that shares the clipboard with the
  other programs.
- **WebExplorer**: a web browser built into the shell. It shows text,
  links, forms and pictures over HTTP and HTTPS, with some CSS and cookies.
- **Connections**: TCP/IP status, Ping and Telnet.
- **The Assistance Center**: the help, from `userland/rootfs/help`.

Terminals are [foot](https://codeberg.org/dnkl/foot) running `os2cmd`, so
every terminal is an OS/2 Command Prompt; fuzzel is the program launcher.
Keyboard layouts come from xkeyboard-config, with dead keys and Compose.

`userland/README.md` (stage 6) has the details.

---

## 7. Programs

### Linux programs

The kernel runs AArch64 Linux ELF binaries at EL0 and implements the
Linux system calls itself (`kernel/src/linux/linux.c`). It provides:

- **Static and dynamic executables**: `ET_EXEC` at their address, and
  position-independent ones (`ET_DYN`, at 0x60000000); a program naming
  an interpreter (`PT_INTERP`) gets it loaded beside it (musl's `libc.so`,
  `ld-musl-aarch64.so.1`, built by `userland/build-dynamic.sh`, in the
  stick's `\lib` - UNIXROOT's `/lib`), which then loads its shared
  libraries; `dlopen`/`dlsym` and TLS in shared libraries work
  (`dyntest`).
- **Processes**, each with its own address space, preemptively scheduled
  (10 ms, GICv3 timer), with demand paging. **SMP**: every CPU QEMU gives
  the machine (PSCI CPU_ON, up to 8) runs programs' threads at once - 4
  spinning threads take about as long as one, under TCG and HVF alike -
  while the kernel itself runs on one CPU at a time (a big kernel lock,
  `kernel/src/smp.c`); `/proc/cpuinfo`, `/proc/stat` and
  `sched_getaffinity` show them all. `fork`, `vfork`/`posix_spawn`,
  `execve`, `wait4` and `#!` scripts all work.
- **Threads**: `clone` with `CLONE_THREAD`, futexes and TLS.
- **Signals**: Linux-layout signal frames, `SA_RESTART`, process groups and
  sessions.
- **Events and IPC**: `poll`/`ppoll`/`pselect6`, `epoll`, `eventfd`,
  `timerfd`, `signalfd`, pipes, `AF_UNIX` sockets with `SCM_RIGHTS` -
  streams, datagrams and `SOCK_SEQPACKET` (`sendto`/`recvfrom` by name, `connect`,
  socketpairs; busybox's `syslogd -L` and `logger` over `/dev/log`).
- **Shared memory**: `memfd_create` and `MAP_SHARED`, `/dev/shm`.
- **Terminals**: the console and pseudo-terminals, with termios.
- **`/proc`**: enough for busybox's `ps`, `top`, `free` and `df`.
- **Devices**: `/dev/fb0` (fbdev) and `/dev/input/event*` (evdev), which
  the desktop's wlroots backend (`userland/wlr-os2`) uses.
- **Networking**: AF_INET and AF_INET6 sockets.

What runs on it: busybox 1.37 (ash and its applets), libwayland, wlroots
0.19, tinywl, cage, dwl, foot, fuzzel, and as RPM packages
(`userland/rpm/*.spec`, installed with `rpm -i` from `\OS2\RPMS` or a
repository on the host) GNU binutils, GNU make, tcc and **clang/lld 23**.
So `clang hello.c -o hello` and `clang++ -O2 x.cpp -o x` work on the
system itself.

### OS/2 programs for ARM64

OS/2 LX executables containing AArch64 code are built on the host by the
`lx/` tools (`lxbuild`, `makeexe`, `makedll`). They run as processes, at
EL0 in their own address space, and call the system through the
`lx_services` table and `DOSCALLS.DLL`. `CMD.EXE` (`dos/cmd_main.c`) is
the same command set as the console shell, built as a standalone OS/2
program. `KAYTEVM.EXE` is another such program. Type `CMD` in a prompt,
`RUN CMD.EXE` on the console, or double-click one in a folder.

### x86 OS/2 programs

`userland/os2x86` runs real OS/2 executables built for x86 on this ARM
system: 32-bit OS/2 2.x-4.x programs (LX) and 16-bit OS/2 1.x programs
(NE: text and Presentation Manager, their DLLs, threads and semaphores). The kernel hands any x86 LX image to it as if it
were a `#!/h/OS2/os2x86` script. It contains:

- **an i386 interpreter** with the x87, 16-bit code and OS/2's tiled LDT
  (for the 16-bit KBD/VIO/MOU calls the C runtimes use);
- **an LX loader**, with fixups, EXEPACK, and the program's own DLLs;
- **about 100 DOSCALLS/NLS/MSG/QUECALLS/SESMGR calls**, done with this
  system's own: files, memory, threads, semaphores, exceptions,
  `DosExecPgm`, sessions and queues;
- **Presentation Manager**, drawn into Wayland windows that the Workplace
  Shell frames:
  - windows, message queues, painting, timers and message boxes;
  - resources: menus, dialogs, accelerators and string tables;
  - the controls: buttons, entry fields, list boxes, combo boxes, scroll
    bars, the MLE, spin buttons, sliders, circular sliders, value sets,
    notebooks and containers (icon, name, text, details and tree views,
    editing in place), owner-drawn items;
  - GPI text in any size (DejaVu fonts); lines, arcs, polygons, areas
    and paths with line types, patterns and mixes; markers; bitmaps,
    BitBlt, memory presentation spaces, icons and pointers;
  - the clipboard, shared with the desktop's, and drag and drop between
    a program's windows;
  - profiles (OS2.INI and programs' own .INI files, in OS/2's format), a
    help manager instance (the help itself isn't shown yet) and the
    standard file and font dialogs.

OS/2 system DLLs it doesn't have load as stubs. A call it doesn't
implement prints one line and fails, and `OS2X86_TRACE=1` traces every
call. `scripts/desktop.sh --os2-demos` puts its test programs on the
stick: real C, C++ and PM programs compiled with OpenWatcom. The details
are in `userland/README.md`.

---

## 8. File systems and storage

```
shell commands, os2api (DosOpen, DosFindFirst, DosResolvePath, ...), Linux calls
        │
kernel/src/vfs.c  (mounts, open files, path dispatch, drive letters)
        │
   HPFS · FAT16/32 · ext4 · btrfs · ramfs (/tmp)
        │
blkdev (GPT, MBR)  ←  virtio-blk  ·  USB mass storage (xHCI)  ·  SD (boards)
```

| driver | where | reads | writes |
|---|---|---|---|
| **HPFS** (`fs/hpfs`) | the USB stick, D: | yes | yes: files (extents in the fnode or anodes) and directories; clean/dirty marking, and a full check (`AUTOCHECK`, `CHKDSK`) before a dirty volume is mounted read-write |
| **FAT16/FAT32** (`kernel/src/fat32.c`) | `MOUNT`ed volumes, the BOOT partition, SD cards | yes | create, write, mkdir, delete, rename |
| **ext4** (`fs/ext4`) | C: on the test disk, or `MOUNT`ed | yes | create, write, truncate, delete, mkdir, rmdir, rename (not with `metadata_csum`/`uninit_bg`: read-only then); checked against `e2fsck` by `scripts/test_ext4_driver.sh` |
| **btrfs** (`fs/btrfs`) | C: on the test disk (`C_FS=btrfs`) | yes | copy-on-write with checksummed nodes, but no extent or checksum tree: written volumes are for this system only |
| **f2fs** (`fs/f2fs`) | C: when it's f2fs, or `MOUNT`ed | yes | create, write, truncate, delete, mkdir, rmdir, rename; superseded blocks freed, and a segment cleaner (owners from the segment summaries) when free segments run low; self-checked only (`scripts/test_f2fs_driver.sh`, whose `--format` makes an empty volume) |
| **ramfs** (`kernel/src/ramfs.c`) | `/tmp` | | |

The stick (`userland/make-stick.sh`, built by `scripts/mkhpfs.py`) holds
the desktop and its programs in a hidden `\OS2` folder, and is UNIXROOT
(`SET UNIXROOT=D:` in `CONFIG.SYS`). Linux programs see its `/usr`,
`/etc`, `/bin` and `/var`, so installed packages stay there.

`MOUNT device \dir` takes any of these (by their superblocks), and
virtio disks after the first are `disk1`, `disk2`, ... (their MBR
partitions `disk1p1`, ...), for volumes to try out:
`-drive if=none,id=x,file=vol.img,format=raw -device virtio-blk-device,drive=x`.

C: in QEMU is a GPT disk image (`scripts/mktestdisk.sh`: a FAT16 BOOT
partition and an ext4 or btrfs C: partition), given to the kernel as a
virtio-blk disk (`kernel/src/blkdev.c`, polled, on virtio-mmio): what the
system writes to C: is kept in `build/testdisk.img`. (Without one, the
kernel falls back to a 24 MB RAM window QEMU can preload.)

---

## 9. Networking

A virtio-net driver (`kernel/src/net/virtio_net.c`) and lwIP 2.2.0
(`external/lwip`) give the kernel the network. IPv4 has DHCP, DNS, ICMP,
UDP and TCP. IPv6 has link-local and SLAAC addresses. The interface is
`lan0`.

- **The shell**: `PING`, `IFCONFIG` and `HOST`.
- **Linux programs**: TCP, UDP and raw ICMP sockets, dual-stack IPv6,
  loopback, poll/epoll. The name servers come from DHCP when there's no
  `/etc/resolv.conf`.

So busybox's `wget` (also HTTPS, without certificate checks), `nc`,
`nslookup` and `ping6` work, and so does WebExplorer.

```
sh -c "wget -qO- http://example.com/ | head"
rpm -i make                   # from the host's repository: userland/serve-rpms.sh
```

---

## 10. The hypervisor

When QEMU gives the kernel EL2 (`scripts/run-vm.sh` without `--hvf`:
`virtualization=on`, emulated), `kernel/src/hyp.c` is a small type-2
hypervisor:

- stage-2 page tables;
- a vCPU run loop that turns traps into MMIO exits;
- a virtual GIC (`vgic.c`) and an emulated PL011 (`vm_pl011.c`).

`VM TEST` boots a built-in hello guest (`vm_guest_hello.S`) and connects
the console to it. Ctrl+] gets back to the host. `scripts/run-vm.sh
--test` runs `VM TEST` non-interactively and checks its output.

---

## 11. Other boards

### Raspberry Pi 3 / 3+

```bash
cmake -B build-rpi -DTARGET_PLATFORM=RPI3 \
      -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang -DCMAKE_ASM_COMPILER=$(brew --prefix llvm)/bin/clang
cmake --build build-rpi        # build-rpi/kernel8.img
```

Copy `kernel8.img` to a FAT32 card with the Pi firmware files
(`bootcode.bin`, `start.elf`, `fixup.dat`) and a `config.txt` (see
`bootloader/config.txt`). The console is the PL011 UART at 115200 baud:
GND on pin 6, the cable's RX on pin 8 (TX), its TX on pin 10 (RX). The
Pi's entry code (`bootloader/rpi_boot.S`) drops from EL2/EL3 to EL1 and
parks the other cores. This port is less exercised than QEMU.

### BeagleBone Black, BeagleV-Ahead, QEMU RISC-V

```bash
cmake -B build-bbb -DTARGET_PLATFORM=BBB -DCMAKE_C_COMPILER=... -DCMAKE_ASM_COMPILER=...
cmake --build build-bbb                     # or: ci/scripts/build-beagle.sh BBB

cmake -B build-qemu-riscv -DTARGET_PLATFORM=QEMU_RISCV -DCMAKE_C_COMPILER=... -DCMAKE_ASM_COMPILER=...
cmake --build build-qemu-riscv --target run # Ctrl-A X quits
```

These ports build a smaller kernel:

- boot code: `bbb_boot.S` or `beaglev_boot.S`;
- a 16550 UART driver;
- a serial prompt: VER, HELP, MEM, ECHO, CLS, EXIT, plus CD, DIR and TYPE
  through the OS/2 API;
- a microSD driver (`sdhci.c`: the AM335x MMCHS, the TH1520's DWC MSHC,
  QEMU's `sdhci-pci`), MBR/GPT probing, FAT16/32 and ext4.

The boot FAT partition becomes C:, other volumes `\<partition>`.

| | BeagleBone Black | BeagleV-Ahead | QEMU RISC-V |
|---|---|---|---|
| CPU | Cortex-A8 (ARMv7, 32-bit) | 4x XuanTie C910 (RV64GC) | RV64GC, 4 harts |
| Boot | U-Boot `bootz` at 0x82000000 | U-Boot `booti` at 0x200000 | OpenSBI, `-kernel` at 0x80200000 |
| Console | UART0, J1 header (3.3 V, 115200) | UART0 debug header | ns16550a |
| Card | `mmc 0:1` (hold S2 at power-on) | `mmc 1:1` | `scripts/mksdcard.py`: FAT32 + ext4 |

For a board, copy the image and the generated `uEnv.txt` to the card's
FAT partition. To boot by hand from U-Boot:

```
load mmc 0:1 0x82000000 os2warp-bbb.img; bootz 0x82000000 - ${fdtcontroladdr}
load mmc 1:1 0x200000 os2warp-beaglev-ahead.img; booti 0x200000 - ${fdtcontroladdr}
```

Neither board is emulated by QEMU, and neither image has been run on real
hardware yet. The same boot code and drivers were smoke-tested in QEMU
with only the device addresses changed (`ast2600-evb` for ARMv7,
`riscv64 virt` for RV64). These ports have no MMU, interrupts, GUI or LX
loader. LX programs contain AArch64 code, so they couldn't run on these
CPUs anyway.

---

## 12. BASIC, DEBUG and AI

**BASIC** is a QuickBASIC-style interpreter (`kernel/src/basic.c`).
Type `BASIC` to start it and `BYE` to leave. It supports:

- line numbers, `LIST`, `RUN`, `NEW`, `RENUM`;
- `PRINT`, `INPUT`, `LET`;
- block `IF`, `SELECT CASE`, `FOR`, `WHILE`, `DO ... LOOP`;
- `GOTO`, `GOSUB`, `ON ... GOTO`;
- `READ`/`DATA`, one-dimensional arrays, `CONST`, `SWAP`;
- `CLS`, `LOCATE`, `COLOR`;
- string and numeric functions (`MID$`, `INSTR`, `HEX$`, `SQR`, `RND`,
  `INKEY$`, ...).

```basic
10 FOR I = 1 TO 10 : PRINT I * I : NEXT I
RUN
```

**DEBUG** is a memory monitor in the style of DEBUG.COM. It has dump,
enter, fill, compare, move, search and hex arithmetic over the flat 64-bit
address space, and shows the AArch64 registers. `?` lists its commands;
the x86-only ones say so.

**AI** is a small on-box assistant. It is not a language model; it
watches the commands you use. `AI STATUS` summarises the session, `AI
STATS` counts commands, and `AI TIP` suggests a command you haven't used.

---

## 13. Tests

| | |
|---|---|
| `userland/build-tests.sh` | builds the Linux-ABI test programs (`userland/tests`: processes, threads, signals, IPC, ptys, `/proc`, demand paging, Wayland, wlroots); run them with `RUNELF \h\name` |
| `userland/os2x86/test/run-tests.sh` | the x86 emulator on the build machine: a freestanding test program, and with OpenWatcom (`WATCOM`, or unpacked in `build/os2x86-test/ow`) real C, C++, conio, DLL, mouse and 12 headless Presentation Manager programs, scripted and their output checked |
| `scripts/test_ext4_driver.sh` | the ext4 write path against `mke2fs`/`e2fsck`/`debugfs` |
| `scripts/test_f2fs_driver.sh` | the f2fs driver against itself |
| `scripts/check_btrfs_image.py`, `scripts/hpfsck.py` | independent checkers for btrfs and HPFS images |
| `scripts/run-vm.sh --test` | boots QEMU and checks the hypervisor's guest |

---

## 14. Repository layout

```
os24arm/
├── CMakeLists.txt         the build: platforms, kernel, LX programs, run targets
├── CONFIG.SYS             the system's configuration (UNIXROOT, RUNWORKPLACE, ...)
├── bootloader/            QEMU and Raspberry Pi entry code, linker scripts, U-Boot scripts
├── kernel/
│   ├── include/
│   └── src/
│       ├── main.c             kernel_main, the shell's commands, MOUNT, DESKTOP, SHUTDOWN
│       ├── mmu.c, mem.c, scheduler.c, gicv3.c
│       ├── linux/             the Linux ABI: system calls, EL0 entry, ELF and LX processes
│       ├── net/               virtio-net, lwIP glue, sockets
│       ├── gui/               virtio-gpu, ramfb, virtio-input, the in-kernel compositor
│       ├── lx_loader.c, lx_dll_loader.c, doscalls.c   OS/2 LX programs
│       ├── hyp.c, hyp_entry.S, vgic.c, vm_*.c          the hypervisor
│       ├── fat32.c, ramfs.c, vfs.c, blkdev.c, gpt.c, mbr.c
│       ├── basic.c, ai.c, unixroot.c
│       └── beagle_main.c, bbb_boot.S, beaglev_boot.S, ns16550.c, sdhci.c, qemu_pci.c
├── drivers/usb/           xHCI, USB core, keyboard, mouse/tablet, mass storage
├── fs/                    hpfs, ext4, btrfs, f2fs
├── os2api/                the OS/2 API (DosOpen, DosFindFirst, drive letters, ...)
├── dos/                   the shell's commands, DOSKEY, DEBUG; CMD.EXE
├── lx/                    host tools that build LX executables and DLLs
├── external/lwip/         lwIP 2.2.0
├── userland/
│   ├── README.md          the user mode in detail: the Linux ABI stage by stage, the desktop, os2x86
│   ├── build-*.sh         musl, libraries, busybox, compositors, RPMs, the shell, tests
│   ├── make-stick.sh      the HPFS USB stick
│   ├── wps/               the Workplace Shell
│   ├── wlr-os2/           wlroots backend for /dev/fb0 and evdev
│   ├── os2x86/            the x86 OS/2 emulator and Presentation Manager
│   ├── apps/              os2cmd (Command Prompt), the applet launcher
│   ├── rootfs/            the stick's files: scripts, help, fonts and terminal settings
│   ├── rpm/               package specs, patches, macros, source checksums
│   └── tests/             Linux-ABI test programs
├── scripts/               desktop.sh, run-vm.sh, disk and stick image builders, checkers
├── rexx/                  an early REXX experiment (linked, unused: userland/apps/rexx.c is the REXX)
├── tools/kcc/             the Kayte C Compiler (an alternative cross-compiler, -DUSE_KCC=ON)
├── kayte/                 the Kayte language toolchain (Free Pascal, separate)
├── workplace/, installer/ an earlier Qt Workplace Shell and an installer (not built)
└── docs/                  Doxygen output (from Doxyfile)
```

---

## 15. Status and what's missing

Works: boot in QEMU to the console or the desktop, Linux and OS/2
programs as processes, the desktop with its folders, editor, browser and
terminals, USB keyboards, mice and sticks, networking, packages and a
compiler on the system, x86 OS/2 text and PM programs. The kernel
develops in QEMU; the Raspberry Pi port is less tested, and the
BeagleBoard and RISC-V ports are early, serial-only and smoke-tested in
QEMU only.

Not there yet:

- **Storage**: the btrfs extent and checksum trees; checking f2fs against real f2fs-tools.
- **The kernel**: finer-grained locking than the one big kernel lock (system
  calls from different CPUs wait for each other); glibc-linked programs
  (musl's dynamic linker only).
- **Graphics**: GPU acceleration (the desktop renders with pixman).
- **os2x86**: PM drag and drop between programs (within one works), IPF
  help files, and
  the rest of OS/2's API (the help manager, multimedia, ...).
- **Other**: DEBUG's assembler, disassembler and breakpoints; REXX's
  RexxUtil's less used functions (extended attributes, WPS objects), and
  REXX strings holding NUL bytes (they're C strings: `'00'x`, and binary
  .INI values with a zero byte, end at the first NUL).

---

## 16. License

GPLv3, see `LICENSE`. Third-party code keeps its own license: lwIP
(`external/lwip`), stb_truetype and stb_image (`third_party/` folders),
and the packages built from source by the `userland` scripts.
