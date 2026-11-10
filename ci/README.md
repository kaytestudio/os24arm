# CI

Builds the os24arm kernel images and the bootable ISO, on both GitHub
Actions (`.github/workflows/build.yml`) and Jenkins (`Jenkinsfile` at the
repo root). Both drive the same scripts here so there's one place that
knows how to build the project:

- `scripts/install-deps-linux.sh` — installs the pinned clang/lld/llvm,
  cmake, and xorriso on a Debian/Ubuntu runner.
- `scripts/build-qemu.sh [build-dir]` — configures and builds the
  QEMU-target kernel image (`os2warp.img`) and the bootable ISO
  (`os2warp-arm64.iso`, via CMake's `iso` target and `scripts/build_iso.sh`
  at the repo root) that can be written straight to a USB stick with `dd`.
- `scripts/build-rpi.sh [build-dir]` — configures and builds the
  Raspberry Pi 3+ kernel image (`kernel8.img`).
- `scripts/build-beagle.sh <BBB|BEAGLEV_AHEAD|QEMU_RISCV> [build-dir]` —
  configures and builds the BeagleBone Black (`os2warp-bbb.img`) or
  BeagleV-Ahead (`os2warp-beaglev-ahead.img`) kernel image, plus the
  `uEnv.txt` that boots it from U-Boot, or the same RV64 kernel for QEMU's
  riscv64 virt machine (`os2warp-qemu-riscv.img`).

Both build scripts accept a `CC` environment variable (default: `clang`)
naming the clang binary to build with — set it to a versioned package name
(e.g. `clang-17`, matching what `install-deps-linux.sh` installs) on a
Debian/Ubuntu runner that has no unversioned `clang` on `PATH`.

To build locally the same way CI does:

```sh
ci/scripts/build-qemu.sh build
ci/scripts/build-rpi.sh build-rpi
ci/scripts/build-beagle.sh BBB build-bbb
ci/scripts/build-beagle.sh BEAGLEV_AHEAD build-beaglev-ahead
ci/scripts/build-beagle.sh QEMU_RISCV build-qemu-riscv
```

See the repo root `README.md` for the full prerequisites (macOS vs. Linux)
these scripts assume are already installed when not running
`install-deps-linux.sh` first.
