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
   Sans Mono, and busybox as its shell. `userland/make-stick.sh` puts it
   all on an HPFS stick image with `userland/rootfs/` (`env.sh`,
   `desktop.sh`, `dwl.sh`, `fonts.conf`, `foot.ini`, `fuzzel.ini`, and
   `applications/*.desktop` - what the launcher offers).

   Try it: `scripts/run-vm.sh --wayland --hvf` (`--hvf`: native speed
   with Apple's Hypervisor.framework; without it QEMU emulates (TCG),
   which the kernel's own hypervisor needs, and the desktop is slow),
   then `DESKTOP [layout [variant]]` (e.g. `pt`, `de nodeadkeys`; default
   `us`): it mounts the stick at `\h` and starts dwl (`dwl.sh`). The user
   sees an OS/2 system: every terminal is an OS/2 Command Prompt (foot's
   `shell=` is `/h/os2cmd`; the first window, Alt+Shift+Enter and Alt+O
   open one), Alt+P lists OS/2 Command Prompt, System Monitor and Text
   Editor, and the compositor's and terminals' own output goes to
   `/tmp/desktop.log` rather than the console. Alt+J/K moves the focus,
   Alt+Shift+C closes a window, Alt+Shift+Q goes back to the console.
   (`RUNELF \h\busybox sh /h/desktop.sh` instead: one prompt in cage.)

   `userland/apps/os2cmd.c` relays a terminal to `/dev/os2cmd`, which runs
   each line in the shell of `kernel/src/main.c` - DIR, COPY, MOUNT, USB,
   MEM, ... on the same mounts as the serial console's; each window starts
   in its current directory and then keeps its own, like an OS/2 session.
   (RUN, RUNELF, DEBUG, BASIC, UMOUNT and the like stay on the serial
   console.) Names the OS/2 shell doesn't know run as programs, in its
   current directory (`ls -l`, `\h\cat x`, `vi`: files, `$PATH`, busybox
   applets); `START prog` opens one in a new window, `START /PM prog` a
   graphical one. `/h/os2cmd dir \h` runs one OS/2 command from a script.

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
