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
2. **Processes** *(done, except preemption and signal handlers)*.
   Per-process address spaces (page frames from a pool at
   0x64000000-0x80000000, TTBR0 switch), per-process kernel stacks, up to
   16 processes. `clone` as `fork` (address space copied) and as
   `CLONE_VM|CLONE_VFORK` (musl's `posix_spawn`), `execve`, `exit` /
   zombies / `wait4`, `kill` (default actions), `getpid`/`getppid`,
   shared open-file objects (fork and `dup` share offsets), `pipe2`,
   `dup`/`dup3`/`fcntl`, close-on-exec, per-process `tpidr_el0`, Ctrl-C at
   the console -> SIGINT. Scheduling is cooperative (switch on block or
   `sched_yield`). Still to do: preemption from the timer interrupt,
   `rt_sigaction` handlers (SIGCHLD/SIGPIPE), threads (`CLONE_THREAD`,
   futex). Tests: `procs` (fork/wait, pipes, posix_spawn, execve, kill),
   `cat` (console input, Ctrl-D, Ctrl-C).
3. **Event and IPC primitives.** `poll`/`ppoll`, `epoll`, `eventfd`,
   `timerfd`, `signalfd`, `pipe2`; `AF_UNIX` stream sockets with
   `SCM_RIGHTS`; `memfd_create` + `MAP_SHARED` mappings shared between
   processes; `/dev/null`, `/dev/urandom`, `/proc/self` basics.
4. **Libraries.** Cross-build libffi, libwayland (server + client +
   `wayland-scanner` host tool), pixman, libxkbcommon (with a compiled-in
   keymap; no X11 data files at runtime), against the musl sysroot.
   Test: `wayland-server` + a `wl_shm` client exchanging buffers.
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
sysroot to `build/userland/sysroot`. `userland/tests/` holds the test
programs, built with `userland/build-tests.sh`.
