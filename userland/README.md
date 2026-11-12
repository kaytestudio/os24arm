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
2. **Processes** *(done, except preemption)*.
   Per-process address spaces (page frames from a pool at
   0x64000000-0x80000000, TTBR0 switch), per-process kernel stacks, up to
   16 processes. `clone` as `fork` (address space copied, `MAP_SHARED`
   pages shared) and as `CLONE_VM|CLONE_VFORK` (musl's `posix_spawn`),
   `execve`, `exit` / zombies / `wait4`, `getpid`/`getppid`, shared
   open-file objects (fork and `dup` share offsets), close-on-exec,
   per-process `tpidr_el0`. Scheduling is cooperative (switch on block or
   `sched_yield`); waits can have deadlines. Still to do: preemption from
   the timer interrupt, threads (`CLONE_THREAD`, futex).
   Tests: `procs` (fork/wait, pipes, posix_spawn, execve, kill), `cat`
   (console input, Ctrl-D, Ctrl-C).
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
   datagram/seqpacket sockets, `/proc`.
   Tests: `ipc` (all of the above), `fsops` (directory ops on a volume).
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
5. **wlroots backend for this machine.** A wlroots backend (in the
   spirit of its `headless` backend) whose output is the kernel's
   framebuffer (virtio-gpu / ramfb, exposed to userspace as a mappable
   device and a `DRM_IOCTL_MODE_*`-free "present" ioctl), and whose input
   devices are the USB HID / virtio-input devices, delivered as
   evdev-style events. Renderer: wlroots' pixman renderer (no GPU).
6. **Compositor.** `tinywl` (wlroots' reference compositor) first, then
   a real one (e.g. `cage`, `labwc`), and native Wayland clients built
   against the same sysroot. The in-kernel compositor
   (`kernel/src/gui/wayland.c`) stays as the fallback console GUI.

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
`bison`.

`userland/tests/` holds the test programs, built with
`userland/build-tests.sh` into `build/userland/tests/`; a test with a
` * pkg: ...` line in its header comment is linked against those
libraries. Copy them to a volume and run them from the shell with
`RUNELF \h\name` (e.g. an HPFS stick made with `scripts/mkhpfs.py
--from`, then `MOUNT usb0p1 \h`).
