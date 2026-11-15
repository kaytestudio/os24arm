# Userland: a Linux-compatible user mode, towards wlroots

Goal: run a [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)
compositor as the display server on this kernel.

wlroots is a Linux userspace library, not a drop-in component. It needs:

| needs | provided by, on Linux |
|---|---|
| a C library | glibc / musl |
| processes with their own address spaces, `fork`/`exec`/`posix_spawn` | kernel |
| file descriptors, `read`/`write`/`poll`, `epoll`, `timerfd`, `signalfd`, `eventfd` | kernel |
| Unix domain sockets with `SCM_RIGHTS` fd passing (the Wayland wire protocol) | kernel |
| shared memory: `memfd_create` + `mmap(MAP_SHARED)` (wl_shm buffers) | kernel |
| `libwayland-server` (+ `libffi`), `pixman`, `libxkbcommon` | userspace libraries |
| a backend: DRM/KMS output, libinput/evdev input, udev | kernel + libraries |

This kernel has none of the kernel side (everything runs at EL1, one
address space, no syscalls). The plan is therefore **Linux-ABI
compatibility**: the kernel runs unmodified static AArch64 Linux ELF
binaries at EL0 and implements Linux system calls itself. Then musl,
libwayland, libffi, pixman, libxkbcommon and wlroots build unmodified with
a standard `aarch64-linux-musl` cross toolchain (clang + lld + a musl
sysroot built here), and only the parts that are genuinely hardware- or
platform-specific are new code.


7. **UNIXROOT packages, as RPMs** *(binutils installs, assembles and links)*.
   UNIXROOT is the USB stick, D: (CONFIG.SYS `SET UNIXROOT=D:`): the Linux
   layer maps /usr, /etc, /bin, /var, /home, /lib, /opt, /sbin, /emx and
   kLIBC's /@unixroot/... onto it, so what's installed there stays.
   - Packages are built by rpmbuild (Homebrew's rpm 6) from
     `userland/rpm/<name>.spec`: `%prep` unpacks the release tarball and
     applies the patches next to the spec, `%build` cross-compiles with
     the system's toolchain (`%{os2_configure}`, `%{os2_cc}`, ... in
     `userland/rpm/macros.os2warp`), `%install` into the buildroot.
     `userland/build-rpms.sh [name]` downloads each Source once, checks it
     against `userland/rpm/sources` (SHA-256, and the GNU signature), and
     runs `rpmbuild -ba`: build/userland/rpms/*.rpm and SRPMS/*.src.rpm.
     The macros make what busybox's rpm reads (format 4, xz payload,
     linux/aarch64, the /usr layout, no Mac strip, debuginfo or automatic
     dependencies, a neutral build host); make-stick.sh puts the RPMs in
     \OS2\RPMS.
   - `binutils.spec`: GNU binutils 2.44, static, native aarch64-linux-musl;
     `binutils-2.44-os2warp-ld-base.patch` makes ld link at the kernel's
     user window (0x60000000), so `as x.s -o x.o; ld x.o -o x` runs here.
   - `make.spec`: GNU make 4.4.1 (no dynamic "load", no Guile); recipes
     run through UNIXROOT's /bin/sh, a launcher for busybox's sh
     (userland/apps/applet.c, put there by make-stick.sh).
   - `musl-devel.spec`: musl 1.2.5 headers, libc.a and crt*.o for /usr.
   - `tcc.spec`: tcc 0.9.27, a C compiler on the system: rpmbuild builds a
     cross tcc for the Mac first (it makes libtcc1.a), then the tcc that
     runs here. `tcc-0.9.27-os2warp.patch`: static programs by default at
     0x60000000, the AArch64 relocations clang uses in libc.a, the GOT of
     a static program filled in, libc after libtcc1.a. With musl-devel,
     `tcc hello.c -o hello` on the system makes a program that runs.
   - `llvm.spec`: LLVM 23.1.5 - clang (C, C++) and lld on the system.
     rpmbuild builds the C++ runtimes for aarch64-linux-musl first
     (compiler-rt builtins and crt objects, libunwind, libc++abi, libc++;
     the `libcxx-devel` subpackage), then clang + lld against them as one
     static multi-call `llvm` (78 MB, linked in the kernel's image area at
     0x1_0000_0000 - too big for the user window) with Homebrew's tblgen
     tools (the same version). `llvm-launcher.c` stands in for the
     symlinks (clang, clang++, cc, c++, ld.lld, llvm-ar, ...);
     `/etc/clang/aarch64-unknown-linux-musl.cfg` makes programs static at
     0x60000000. With musl-devel (and libcxx-devel for C++), `clang hi.c
     -o hi` and `clang++ -O2 x.cpp -o x` build programs that run here.
   - On the system, `rpm` (userland/rootfs/rpm, busybox's rpm underneath,
     a database in /var/lib/rpm): `rpm -i binutils` (a file, an http(s)://
     address, or a name - from \OS2\RPMS, else from the repository), `-U`,
     `-e`, `-q`, `-qa`, `-qi`, `-ql`, `-qp[il]`, `-qA` (what the repository
     has). The repository: /etc/rpm.repo's address, else
     http://10.0.2.2:8080/ - the host, where `userland/serve-rpms.sh`
     serves build/userland/rpms with an index.txt. (In `sh`, `rpm` is
     busybox's applet: busybox prefers its applets; /h/OS2/rpm is this one.)
   - From the OS/2 prompt, arguments that are OS/2 paths (D:\x.s, src\x.c)
     reach programs as the paths they can open; `#!` scripts run (an
     interpreter that isn't there is busybox's applet of that name).

## Stages

1. **EL0 + Linux syscall ABI + static ELF loader** *(done)*.
   `svc #0` with the syscall number in `x8`, arguments in `x0`-`x5`, result
   in `x0` (negative errno). ELF64 `ET_EXEC` loader with per-segment
   permissions, initial stack with argv/envp/auxv, `brk`, anonymous
   `mmap`, file I/O through the VFS, console I/O. One program at a time,
   in a fixed user window of the identity map. Shell: `RUNELF path args`.
   Test: musl-built programs (hello, malloc, file read, clock).
2. **Processes** *(done)*.
   Per-process address spaces (page frames from a pool at
   0x64000000-0x80000000, TTBR0 switch), per-process kernel stacks, up to
   16 processes. `clone` as `fork` (address space copied, `MAP_SHARED`
   pages shared) and as `CLONE_VM|CLONE_VFORK` (musl's `posix_spawn`),
   `execve`, `exit` / zombies / `wait4`, `getpid`/`getppid`, shared
   open-file objects (fork and `dup` share offsets), close-on-exec,
   per-process `tpidr_el0`. Preemptive scheduling: the EL1 physical timer
   (PPI 30, GICv3) interrupts EL0 every 10 ms and the next runnable
   process gets the CPU (the kernel itself runs with interrupts masked);
   waits can have deadlines. Threads: a process is a thread group
   (shared address space, fds, signal handlers, cwd); `clone` with
   `CLONE_THREAD` (`SETTLS`, `PARENT_SETTID`, `CHILD_SETTID`,
   `CHILD_CLEARTID`), `set_tid_address`, `futex` (`WAIT`, `WAKE`,
   `REQUEUE`, `CMP_REQUEUE`, `*_BITSET`, timeouts; keyed by address space,
   so not across processes), `gettid`/`tgkill`, `exit` (one thread) vs
   `exit_group`, fatal signals and `execve` ending the other threads,
   process-directed signals delivered to a thread that doesn't block them.
   Tests: `procs` (fork/wait, pipes, posix_spawn, execve, kill), `cat`
   (console input, Ctrl-D, Ctrl-C), `preempt` (children spinning without
   system calls don't starve the parent), `threads` (pthreads: mutexes,
   condition variables, TLS, join, exit/execve/signals with threads).
   Memory: demand paging. Each process has a 64 MB window at 0x60000000
   (program image, brk heap, a 16 MB stack) and a 4 GB mmap region at
   0x1_0000_0000; the heap, the stack and every mapping are VM areas
   whose pages arrive on first touch (EL0 translation faults, and the
   kernel's own user-memory accesses). `mmap` (anonymous, shared, memfd,
   file copies, the framebuffer, `MAP_FIXED[_NOREPLACE]`), `munmap`,
   `mprotect` and `mremap` split and merge areas; PROT_NONE guard pages
   fault. Shared memory objects are sparse (up to 1 GB), and
   `fallocate(PUNCH_HOLE)` frees pages and unmaps them everywhere.
   Test: `vmtest`.
3. **Event and IPC primitives** *(done)*.
   Signals: `rt_sigaction` handlers run on the user stack with a
   Linux-layout `rt_sigframe` (siginfo, ucontext, FP/SIMD record) and
   return via `rt_sigreturn`; `SA_RESTART`/`SA_NODEFER`/`SA_RESETHAND`,
   masks, pending sets, `rt_sigsuspend`, default actions, SIGCHLD on child
   exit, SIGPIPE, Ctrl-C -> SIGINT. `ppoll`, `pselect6`, `epoll`
   (`create1`/`ctl`/`pwait`/`pwait2`, `EPOLLONESHOT`), `eventfd2`,
   `timerfd_*`, `signalfd4`, `pipe2`; `AF_UNIX` stream sockets (`bind`/
   `listen`/`accept4`/`connect`, `socketpair`, `sendmsg`/`recvmsg` with
   `SCM_RIGHTS`, `SO_PEERCRED`, `shutdown`); `memfd_create` + `ftruncate`
   + `mmap(MAP_SHARED)` (frames are reference counted and shared between
   processes), shared anonymous mappings; `/dev/null`, `/dev/zero`,
   `/dev/urandom`; `/tmp` is a RAM file system (`kernel/src/ramfs.c`,
   `XDG_RUNTIME_DIR=/tmp`), `flock` always succeeds (one user). Not done:
   datagram/seqpacket sockets.
   `/proc`: the process list, `self`, `meminfo`, `uptime`, `loadavg`,
   `stat`, `version`, `cpuinfo`, `mounts`; per process `stat`, `status`,
   `statm`, `cmdline`, `comm`, `exe`, `fd/`; and `sysinfo` - enough for
   busybox `ps`, `top`, `free`, `uptime`, `df` (CPU time: the 10 ms tick
   charges the process it interrupts). `statfs` reports each volume's
   real size and free space (the VFS `statfs` hook: FAT, HPFS, btrfs,
   ramfs) and each volume has its own `st_dev`.
   Test: `proctest`.
   Terminals: the console and pseudo-terminals (`/dev/ptmx`, `/dev/pts/N`)
   share one line discipline - Linux termios (`TCGETS`/`TCSETS*`;
   canonical editing with VERASE/VKILL/VWERASE/VEOF, raw mode, ECHO,
   ISIG, ICRNL, OPOST/ONLCR), window size with SIGWINCH, controlling
   terminals (`setsid`, `TIOCSCTTY`, `/dev/tty`), process groups and
   sessions (`setpgid`, `TIOCSPGRP`; Ctrl-C goes to the foreground group),
   hang-up when the master closes.
   Tests: `ipc` (all of the above), `fsops` (directory ops on a volume),
   `ptytest` (an interactive busybox shell on a pty, driven through the
   master: commands, Ctrl-C, resize, exit).

   A shell: `userland/build-busybox.sh` builds busybox 1.37 (static, ash
   with the applets built in; `BB_GLOBAL_CONST=''`, as busybox advises
   for compilers that see through its const-globals trick). `RUNELF \h\busybox sh` gives an
   interactive shell on the console - line editing, pipes, redirection,
   job control, Ctrl-C.
4. **Libraries** *(done)*. `userland/build-libs.sh` cross-builds, as
   static libraries in the musl sysroot: Linux UAPI headers (sabotage's
   sanitized 4.19 kernel-headers), libffi 3.5.2, libwayland 1.24.0
   (server, client, cursor, egl), pixman 0.46.4, libxkbcommon 1.11.0 (no
   X11, no registry; keymaps from strings until an xkeyboard-config tree
   is installed), plus a host `wayland-scanner` (with a host expat).
   Test: `wltest` - a `wl_display` with `wl_shm` and a minimal
   `wl_compositor`, a client in a forked process that connects through
   `/tmp/wayland-0`, attaches a memfd-backed ARGB buffer and gets it
   released; the server reads it with `wl_shm_buffer`, composites it with
   pixman and checks every pixel; xkbcommon compiles and serialises a
   keymap. Passes with no unimplemented system calls.
5. **wlroots on this machine** *(done)*. `build-libs.sh` adds libdrm
   (core library only), wayland-protocols and wlroots 0.19.2 (pixman
   renderer, shm allocator; no DRM/libinput/X11 backends, no session, no
   XWayland). The kernel provides, with the standard Linux interfaces:
   - `/dev/fb0` - fbdev (`FBIOGET_VSCREENINFO`/`FSCREENINFO`,
     `FBIOPAN_DISPLAY`) on the kernel's framebuffer (virtio-gpu or ramfb,
     XRGB8888), `mmap`able (the front buffer's own page frames), plus a
     damage ioctl (`_IOW('F', 0xF0, {x, y, w, h})`) for virtio-gpu flushes
   - `/dev/input/event0` (USB keyboard: press/release from the HID boot
     reports, HID usage -> Linux key code; `EVIOCGRAB` keeps keys away from
     the console) and `/dev/input/event1` (USB mouse or tablet: `EV_REL` /
     `EV_ABS` 0..32767, buttons, wheel), with the usual `EVIOCG*` ioctls
   - POSIX shared memory (`/dev/shm`, `shm_open`), which wlroots' shm
     allocator uses
   `userland/wlr-os2/os2_backend.c` is the wlroots backend: one output on
   `/dev/fb0` (damage copied from the rendered frame, 60 Hz frame timer,
   software cursor) and evdev keyboard/pointer devices. `keymap_us.h`
   (from `gen-keymap.py`) is a built-in US keymap for when xkeyboard-config
   isn't installed.
   Tests: `wlrtest` (headless backend, scene graph, xdg-shell client,
   output pixels checked), `evtest` (prints evdev events).
6. **Compositors and a terminal** *(tinywl, cage, dwl and foot run)*.
   `userland/build-compositor.sh` builds wlroots' `tinywl`, the `cage`
   kiosk compositor (0.2.1), the `dwl` tiling compositor (0.8, without
   libinput's pointer settings), the `foot` terminal (1.28.0) and the
   `fuzzel` launcher (1.15.0, no icons), the compositors patched at
   build time - os2 backend, the built-in keymap when
   xkeyboard-config is missing, `tinywl -s` without a shell, cage without
   VT switching or XWayland - and `wlclient`, a small xdg-shell client.
   foot uses fcft, freetype and fontconfig (`build-libs.sh`) with DejaVu
   Sans Mono, and busybox as its shell (the Workplace Shell uses DejaVu Sans). `userland/make-stick.sh` puts it
   all on an HPFS stick image with `userland/rootfs/` (`env.sh`,
   `desktop.sh`, `dwl.sh`, `wps.sh`, `help/`, `fonts.conf`, `foot.ini`, `fuzzel.ini`, and
   `applications/*.desktop` - what the launcher offers).

   Try it: `scripts/run-vm.sh --wayland --hvf` (`--hvf`: native speed
   with Apple's Hypervisor.framework; without it QEMU emulates (TCG),
   which the kernel's own hypervisor needs, and the desktop is slow),
   and the desktop starts by itself at boot (CONFIG.SYS `SET RUNWORKPLACE=`
   `...\PMSHELL.EXE`; `CMD.EXE` keeps the console; a key on the console
   while it waits for the stick does too). By hand: `DESKTOP [layout
   [variant]]` (e.g. `pt`, `de nodeadkeys`; default `us`) - it mounts the
   stick at `\h` and starts the Workplace Shell (`wps.sh`; dwl, `dwl.sh`,
   when the stick has no wps). The user
   sees an OS/2 system: every terminal is an OS/2 Command Prompt (foot's
   `shell=` is `/h/OS2/os2cmd`; Alt+Shift+Enter and Alt+O open one), Alt+P
   lists OS/2 Command Prompt, System Monitor and Text Editor, and the
   compositor's and terminals' own output goes to `/tmp/desktop.log`
   rather than the console. Alt+Shift+Q goes back to the console.

   The Workplace Shell (`userland/wps`, built by `userland/build-wps.sh`)
   is a wlroots compositor in C++17, linked with the system's own libc++
   (the `libcxx-devel` and `llvm` packages), in the style of OS/2 Warp 4:
   the blue desktop with its objects (OS/2 System, Drives, Command Prompt,
   Drive D, Programs, Window List; Shut Down and the Shredder bottom right - double-click
   one, right-click the desktop for the system menu), the WarpCenter
   along the top (system menu, Window List, Command Prompt, Drives,
   Programs, the active window, the clock), and an OS/2 frame on every
   window (server-side decoration: a 3D border to resize by, a title bar
   to move by - blue when active, double-click maximizes, right-click a
   menu - the system menu icon (double-click closes) and hide, maximize
   and close buttons; a window that fixed its size - a dialog, a message
   box - has no maximize and isn't resized). Folders open as windows of the real directories
   (UNIXROOT's and the system's folders stay hidden), with a menu bar -
   Folder (Open parent, Refresh now, Close), Edit (Select all), View
   (Icon, Tree and Details views; Sort by name, type, size or last
   write - or a click on a Details column's heading), Selected (Open, Open as Tree view,
   Properties), Help - and a scroll bar. Double-click a folder to open
   it, a program (Linux or OS/2) to run it in a prompt, a file to edit
   it. Right-click an object for its menu. Drag an
   object (left button) onto a folder, a folder window or a drive: on the
   same drive it moves, to another drive it's copied (OS/2's rule - Shift
   forces a move, Ctrl a copy; Esc cancels the drag); onto the Shredder
   (bottom right) it's deleted, after a confirmation. Folder > Create new
   folder; Selected > Rename... (F2) and Delete... (Del). Several
   objects at once: Ctrl+click, Shift+click (a range), a rubber band
   from the folder's background, Ctrl+A; dragging one of them takes them
   all (to a folder, the desktop - shadows - or the Shredder: one
   question for all). Keys in a
   folder: arrows, Home/End, Page Up/Down, Enter opens, Backspace the
   parent, F5 refreshes, F10 the menu bar (arrows, Enter, Esc); in a
   Tree view Right/+ expands and Left/- collapses.
   Programs is a folder of program objects - the launcher's entries
   (`\OS2\applications\*.desktop`) - and the WarpCenter's Programs button
   a menu of them. Desktop objects can be dragged where you want them,
   and the desktop is kept as it was left: the objects' places and the
   open folder windows (view, place, size, order) are in `\OS2\WPS.INI`
   on the stick, written as they change, and come back at the next boot.
   Shadows, as in OS/2: drop an object (a file, a folder, a program)
   on the desktop and a shadow of it is made there - a link, its title
   in a lighter blue; opening it opens the original, right-click has
   "Open folder of original", and the Shredder deletes only the shadow.
   OS/2 System has a Startup folder: shadows dropped into it are started
   whenever the desktop starts. Shadows are kept in WPS.INI too.
   The desktop's Settings notebook (right-click the desktop, Properties;
   tabs on the right, as in Warp 4): Background - the colour (twelve)
   and the pattern on it - and Window - the title bars' colours (Warp 4,
   Warp 3, Teal, Graphite, Plum). Changes show at once and are kept in
   WPS.INI.
   Text files open in the System Editor (OS/2 System has one to start
   with an empty file): a window of the Workplace Shell's own, with a
   File/Edit/Help menu bar, DejaVu Sans Mono, Shift+arrows or a drag to
   select, Ctrl+X/C/V/A/S, Enter keeping the indentation, the line and
   column below; File > Save as takes a DOS path (D:\NOTES\TODO.TXT);
   closing it with unsaved changes asks Save, Discard or Cancel. Binary
   files (a NUL in them) and files over 4 MB aren't opened. Its
   clipboard is the seat's selection (a wlr_data_source of the shell's,
   written to the reader through the event loop; another program's read
   the same way), so text goes both ways between it and foot.
   Connections (on the desktop) is the network: TCP/IP Status (the
   kernel's IFCONFIG, its output read through a pipe without the shell
   waiting), Ping and Telnet (they ask for a host, then run in a Command
   Prompt window).
   WebExplorer (on the desktop, and in Connections) is a web browser of
   the shell's own: busybox's wget fetches (HTTP, and HTTPS through its own
   TLS - certificates not checked), a parser here takes the HTML apart
   (headings, paragraphs, lists, pre, links, anchors, entities, forms -
   text fields, submit buttons, hidden fields, a select's choice, sent by
   GET or POST; scripts and styles left out) and lays the text
   out to the window, with its pictures (PNG, JPEG, GIF, BMP: fetched
   four at a time after the page, decoded by stb_image - userland/wps/
   third_party - scaled to fit; SVG and WebP as their names). file:
   addresses show local files; picture files in folders open there.
   Back, Forward, Reload, Home, an address bar
   (Ctrl+L), the link under the pointer in the status line.
   The Assistance Center (on the desktop, F1, a folder's Help menu) is
   the help: the topics on the left, the text on the right - plain text
   files in `userland/rootfs/help` (`\OS2\HELP`: a title line, then
   paragraphs, wrapped to the window; lines starting with two spaces
   kept as they are).
   Ctrl+Esc is the Window List (hidden windows too), Alt+Tab the next
   window, Alt+F4 closes. The pictures are drawn by the compositor
   (pixman; the icons are its own), the text by fcft with DejaVu Sans.
   With dwl instead: Alt+J/K moves the focus, Alt+Shift+C closes a window.
   Shut Down (Alt+P) or SHUTDOWN in a prompt ends the programs, unmounts
   every drive cleanly (HPFS marked clean) and powers off - closing QEMU's
   window instead leaves the stick marked dirty; the next MOUNT checks it
   (AUTOCHECK, fs/hpfs hpfs_check: every fnode, dnode, anode and extent,
   against the bitmaps) and, when it's sound, marks it clean and mounts it
   read-write - otherwise it stays read-only and CHKDSK \h lists what's
   wrong. Nothing is ever freed or moved.
   (`RUNELF \h\busybox sh /h/desktop.sh` instead: one prompt in cage.)

   OS/2 programs (LX: CMD.EXE, KAYTEVM.EXE, ...) run as processes too:
   type `CMD`, `kaytevm` or `RUN C:\HELLO.EXE` in a prompt (a name finds
   NAME.EXE, in any case). The kernel's execve recognises an LX image
   (`kernel/src/linux/linux.c`, "OS/2 programs as processes") and maps
   it, at EL0, where it's linked: the LX window, 0x48000000 (`mmu.h`
   MMU_LX_BASE; every address space has its own) - code read-only and
   executable, its data, and on demand its .bss, its heap, and the DLL
   it loads itself (DOSCALLS.DLL at 0x4A000000, made executable through
   the kernel). `main()` gets an `lx_services` table, version 3
   (`lx_services.h`): console output and input are its stdout and stdin
   (`kernel/src/linux/lx_stubs.S`, EL0 code in a page of its own), the
   kernel's file systems one system call (`LX_SYS`, the `lx_vfs_ops`),
   the clock `clock_gettime`; the handles it opens are closed when it
   ends. The programs' `uart.c` uses those instead of the UART when
   they're there; the terminal is raw while one runs (they echo
   themselves). `RUN` at the system console starts them the same way,
   in the shell's directory, the console raw while they run.

   16-bit OS/2 1.x programs (NE executables) run too (`ne16.inc`): each
   segment in a 64 KB tile of its own (its selector the tile's), the
   relocations applied (offsets, selectors, far pointers; chained or
   additive; internal, by entry ordinal, imported by ordinal or name),
   started as OS/2 starts them (AX the environment's selector, BX the
   command line's offset, DS the automatic data); about 50 of the 16-bit
   DOSCALLS and NLS calls (DosOpen, DosRead, DosWrite, DosChgFilePtr,
   DosFindFirst/Next, DosAllocSeg, DosGetInfoSeg, DosGetEnv, DosScanEnv
   ...) - most of them the 32-bit calls underneath, their arguments and
   results converted - and the 16-bit KBD/VIO/MOU calls the 32-bit
   programs use. The kernel and the Workplace Shell hand NE programs to
   os2x86 as they do LX ones. Not yet: a program's own 16-bit DLLs,
   DosExecPgm, threads and semaphores, 16-bit Presentation Manager.
   `OS2X86_ITRACE=1` traces every instruction (CS:IP, its bytes, the
   registers).
   32-bit x86 OS/2 programs (real OS/2 2.x-4.x console .EXEs: LX, usually
   after a DOS "MZ" stub, CPU type 386-586) run through an emulator,
   `userland/os2x86` (`\OS2\os2x86` on the stick): the kernel's execve
   treats one like a `#!/h/OS2/os2x86` script. `cpu.c` is an i386 in
   software (the 386/486 integer instructions, CMOV, BSWAP, CPUID, the
   x87 with doubles inside) with a flat 4 GB address space of its own (512
   MB of it backed, `OS2X86_MEM=MB` for more); `os2x86.c` places the LX
   objects where they're linked (or elsewhere, if they have fixups),
   unpacks EXEPACK pages, applies the fixups, and turns the imports from
   DOSCALLS, NLS and MSG into addresses at the top, where the interpreter
   stops and the call is done with this system's own calls: about 100 of
   them - files (DosOpen ... DosFindFirst, any case on any file system),
   directories and drives (C: is /, D: the stick at /h, or
   `OS2X86_DRIVES="C=/;D=/h;E=/mnt"`), memory (DosAllocMem, in 64 KB),
   the environment and command line (DosGetInfoBlocks, the TIB at FS),
   DosExecPgm/DosWaitChild (fork and exec: x86 programs run through
   os2x86 again), exception handlers (DosSetExceptionHandler: an access
   violation, a division by zero, an illegal instruction go to the
   program's handlers, as on OS/2; unhandled, a SYS3175 report with the
   registers), threads (DosCreateThread, DosWaitThread, Suspend/Resume/
   Kill, DosEnterCritSec: each a thread here with its CPU of its own; one
   lock lets one run at a time, given up while a call waits and every
   32768 instructions), event, mutex and muxwait semaphores that really
   wait, exit lists, x86 DLLs it brings along (the program's directory,
   the current one, LIBPATH; their initialisation runs), and the 16-bit
   console calls the C runtimes reach through a 16-bit thunk (getch,
   kbhit, cgets, putch: KbdCharIn/Peek/StringIn, VioWrtTTY, VioGetMode,
   VioSetCurPos/GetCurPos, VioScrollUp/Dn, VioWrtCharStr(Att)/NChar/NCell/
   CellStr, cursor type: on this terminal, ANSI): for those the CPU has
   OS/2's tiled LDT (selector n*8+7 is the 64 KB at n*64K), 16-bit code
   and stacks, far jumps, calls and returns, LSS/LDS/LES, and the call is
   Pascal (the callee pops, the result in AX). A 16-bit call it doesn't
   have stops the program, naming it. Queues (QUECALLS: FIFO, LIFO,
   priority) and DosStartSession (a child on the same terminal; a
   related one's end goes to its termination queue, as spawn() in the C
   runtimes waits for it). The mouse (MOUCALLS: MouOpen, MouReadEventQue,
   MouGetNumQueEl, event masks, ...) from the terminal's own mouse reports
   (SGR, asked for while a program has the mouse open; the keys that come
   in between still reach KbdCharIn). And the start of Presentation
   Manager (`pm.inc`, `pm_wl.inc`): PMWIN's windows, classes, message
   queues (WinGetMsg/PeekMsg/DispatchMsg/PostMsg/SendMsg), window
   procedures called back in the program, WM_CREATE/SIZE/PAINT/CHAR/
   BUTTONn/MOUSEMOVE/TIMER/CLOSE, painting (WinBeginPaint, WinFillRect,
   WinDrawText), window words and text, timers, WinMessageBox; PMGPI's
   text (DejaVu Sans from \OS2\fonts, through stb_truetype), lines,
   boxes, colours. A frame window (WinCreateStdWindow) is a window of the
   desktop: the Workplace Shell draws its OS/2 frame, os2x86 the inside
   (an xdg-shell toplevel; the pointer and keys become PM's messages);
   the desktop starts an x86 PM program without a terminal. With
   OS2X86_PM=headless it runs without one, OS2X86_PM_SCRIPT giving it
   input ("click X Y; key abc; esc; close; shot F.ppm"). Then
   (`pm_ctl.inc`) resources (the LX resource table: DosGetResource,
   WinLoadString), menus from their templates (FCF_MENU, WinLoadMenu: a
   bar at the top of the frame, drop-downs, WM_COMMAND, MM_SETITEMATTR
   & co.), dialogs from theirs (WinDlgBox, WinLoadDlg, WinCreateDlg,
   WinProcessDlg, WinDismissDlg, WinDefDlgProc: Enter for the default
   button, Esc, Tab between the controls; WinSendDlgItemMsg & co.) and
   the controls in them: push buttons, check boxes, radio buttons (by
   group), static text and group boxes, entry fields, list boxes, with
   their BM_/EM_/LM_ messages and WM_CONTROL notifications; scroll bars
   (a control, or the frame's: FCF_VERTSCROLL/HORZSCROLL; arrows, track,
   the thumb dragged; WM_VSCROLL/HSCROLL, SBM_ messages), combo boxes (a
   drop-down list over the dialog), accelerators (FCF_ACCELTABLE,
   WinLoadAccelTable & co.) and the menu by the keyboard (Alt+its letter,
   F10, the arrows, Enter, Esc), the multi-line edit control (typing,
   the keys, its scroll bar; MLM_IMPORT/EXPORT, MLM_INSERT, selections,
   line queries; CR LF out) and spin buttons (numbers in a range, or an
   array of strings). Bitmaps (`pm_gpi.inc`): from resources and files
   (OS/2 1.x and 2.x BMPs, bitmap arrays; 1 to 32 bits a pixel),
   GpiCreateBitmap and its bits, WinDrawBitmap (stretched too),
   GpiBitBlt/GpiWCBitBlt with the usual ROPs, memory device contexts and
   presentation spaces (DevOpenDC, GpiCreatePS, GpiSetBitmap): a bitmap
   is a window never shown, so whatever draws into windows draws into
   bitmaps. Icons and pointers: WinLoadPointer from ICON/POINTER
   resources (bitmap arrays, the device-independent image; colour and
   monochrome; the AND mask the first half of the mask, the XOR the
   second), WinDrawPointer (DP_MINI, DP_INVERTED, DP_HALFTONED), system
   icons (WinQuerySysPointer: information, question, warning, error,
   drawn here; message boxes show theirs), SS_ICON, SS_SYSICON and
   SS_BITMAP statics. Fonts: DejaVu Sans, Sans Bold, Sans Mono and Sans
   Mono Bold at any size, OS/2's faces (Helv, Helvetica, Tms Rmn,
   Courier, System Proportional/VIO/Monospaced) mapped onto them -
   GpiQueryFonts, GpiCreateLogFont (bitmap ones by lMatch, outline ones
   sized by GpiSetCharBox), GpiSetCharSet, the metrics; presentation
   parameters (PP_FONTNAMESIZE, PP_FOREGROUNDCOLOR, PP_BACKGROUNDCOLOR:
   WinSetPresParam, a dialog template's PRESPARAMS; a parent's go to its
   children). Notebooks (tabs along the top, a status line, the page
   arrows; pages from dialog resources - WinLoadDlg into a window makes a
   dialog that's a child of it - BKM_INSERTPAGE & co., BKN_PAGESELECTED)
   and sliders (ticks, scale text, buttons, the arm clicked, dragged or
   moved by the keys; SLM_SET/QUERYSLIDERINFO, SLN_CHANGE). Containers
   (`pm_cnr.inc`): records and field descriptions in the program's own
   memory (CM_ALLOCRECORD, CM_ALLOCDETAILFIELDINFO), the icon, name,
   text and details views (string, number, date, time and icon columns,
   titles, separators), selection (CN_EMPHASIS), opening (CN_ENTER: a
   double-click or Enter), CM_SORTRECORD with the program's compare
   function, CM_SEARCHSTRING, a scroll bar. Double-clicks
   (WM_BUTTONnDBLCLK) for every window. The clipboard (WinOpenClipbrd,
   WinSetClipbrdData, WinQueryClipbrdData & co.; CF_TEXT, CF_BITMAP):
   on the desktop the Wayland selection, so text goes between PM
   programs and the others (Latin-1 one side, UTF-8 the other); entry
   fields (with selections now: Shift and the keys, a drag, a double-click
   for a word) and MLEs cut, copy and paste through it (Ctrl+X/C/V, Shift+Del,
   Ctrl+Ins, Shift+Ins). WinSetPointer: the desktop's pointer over the
   program's windows takes that shape (its hot spot from the file), PM's
   arrow by default. Value sets (`pm_vs.inc`: a grid of colours, text,
   icons or bitmaps; VM_SETITEM, VM_SETITEMATTR, VM_SELECTITEM,
   VN_SELECT, VN_ENTER) and circular sliders (a dial turned by dragging,
   its buttons or the keys; CSM_SETRANGE, CSM_SETVALUE, CSN_CHANGED,
   CSN_TRACKING). The container's tree view: records under records
   (RECORDINSERT's parent), the + and - buttons, tree lines, the keys
   (Right and + open, Left and - close), CM_EXPANDTREE/COLLAPSETREE,
   CN_EXPANDTREE/COLLAPSETREE, CM_QUERYRECORD by parent and child,
   sorting and removing whole branches. Editing a record's text in place
   (Alt+click, CM_OPENEDIT: an entry field over it; Enter or a click
   elsewhere keeps it - CN_REALLOCPSZ for the program to make room, then
   the text into its string, CN_ENDEDIT - Esc drops it; CN_BEGINEDIT
   first). Owner drawing: list boxes with LS_OWNERDRAW (WM_MEASUREITEM
   for the items' height, LM_SETITEMHEIGHT; WM_DRAWITEM for each, with an
   OWNERITEM and a presentation space of the list box - a selection the
   owner leaves to it is inverted), menu items with MIS_OWNERDRAW
   (measured, drawn, MIA_HILITED for the lit one) and value set items
   with VIA_OWNERDRAW. Drag and drop (`pm_drag.inc`, PMDRAG): button 2
   pressed and moved gives WM_BEGINDRAG (containers: CN_INITDRAG);
   DrgAllocDraginfo, DrgSetDragitem, string handles, DrgVerifyType/RMF
   and the other queries; DrgDrag follows the pointer - DM_DRAGOVER to
   the window under it (containers: CN_DRAGOVER with the record there),
   the pointer showing whether it may drop (OS/2's no-entry sign when
   not), Ctrl to copy, Shift to move, Esc to give up - and ends with
   DM_DROP (CN_DROP) or DM_DRAGLEAVE. Between one program's windows; not
   yet to and from other programs or the Workplace Shell. GPI's geometry
   (`pm_geo.inc`): lines with types (dotted, dashed, ...), widths and
   mixes (XOR, invert, OR, AND, ...); arcs - GpiFullArc, GpiPartialArc,
   GpiPointArc, with GpiSetArcParams' transform -, boxes with rounded
   corners, polylines, fillets, splines, polygons (alternate or winding);
   areas (GpiBeginArea/EndArea) and paths (GpiBeginPath ... GpiFillPath,
   GpiStrokePath) filled with the patterns (dense, hatched, halftone,
   ...); markers; a clip rectangle (GpiIntersectClipRectangle).
   Profiles (`pm_prf.inc`, PMSHAPI): PrfQueryProfileString/Int/Data/Size,
   PrfWriteProfileString/Data (deleting keys and applications too),
   enumerating them, PrfOpenProfile for a program's own .INI files - in
   OS/2's own binary .INI format, written back at each change; the
   user's OS2.INI and the system's OS2SYS.INI are USER_INI and
   SYSTEM_INI, else in $OS2X86_HOME (on the desktop \OS2 on the stick),
   else ~/.os2x86. The help manager (HELPMGR): an instance programs can
   create, associate and destroy; asked to show help, it says the help
   (IPF .HLP files) can't be shown yet. The standard file dialog
   (`pm_fdlg.inc`, PMCTLS' WinFileDlg): Open and Save As, the file name,
   the directory's subdirectories and the drives, the files that match
   the filter (szFullFile's: "*.TXT", "D:\\DOCS\\*.C"), double-clicks
   and Enter; Open wants a file that's there. The font dialog
   (WinFontDlg): the families (as GpiQueryFonts has them, filtered by
   FNTS_FIXEDWIDTHONLY & co.), the sizes (the program's list or ours),
   bold, italic, underline, strikeout, a preview of the program's text;
   the family, point size, weight and an FATTRS for GpiCreateLogFont back.
   Not yet: custom dialog templates and procedures, several files at
   once, modeless dialogs. OS/2's DLLs that aren't
   here (PMSHAPI, HELPMGR, TCP32DLL, ...) load as stubs, so a program
   that only links them in runs; a call into one says so and returns 0. Calls it doesn't have yet print
   one line on stderr and return error 1; `OS2X86_TRACE=1` lists every
   call. `userland/os2x86/test/run-tests.sh` runs its test program
   (freestanding C, `os2test.c`, made an LX .EXE by `elf2lx.py`) on the
   build machine, and, when OpenWatcom is there (`WATCOM`, or its snapshot
   unpacked in `build/os2x86-test/ow`), real OpenWatcom programs from
   `test/ow`: the C runtime (files, directories, maths, signals through
   OS/2 exceptions and longjmp, _beginthread, spawnl), C++ (static
   constructors, iostreams, the STL, exceptions), conio (getch and putch
   through its 16-bit thunks), the mouse (fed terminal mouse reports), a
   DLL of its own (moved, initialised, DosLoadModule) and two PM
   programs (headless, scripted: a window, its painting checked; a menu,
   a dialog from a resource with its controls, by mouse and by keys;
   scroll bars, a combo box, accelerators, the menu by keys; an MLE and
   spin buttons; bitmaps and icons - a real OS/2 one from OpenWatcom's
   samples - their pixels checked). They pass on the system too.

   `userland/apps/os2cmd.c` relays a terminal to `/dev/os2cmd`, which runs
   each line in the shell of `kernel/src/main.c` - DIR, COPY, MOUNT, USB,
   MEM, ... on the same mounts as the serial console's; each window starts
   in its current directory and then keeps its own, like an OS/2 session.
   (RUN, RUNELF, DEBUG, BASIC, UMOUNT and the like stay on the serial
   console.) Names the OS/2 shell doesn't know run as programs, in its
   current directory (`ls -l`, `\h\cat x`, `vi`: files, `$PATH`, busybox
   applets); `START prog` opens one in a new window, `START /PM prog` a
   graphical one. `/h/OS2/os2cmd dir \h` runs one OS/2 command from a script.

   Drive letters: C: is the system volume, D:, E:, ... the volumes MOUNTed
   after it (the stick is D:; os2api DosResolvePath/DosDisplayPath, which
   CMD.EXE shares through DOSCALLS.DLL); `D:` changes drive, `\x` is the
   current drive's root. os2cmd asks /dev/os2cmd (an ioctl) for the Linux
   path of what's typed.

   Everything the desktop needs lives in `\h\OS2`, a hidden system folder
   (`make-stick.sh`, `mkhpfs.py --hidden`): DIR, TREE, DEL and wildcard
   COPY leave hidden and system entries out unless given `/A`, so the
   stick looks empty but for the user's own files; `CD \h\OS2` still
   works. The programs' RAM `/tmp` isn't in the MOUNT list.

   Keyboard layouts: `build-libs.sh` stages xkeyboard-config 2.46's data
   and libX11's UTF-8 Compose table (dead keys, Multi_key); the stick has
   them in `/h/xkb` and `/h/Compose` (`XKB_CONFIG_ROOT`,
   `XKB_DEFAULT_LAYOUT`, `XCOMPOSEFILE` in `desktop.sh`), so xkbcommon
   works as on Linux - the built-in keymap is only a fallback now.

   Next: cursor themes, more applications. The
   in-kernel compositor (`kernel/src/gui/wayland.c`, `GUI`) stays as the
   fallback console GUI.

## Building the toolchain pieces

`userland/build-musl.sh` downloads musl (pinned version and SHA-256),
builds it with the same Homebrew clang as the kernel, and installs a
sysroot to `build/userland/sysroot`, with compiler-rt's builtins.

`userland/build-libs.sh` adds the libraries above to that sysroot and
writes the cross toolchain everything else uses:

- `build/userland/toolchain/aarch64-linux-musl-cc` - clang with the
  sysroot; links static `ET_EXEC` binaries at 0x60000000, as the kernel's
  loader wants them (`CC=` for autotools, `[binaries] c` for meson)
- `build/userland/toolchain/cross.meson` - `meson setup --cross-file`
  (static libraries, `pkg_config_libdir` in the sysroot); add
  `-Dbuild.pkg_config_path=build/userland/host/lib/pkgconfig` so that
  build-machine tools (`wayland-scanner`) are found

Needs Homebrew `llvm`, `lld`, `meson`, `ninja`, `pkg-config`, `cmake`,
`bison`, `gperf`, `make` (GNU make, for busybox).

`userland/tests/` holds the test programs, built with
`userland/build-tests.sh` into `build/userland/tests/`; a test with a
` * pkg: ...` line in its header comment is linked against those
libraries. Copy them to a volume and run them from the shell with
`RUNELF \h\name` (e.g. an HPFS stick made with `scripts/mkhpfs.py
--from`, then `MOUNT usb0p1 \h`).
