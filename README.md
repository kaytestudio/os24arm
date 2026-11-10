# OS/2 Warp 4.52 for ARM64 (QEMU / Raspberry Pi 3+ / BeagleBone Black / BeagleV-Ahead / QEMU RISC-V)

A **bare-metal**, freestanding OS/2-style operating system written in C11 +
AArch64 assembly, targeting the **QEMU virt** machine (Cortex-A72) and
**Raspberry Pi 3/3+** hardware. No libc, no MMU-backed process isolation —
everything (kernel, `CMD.EXE`, filesystem drivers) runs in one flat address
space.

It also boots to a serial-console command prompt on two BeagleBoard.org
boards that aren't AArch64: the **BeagleBone Black** (TI AM335x, 32-bit
ARMv7 Cortex-A8) and the **BeagleV-Ahead** (T-Head TH1520, RV64 XuanTie
C910). Those ports are early and serial-console only. The MMU, GUI and LX
loader are still AArch64-only (see §4b). The RISC-V kernel also runs in
QEMU's riscv64 `virt` machine (`TARGET_PLATFORM=QEMU_RISCV`, see §3b).

It has a PL011 UART driver, a DOS-style command shell with DOSKEY-style
history/macros, a QuickBASIC-compatible interpreter, an ext4 + btrfs
filesystem stack (both read/write) behind a small VFS/IFS layer — plus a
standalone f2fs driver not yet wired into boot — an OS/2 LX `.EXE` loader
that can run a standalone `CMD.EXE`, a Wayland-style GUI compositor with
four switchable "workplaces", a DEBUG.COM-style memory monitor, and a small
on-box heuristic assistant (`AI`) — no network stack, so it's not a real
LLM, just something that watches what you type.

```
┌──────────────────────────────────────────────────────────────────────┐
│                       OS/2 Warp 4.52 ARM64                           │
│                                                                        │
│  bootloader/                     kernel/src/                         │
│  ├── boot.S      (QEMU entry)    ├── main.c        (kernel_main,     │
│  ├── rpi_boot.S  (RPi3 entry)    │                   shell loop)     │
│  └── linker scripts              ├── uart.c         (PL011)          │
│                                   ├── kprintf.c      (kernel printf)  │
│  os2api/                         ├── vfs.c          (VFS layer)      │
│  └── os2_api.c   Dos* calls      ├── ifs_loader.c   (IFS registry)   │
│      (DosOpen/DosRead/...)       ├── blkdev.c/gpt.c (block devices)  │
│                                   ├── lx_loader.c    (OS/2 LX loader) │
│  dos/                            ├── ai.c           (AI assistant)   │
│  ├── dos_commands.c  DIR/TYPE/   ├── basic.c        (QuickBASIC)     │
│  │   COPY/COMP/FC/DEL/REN/MD/    ├── unixroot.c     (EMX dir tree,   │
│  │   RD/CD/VOL/ATTRIB/ECHO/          SET UNIXROOT= in CONFIG.SYS)    │
│  │   CLS/DOSKEY/DEBUG/EXIT       └── gui/            Wayland-style   │
│  └── cmd_main.c  standalone          compositor, 4 workplaces,       │
│      CMD.EXE entry point             virtio-gpu/ramfb/virtio-input   │
│                                                                        │
│                                   fs/                                 │
│                                   ├── ext4/    read/write driver     │
│                                   ├── btrfs/   read/write CoW driver │
│                                   └── f2fs/    read/write driver     │
│                                       (standalone, not boot-wired)   │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 1. Prerequisites

### macOS (for building)

```bash
brew install llvm qemu cmake
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"

# Verify
clang --version            # must be LLVM clang, not Apple clang
llvm-objcopy --version
qemu-system-aarch64 --version
```

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install clang lld llvm qemu-system-arm cmake
```

---

## 2. Build

### QEMU (default)

```bash
git clone <this repo>
cd os24arm

cmake -B build
cmake --build build --target os2warp.img

# Outputs:
#   build/os2warp_kernel  — ELF with debug symbols
#   build/os2warp.img     — raw binary QEMU boots directly
#   build/CMD.EXE         — standalone OS/2 LX executable (see §6)
```

`cmake --build build` alone (no `--target`) builds everything, including the
host-side LX toolchain (`lxbuild`/`makeexe`/`makedll`, built with your host
compiler since they run on the build machine, not the target) and `kcc`, the
Kayte C Compiler host tool used by the LX build pipeline. If you only want
the kernel image, target `os2warp.img` specifically as above — it's faster.

### Raspberry Pi 3+

```bash
cmake -B build-rpi -DTARGET_PLATFORM=RPI3
cmake --build build-rpi

# Outputs:
#   build-rpi/os2warp_kernel  — ELF with debug symbols
#   build-rpi/kernel8.img     — raw binary for the Pi's firmware to load
```

The RPi3 path shares the same kernel sources as QEMU where possible (see
`kernel/CMakeLists.txt`); `bootloader/rpi_boot.S` and `bootloader/rpi_main.c`
handle the Pi-specific entry sequence (dropping from EL2/EL3 to EL1, parking
secondary cores, etc.) before handing off. It hasn't been exercised as
heavily this session as the QEMU path — treat it as less battle-tested.

### BeagleBone Black / BeagleV-Ahead

```bash
cmake -B build-bbb -DTARGET_PLATFORM=BBB \
      -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
      -DCMAKE_ASM_COMPILER=$(brew --prefix llvm)/bin/clang
cmake --build build-bbb

cmake -B build-beaglev-ahead -DTARGET_PLATFORM=BEAGLEV_AHEAD \
      -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
      -DCMAKE_ASM_COMPILER=$(brew --prefix llvm)/bin/clang
cmake --build build-beaglev-ahead

# Outputs:
#   build-bbb/os2warp-bbb.img                      — zImage-format image for U-Boot bootz
#   build-beaglev-ahead/os2warp-beaglev-ahead.img  — RISC-V Image for U-Boot booti
#   build-*/uEnv.txt                               — U-Boot script that loads and boots it
```

(or `ci/scripts/build-beagle.sh BBB` / `ci/scripts/build-beagle.sh BEAGLEV_AHEAD`.)
The same clang builds all five platforms; `TARGET_PLATFORM` selects the
target triple (`aarch64-none-elf`, `armv7a-none-eabi` or
`riscv64-unknown-elf`). These two platforms build a smaller kernel:
`kernel/src/bbb_boot.S` or `kernel/src/beaglev_boot.S`, a 16550 UART
driver (`kernel/src/ns16550.c`), and `kernel/src/beagle_main.c` (heap,
scheduler stub and a VER/HELP/MEM/ECHO/CLS/EXIT prompt). Both also link
the OS/2 API (`os2api/`, on top of `kernel/src/vfs.c`) and add CD, DIR and
TYPE commands that go through it. Both also include a microSD driver
(`kernel/src/sdhci.c`), MBR/GPT partition probing (`kernel/src/mbr.c`,
`kernel/src/gpt.c`) and the FAT16/FAT32 and ext4 drivers
(`kernel/src/fat32.c`, `fs/ext4/`). On the BeagleBone Black, `kernel/src/arm_aeabi.c` supplies
the `__aeabi_*` memory and division helpers clang emits on ARMv7 (the
Cortex-A8 has no divide instruction). The DOS
commands, REXX, the filesystems and the CMD.EXE/DOSCALLS.DLL/LX pipeline
are not built for either board.

The toolchain is configured directly in `CMakeLists.txt` (no separate
toolchain file needed):

```cmake
-target aarch64-none-elf
-mcpu=cortex-a72
-ffreestanding -nostdlib
-fno-builtin -fno-stack-protector -fno-pie -fno-pic
```

---

## 3. Run in QEMU

There's no real VirtIO block driver yet (`blkdev.c`'s `virtio_blk_read/write`
are unused stubs) — disk I/O only works via a fixed-physical-address
"preloaded disk" window (`PRELOADED_DISK_ADDR = 0x44000000`, 24 MB) that
QEMU's `-device loader` injects a raw disk image into *before* boot. That's
what `run-testdisk` (below) sets up for you.

```bash
# Boot with just the kernel — no disk, serial console only
cmake --build build --target run

# Boot with a real display: type GUI at the prompt for the Wayland-style
# compositor; Cmd+Escape cycles the 4 workplaces; mouse is live
cmake --build build --target run-gui

# Build a small ext4 test disk (CMD.EXE, HELLO.EXE, README.TXT) and boot
# with it preloaded — makes DIR/TYPE/COPY/RUN actually have something to
# work with
cmake --build build --target run-testdisk

# GDB/LLDB debugging (QEMU pauses, waits for a debugger on :1234)
cmake --build build --target debug
```

Press **Ctrl-A X** to quit QEMU (nographic mode).

For a btrfs test disk (see §7 for all three filesystem drivers), build one with
`scripts/mkbtrfs_image.py` and inject it the same way `run-testdisk` does,
via `-device loader,file=...,addr=0x44000000,force-raw=on`; GPT-partition it
with a partition literally named `C` (`sgdisk --change-name=1:C`) so
`btrfs_init()` finds it (see `fs/btrfs/btrfs.c`).

---

## 3b. Run the RISC-V kernel in QEMU

`TARGET_PLATFORM=QEMU_RISCV` builds the BeagleV-Ahead kernel for QEMU's
riscv64 `virt` machine. It has the same boot code, shell, OS/2 API, SD
driver and filesystems. Only the device addresses and the link address
(`0x80200000`, DRAM base + 2 MB) differ. OpenSBI (`-bios default`) starts
it in S-mode, just as it does on the board.

```bash
# needs qemu-system-riscv64 (brew install qemu / apt install qemu-system-misc)
# and, for the ext4 partition on the test card, e2fsprogs (brew install e2fsprogs)
cmake -B build-qemu-riscv -DTARGET_PLATFORM=QEMU_RISCV \
      -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
      -DCMAKE_ASM_COMPILER=$(brew --prefix llvm)/bin/clang
cmake --build build-qemu-riscv --target run      # Ctrl-A X quits QEMU
```

| Target | What it does |
|---|---|
| `run` | Boots with the test SD card (4 harts, 512 MB) |
| `run-nocard` | Boots with an empty SD slot |
| `debug` | Same as `run`, paused at reset and waiting for GDB/LLDB on `:1234` |
| `mksdcard` | Builds `build-qemu-riscv/sdcard.img` if it's missing |

The test card (`scripts/mksdcard.py`) is a 128 MB MBR image:

- An active FAT32 partition, which becomes C:. It holds the repo's
  `CONFIG.SYS`, a `README.TXT` and `DOCS\NOTE.TXT`.
- An ext4 partition at `\mmc0p2`, added only when `mke2fs` is found.

The image is built only when missing, so anything the kernel writes to it
survives between runs. Delete it to reset.

QEMU's `virt` machine only has PCI SD controllers, so `run` adds a
`-device sdhci-pci`. Nothing else sets up PCI, so `kernel/src/qemu_pci.c`
finds the controller on bus 0 through the ECAM window, maps its BAR0 at
`0x40000000` and enables it. `sdhci.c` then drives it like the boards'
memory-mapped controllers.

---

## 4. Boot on Raspberry Pi 3+

### Hardware needed

- Raspberry Pi 3, 3+, or 3B+ (ARM64, Cortex-A53)
- MicroSD card (8 GB+, FAT32)
- USB-to-TTL serial cable for console output
- 5V power supply (2.5A minimum)

### SD card

```bash
# Format as FAT32, then copy the kernel image
diskutil eraseDisk FAT32 BOOT /dev/diskX   # macOS
cp build-rpi/kernel8.img /Volumes/BOOT/
diskutil eject /dev/diskX
```

You'll also need the Pi's own firmware files (`bootcode.bin`, `start.elf`,
`fixup.dat`) and a `config.txt` on the card — see `bootloader/config.txt` for
a starting point.

### Serial console

| Cable wire  | RPi pin | Function |
|-------------|---------|----------|
| Black (GND) | Pin 6   | Ground   |
| Green (RX)  | Pin 8   | UART TX  |
| White (TX)  | Pin 10  | UART RX  |
| Red (VCC)   | **do not connect** | the Pi powers itself |

```bash
screen /dev/tty.usbserial-* 115200   # macOS
screen /dev/ttyUSB0 115200           # Linux
```

---

## 4b. Boot on BeagleBone Black / BeagleV-Ahead

Both boards boot through U-Boot from a microSD card whose first partition
is FAT. Each image carries a Linux-style header (zImage for the BBB,
RISC-V `Image` for the BeagleV-Ahead). U-Boot's `bootz`/`booti` therefore
hands over with the caches cleaned and the MMU off, just as it would for
Linux. The images aren't position-independent, so they must be loaded at
the address the generated `uEnv.txt` uses.

| | BeagleBone Black | BeagleV-Ahead |
|---|---|---|
| Load address | `0x82000000` | `0x00200000` (DRAM base + `text_offset`) |
| U-Boot command | `bootz` | `booti` |
| Card device | `mmc 0:1` | `mmc 1:1` (eMMC is `mmc 0`) |
| Console | UART0, J1 header: pin 1 GND, pin 4 RX, pin 5 TX | UART0 debug header (GND/TX/RX) |
| microSD controller | MMCHS0 @ `0x48060000` (96 MHz) | SDIO0, DWC MSHC @ `0xFFE7090000` (198 MHz) |

Both consoles are 3.3 V TTL at 115200 8N1. The kernel reuses U-Boot's baud
rate setting and doesn't reprogram the UART clock.

### BeagleBone Black

```bash
# FAT partition 1 of the microSD card needs BeagleBoard.org's MLO and
# u-boot.img, plus:
cp build-bbb/os2warp-bbb.img build-bbb/uEnv.txt /Volumes/BOOT/
```

Hold **S2 (BOOT)** while powering on so the board boots from microSD
instead of the U-Boot on its eMMC. U-Boot imports `uEnv.txt` and runs its
`uenvcmd`. To boot by hand from the U-Boot prompt:

```
load mmc 0:1 0x82000000 os2warp-bbb.img
bootz 0x82000000 - ${fdtcontroladdr}
```

### BeagleV-Ahead

```bash
cp build-beaglev-ahead/os2warp-beaglev-ahead.img build-beaglev-ahead/uEnv.txt /Volumes/BOOT/
```

The BeagleV-Ahead's U-Boot lives in eMMC and may not import `uEnv.txt` from
the microSD card. If it doesn't, stop autoboot on the serial console and
boot by hand:

```
load mmc 1:1 0x200000 os2warp-beaglev-ahead.img
booti 0x200000 - ${fdtcontroladdr}
```

If U-Boot starts every hart at the entry point, the first one through the
boot lottery in `beaglev_boot.S` runs the kernel and the rest park.

### What works so far

Boot banner, heap and the serial command prompt (VER, HELP, MEM, ECHO, CLS,
EXIT). On both boards, the OS/2 API (`DosOpen`, `DosFindFirst`,
`DosSetCurrentDir`, `KernVMAlloc`, ...) is linked into the kernel, and
CD, DIR and TYPE go through it.

At boot the microSD card is identified and registered as block device
`mmc0`, and its partition table is read: MBR first, then GPT. Each
partition holding FAT (FAT16 or FAT32) or ext4 is mounted. The boot
partition becomes `/` (drive C:): that's the MBR "active" FAT partition,
or else the first FAT partition, wherever it sits on the card. Every other
volume goes at `/<partition>`:

```
[SD]     mmc0: SDHC/SDXC card, RCA 0x..., 15193 MB
[MBR]    Found 2 partition(s) on mmc0
[SD]     mmc0p1 mounted at / (C:)
[SD]     mmc0p2 mounted at /mmc0p2

[C:\]> dir
[C:\]> type uenv.txt
[C:\]> dir \mmc0p2
```

On a typical card, the FAT boot partition (with the kernel image and
`uEnv.txt`) is C: and an ext4 root filesystem is `\mmc0p2`. Notes:

- The boot partition can be **FAT16 or FAT32**. `kernel/src/fat32.c` picks
  the variant from the cluster count, as the FAT spec requires, and doesn't
  trust the `FAT16   `/`FAT32   ` label. FAT12 (volumes under about 2 MB
  with 512-byte clusters) is rejected. On FAT16, the high word of a
  directory entry's cluster field holds OS/2's extended-attribute handle,
  so the driver ignores it on read and preserves it on write.
- ext4 is case-sensitive: `TYPE \mmc0p2\HELLO.TXT` works, `hello.txt`
  doesn't.
- At most one FAT and one ext4 volume are mounted, because each driver
  keeps a single volume's state.
- The ext4 driver mounts read-only. The FAT driver can write on FAT16 and
  FAT32: create, write, mkdir, delete and rename. No shell command writes
  yet. The FAT16 root directory is a fixed size (usually 512 entries) and
  can't grow.

The SD driver (`kernel/src/sdhci.c`) targets the SD Host Controller
register layout both SoCs share. It uses PIO with no interrupts or DMA,
identifies the card at 400 kHz, then runs a 4-bit bus at about 25 MHz. It
supports SDSC, SDHC and SDXC cards. U-Boot has already set up pinmux,
clocks and card power by the time the kernel starts. The AM335x MMCHS gets
a full module reset and its TRM-required init stream. On the TH1520, only
the command and data state machines are reset, so U-Boot's vendor PHY and
I/O-voltage settings are kept.

Not yet ported: MMU, interrupts/timer, the eMMC, the GUI, and the LX
loader. LX images contain AArch64 code, so CMD.EXE and DOSCALLS.DLL can't
run on these CPUs anyway.

Neither SoC is emulated by QEMU, and these images have not yet been run on
real boards. The RISC-V kernel can be run in QEMU with `QEMU_RISCV` (§3b).
The same boot code and drivers have also been smoke-tested in QEMU with only
the device addresses swapped. For the ARMv7 path that was
`ast2600-evb` (Cortex-A7) with its standard SDHCI. For the RV64 path it was
`riscv64 virt` via OpenSBI with an `sdhci-pci` controller. The tests used
a 128 MB SDSC card and a 4 GB SDHC card (block read/write and read-back),
each with an MBR, FAT32 and ext4. They also used a FAT16 card with ext4 as
partition 1 and FAT16 as partition 2 (active), which still became C:. Files
and directories written to the FAT16 and FAT32 volumes were checked on the
host with `fsck_msdos` and a separate FAT reader. The TI-specific MMCHS code (module
reset, init stream, clock divider, uncropped R2 responses) and the TH1520
controller have not been exercised. Exceptions/traps print their cause, PC and fault address and halt.
Not yet ported: MMU, interrupts/timer, SD/eMMC block driver (so no
filesystems), GUI, and the LX loader. LX images contain AArch64 code, so
CMD.EXE and DOSCALLS.DLL can't run on these CPUs anyway.

Neither SoC is emulated by QEMU, and these images have not yet been run on
real boards. The same boot code and 16550 driver have been smoke-tested in
QEMU with only the UART base address swapped: `riscv64 virt` via OpenSBI
for the RV64 path, and `ast2600-evb` (Cortex-A7) for the ARMv7 path.

---

## 5. Debug with GDB

```bash
# Terminal 1 — start QEMU paused, waiting on :1234
cmake --build build --target debug

# Terminal 2 — attach
gdb build/os2warp_kernel \
    -ex "target remote :1234" \
    -ex "break kernel_main" \
    -ex "continue"
```

(`aarch64-elf-gdb` if you have a cross-gdb installed; a host `gdb` built with
AArch64 support works too.)

---

## 6. The shell

Boots straight into a command prompt:

```
[C:\]>
```

Type `HELP` for the command list. Type `DOSKEY` once to turn on command
history and macros — until you do, the prompt is plain byte-at-a-time input,
matching how real DOSKEY only changes anything once loaded.

### DOSKEY

Based on real MS-DOS DOSKEY: Up/Down arrows recall history, `F7` lists it,
`F8` prefix-searches it, `F9` selects by number; Left/Right/Home/End/Delete/
Insert edit the line, `Esc` clears it. `$`-macros: `name=text`, with `$1`-`$9`
positional args, `$*` for all args, `$T` to chain multiple commands in one
macro, `$$` for a literal `$`. `DOSKEY /HISTORY`, `/MACROS`, `/LISTSIZE=n`,
`/REINSTALL`, `/MACROFILE=file` all work as in real DOSKEY.

### DEBUG

A partial DEBUG.COM-style memory monitor — its own `-` sub-prompt (`Q` to
exit back to the DOS prompt). `D`(ump)/`E`(nter)/`F`(ill)/`C`(ompare)/
`M`(ove)/`S`(earch) operate on real flat 64-bit addresses, parsed the DEBUG.COM
way (bare hex, `start end` or `start Llength` ranges). `H` does hex
arithmetic; `R` shows real AArch64 state (SP, exception level, DAIF) rather
than faking x86 registers. `?` lists every command DEBUG.COM had — the ones
that don't map to this platform (port I/O, EMS, an x86 assembler/
disassembler, breakpoint/trace) are recognized but say so rather than
pretending.

### AI

An on-box heuristic assistant — **not** a real LLM (there's no network
stack to reach one). `AI STATUS` gives a session summary and a suggestion,
`AI STATS` shows command-usage counts, `AI TIP` suggests a built-in command
you haven't tried yet. State is RAM-only and resets on reboot.

### Commands

| Command | Description |
|---|---|
| `VER` | Show OS/kernel version |
| `HELP` / `?` | List commands |
| `DIR [path]` | List directory contents |
| `TYPE <file>` | Print file contents |
| `COPY <src> <dst>` | Copy a file |
| `COMP <file1> <file2>` | Byte-for-byte compare, reports first differing offset |
| `FC <file1> <file2>` | Line-by-line compare, reports differing lines |
| `DEL <file>` / `ERASE` | Delete a file |
| `REN <old> <new>` / `RENAME` | Rename a file |
| `MD <dir>` / `MKDIR` | Create a directory |
| `RD <dir>` / `RMDIR` | Remove a directory |
| `CD [path]` / `CHDIR` | Show/change current directory |
| `VOL` | Show volume label |
| `ATTRIB <file>` | Show file attributes |
| `CHKDSK`, `FORMAT` | Stubbed — not supported |
| `ECHO <text>` | Print text |
| `CLS` | Clear screen |
| `DOSKEY` | Enable history/macros (see above) |
| `DEBUG` | Memory monitor sub-shell (see above) |
| `MEM` | Show heap usage |
| `IFS` | List loaded filesystem drivers |
| `RUN <file.EXE>` | Load and run an OS/2 LX executable (e.g. `RUN CMD.EXE`) |
| `BASIC` | Start the QuickBASIC interpreter |
| `AI [STATUS\|STATS\|TIP\|HELP]` | On-box assistant (see above) |
| `GUI` | Start the Wayland-style Workplace Shell desktop |
| `EXIT` | Halt the CPU (`wfi` loop) — see §8 for why there's nowhere to "return" to |

---

## 7. Filesystems

```
dos/dos_commands.c   (DIR, TYPE, COPY, DEL, REN, MD, RD, CD, ...)
        │
os2api/os2_api.c     (DosOpen/DosRead/DosWrite/DosFindFirst/... - the only
        │              way commands talk to storage, matching real DOSCALLS.DLL)
kernel/src/vfs.c      (mount table, fd table, path dispatch)
        │
kernel/src/ifs_loader.c  (installable-filesystem registry)
        │
   ┌────┴──────────┐
fs/ext4/   fs/btrfs/   fs/f2fs/  (standalone - not IFS-registered/boot-mounted)
        │
kernel/src/blkdev.c + gpt.c   (block device registry, GPT partitions)
```

All three drivers follow the same shape: a read-only `<fs>.c` (mount,
path resolution, read, directory listing) plus a separate `<fs>_write.c`
write engine, gated by a `writable` flag decided once at mount time — an
image whose on-disk layout falls outside what the write engine knows how
to maintain safely just stays mounted read-only rather than risking
corruption. None of the three maintain their real filesystem's full
feature set (see each driver's own header comment for the exact cut list)
— the goal throughout has been "genuinely correct within a documented,
reduced scope," not a byte-for-byte reimplementation of any of them.

- **ext4** — real per-group bitmap writes: create/write/truncate/delete/
  mkdir/rename, using inline (depth-0) extent trees and directory-entry
  slot splitting. Refuses to write to images with the `uninit_bg`/
  `metadata_csum` features (would need CRC16/CRC32C checksums over every
  group descriptor, bitmap, and inode on every mutation — out of scope,
  falls back read-only instead). `scripts/mktestdisk.sh`'s BOOT partition
  is built with those features disabled so this driver can write to it;
  `scripts/test_ext4_driver.sh` cross-checks every write against a real
  `mke2fs`-built image using real `e2fsck`/`debugfs` (this caught two real
  extent-tree bugs during development) — see `fs/ext4/ext4_write.c`'s
  header comment for the full scope.
- **btrfs** — real copy-on-write writes: create/write/truncate/delete/mkdir/
  rename, with node/leaf splitting, CRC32C-checksummed nodes, and generation
  bumping, landing in a superblock rewrite as the atomic commit point. It
  does **not** maintain the extent tree or checksum tree (a much larger
  undertaking with no way to validate here — see `fs/btrfs/btrfs_write.c`'s
  header comment), and its leaf layout is a simplified, project-local
  dialect (items packed forward, not upstream btrfs's backward-growing
  layout). Images this driver writes to are safe for this OS to read/write
  across reboots, but shouldn't be handed to a real Linux `btrfs` or
  `btrfs-progs` afterward. `scripts/mkbtrfs_image.py` builds test images;
  `scripts/check_btrfs_image.py` is an independent structural verifier
  (checksums, key ordering, generation monotonicity) since there's no
  `btrfsck` available on macOS to check against.
- **f2fs** — a log-structured (LFS-only, no GC/SSR) driver: checkpoint
  versioning with dual packs, Segment Information Table + Node Address
  Table, node/dnode traversal, create/write/truncate/delete/mkdir/rename,
  one full checkpoint commit per operation as the atomic finish line. On-
  disk structs were transcribed byte-for-byte from the real Linux kernel
  header rather than from memory, and its dentry hash and checkpoint-pack-
  selection algorithm were ported from the real driver's own source before
  it was removed from this tree. **Not wired into kernel boot** (`main.c`
  only mounts ext4/btrfs) and, unlike the other two, **not checked against
  any real tooling** — `mkfs.f2fs`/`fsck.f2fs` aren't available on macOS
  and don't install straightforwardly here, so `scripts/test_f2fs_driver.sh`
  can only self-check (format → mount → exercise every op → unmount →
  remount → re-verify everything survived). See `fs/f2fs/f2fs_write.c`'s
  header comment for the full scope and this caveat in more detail before
  relying on it for anything beyond experimentation.

### UNIXROOT (EMX-style directory tree)

At boot, once root is mounted, `kernel/src/unixroot.c` reads `CONFIG.SYS`
for a `SET UNIXROOT=` line (defaulting to `C:`) and lays down the classic
EMX/Unix directory skeleton — `/bin`, `/emx`, `/etc`, `/home/<user>`,
`/usr/{bin,doc,etc,include,info,lib}`, `/usr/share/{locale,man/man1-9,
man/cat1-9,mann,catn}`, `/tmp`, `/var` — via `vfs_mkdir()`. Whether this
actually succeeds depends on which filesystem ended up mounted at `/`
(ext4 and btrfs are both writable now; see above).

---

## 8. CMD.EXE and the LX loader

`RUN <file.EXE>` (`kernel/src/lx_loader.c`) loads and executes a real OS/2 LX
(Linear eXecutable) binary in the kernel's own flat address space — there's
no process isolation or syscall boundary in this project, so this is the
actual command-interpreter logic, not a demo.

`dos/cmd_main.c` builds a standalone `CMD.EXE` from the exact same
`dos_commands.c`/`dos_shell_dispatch()` command set the in-kernel shell uses,
wrapped as a genuine LX executable by the host-side tools in `lx/`
(`lxbuild`, `makeexe`). `RUN CMD.EXE` loads and jumps into it directly. Since
there's no process boundary to return across, `CMD.EXE` is treated like the
in-kernel shell: `EXIT` halts the CPU rather than returning control to
whatever called `RUN`.

---

## 9. The GUI

`GUI` starts a small Wayland-style compositor (`kernel/src/gui/wayland.c`)
with four switchable "workplaces" (virtual desktops) — **Cmd+Escape** in the
display window cycles between them. Backed by `virtio-gpu`/`ramfb` for the
framebuffer and `virtio-input` for keyboard/pointer events (see `run-gui` in
§3 for the QEMU flags that wire this up). This is separate from the serial
console the DOS shell runs on.

---

## 10. QuickBASIC interpreter

Type `BASIC` at the shell. Type `BYE`, `QUIT`, or `SYSTEM` to return.

```basic
10 PRINT "Hello from OS/2 BASIC!"
20 FOR I = 1 TO 10
30   PRINT I * I
40 NEXT I
50 END
RUN
```

### Statements

| Statement | Notes |
|---|---|
| `PRINT expr / "str" [; ,]` | `TAB()`, `SPC()` supported |
| `LET var = expr` | `LET` is optional |
| `INPUT ["prompt";] var` | String or integer |
| `IF expr THEN … [ELSE …]` | Single-line form |
| `IF … / ELSEIF … / ELSE / END IF` | Block form |
| `SELECT CASE … / CASE … / END SELECT` | `CASE IS`, `CASE a TO b` |
| `FOR var = n TO m [STEP s] … NEXT [var]` | |
| `WHILE expr … WEND` | |
| `DO [WHILE\|UNTIL] … LOOP [WHILE\|UNTIL]` | |
| `GOTO line` | |
| `GOSUB line … RETURN` | |
| `ON expr GOTO line[,…]` | |
| `ON expr GOSUB line[,…]` | |
| `READ var / DATA val[,…] / RESTORE` | |
| `DIM var(n)` | 1-D integer arrays, 0-based |
| `CONST name = expr` | |
| `SWAP var, var` | |
| `CLS / LOCATE r,c / COLOR fg[,bg] / BEEP` | ANSI terminal |
| `SLEEP n` | Busy-wait seconds |
| `REM` or `'` | Comment |
| `END / STOP` | |
| `LIST [first[-last]] / RUN / NEW / RENUM` | Direct-mode commands |

### Functions

`ABS` `SGN` `INT` `FIX` `SQR` `RND` `VAL` `LEN` `ASC` `INSTR` `CHR$` `STR$`
`HEX$` `OCT$` `LEFT$` `RIGHT$` `MID$` `LTRIM$` `RTRIM$` `UCASE$` `LCASE$`
`SPACE$` `STRING$` `INKEY$`

### Variables

| Type | Example |
|---|---|
| Integer (`A`–`Z`) | `A = 42` |
| String (`A$`–`Z$`) | `A$ = "hi"` |
| Array (`A(n)`) | `DIM A(10)` |
| Hex literal | `&HFF` |
| Octal literal | `&O17` |

Multi-statement lines separated by `:`. `?` is shorthand for `PRINT`.

---

## 11. Other subprojects in this tree

Not all part of the default build (`add_subdirectory(...)` in the root
`CMakeLists.txt`) — noted here so their status is clear:

- **`rexx/`** — an ooRexx-flavored interpreter. Built and linked into the
  kernel, but not yet wired up to a shell command.
- **`lx/`** — the host-side OS/2 LX executable builder (`lxbuild`, `makeexe`,
  `makedll`) used to produce `CMD.EXE` and other `.EXE`/`.DLL` files; built
  automatically as part of the LX pipeline (§8).
- **`tools/kcc`** — the Kayte C Compiler, an alternative to clang for the
  cross-build (`cmake -DUSE_KCC=ON`), and a host tool in the LX pipeline.
- **`kayte/`** — a separate Free Pascal/Lazarus project (the Kayte language
  toolchain) living in this repo; not part of the CMake build.
- **`workplace/`** — a Qt6-based C++ Workplace Shell desktop/QPA platform
  plugin; not currently wired into `add_subdirectory(...)`, distinct from
  the actual GUI in §9 (`kernel/src/gui/`).
- **`installer/`** — a C++ installer wizard, separate from the kernel build.

---

## 12. Architecture reference

### Platform comparison

| Feature | QEMU virt | Raspberry Pi 3+ | BeagleBone Black | BeagleV-Ahead | QEMU RISC-V virt |
|---|---|---|---|---|---|
| `TARGET_PLATFORM` | `QEMU` | `RPI3` | `BBB` | `BEAGLEV_AHEAD` | `QEMU_RISCV` |
| CPU | Cortex-A72 | Cortex-A53 | Cortex-A8 (ARMv7-A, 32-bit) | 4x XuanTie C910 (RV64GC) | RV64GC (emulated, 4 harts) |
| RAM | Configurable (2 GB in the `run`/`run-gui` targets) | 1 GB | 512 MB | 4 GB | 512 MB in `run` |
| UART | PL011 @ 0x09000000 | PL011 @ 0x3F201000 | 16550 @ 0x44E09000 | DW 16550 @ 0xFFE7014000 | ns16550a @ 0x10000000 |
| Load address | 0x40000000 | 0x80000 | 0x82000000 | 0x200000 | 0x80200000 |
| Heap size | 32 MB (`kernel/src/main.c`; the GUI's window buffers need it — note this is more than the `cmake configure` status message says, which is stale) | 32 MB | 32 MB | 32 MB | 32 MB |
| Output file | `os2warp.img` | `kernel8.img` | `os2warp-bbb.img` | `os2warp-beaglev-ahead.img` | `os2warp-qemu-riscv.img` |
| Boot method | Direct kernel load (`-kernel`) | Pi firmware chain | U-Boot `bootz` | OpenSBI → U-Boot `booti` (S-mode) | OpenSBI `-bios default` → `-kernel` (S-mode) |
| Disk | Preloaded RAM window (§3) — no real VirtIO block driver yet | none yet | microSD (FAT16/32 + ext4) | microSD (FAT16/32 + ext4) | `sdhci-pci` SD card (FAT16/32 + ext4) |

### Boot sequence (QEMU)

```
QEMU loads os2warp.img → 0x40000000
  │
  ▼
boot.S  (_start)
  ├── Zero BSS
  ├── Set stack pointer (_stack_top, linker.ld)
  └── bl kernel_main
        │
        ▼
kernel/src/main.c  (kernel_main)
  ├── mem_init / scheduler_init
  ├── vfs_init → blk_init → GPT scan
  ├── ifs_loader_init → register ext4 + btrfs drivers → mount
  ├── load CONFIG.SYS → unixroot_configure/unixroot_create_tree (§7)
  ├── kbd_init
  └── kernel_shell()  ──►  dos_read_line() (§6)
                              ├── DOSKEY history/macro expansion
                              ├── dos_shell_dispatch() → dos/dos_commands.c
                              └── kernel-local commands (VER, MEM, IFS,
                                  RUN, BASIC, AI, GUI, HELP)
```

---

## 13. Project structure

```
os24arm/
├── CMakeLists.txt          Root build: platform detection, all targets
├── bootloader/
│   ├── boot.S               QEMU entry point
│   ├── rpi_boot.S           Raspberry Pi entry point
│   ├── linker.ld / rpi_linker.ld
│   └── bbb_uEnv.txt.in / beaglev_ahead_uEnv.txt.in   U-Boot scripts (→ build/uEnv.txt)
├── kernel/
│   ├── CMakeLists.txt        Actual kernel source list (see below)
│   ├── linker.ld / bbb_linker.ld / beaglev_linker.ld (RV64: BeagleV-Ahead + QEMU_RISCV)
│   ├── include/               types.h, uart.h, vfs.h, ifs_loader.h, ai.h, ...
│   └── src/
│       ├── main.c              kernel_main, shell loop, HELP/VER/MEM/IFS/...
│       ├── uart.c / kprintf.c   PL011 driver, kernel printf
│       ├── vfs.c / ifs_loader.c VFS layer, installable-FS registry
│       ├── blkdev.c / gpt.c     Block device registry, GPT partitions
│       ├── lx_loader.c          OS/2 LX .EXE loader (RUN)
│       ├── ai.c                 On-box AI assistant
│       ├── basic.c              QuickBASIC interpreter
│       ├── unixroot.c           EMX dir tree from SET UNIXROOT= (§7)
│       ├── bbb_boot.S           BeagleBone Black entry (zImage header, ARMv7)
│       ├── beaglev_boot.S       BeagleV-Ahead entry (RISC-V Image header, RV64)
│       ├── ns16550.c            16550 UART driver (both Beagle boards)
│       ├── sdhci.c              microSD driver: AM335x MMCHS / TH1520 DWC MSHC
│       ├── mbr.c                MBR partition probe (SD cards)
│       ├── qemu_pci.c           QEMU_RISCV: finds sdhci-pci, assigns its BAR
│       ├── fat32.c              FAT16/FAT32 driver (Beagle boot partition)
│       ├── beagle_main.c        Beagle kernel_main + serial prompt (+ CD/DIR via os2api)
│       ├── arm_aeabi.c          __aeabi_mem*/div runtime helpers (BBB, ARMv7)
│       └── gui/                 Wayland-style compositor (framebuffer,
│                                 wayland, virtio_gpu, virtio_input, ramfb)
├── dos/
│   ├── dos_commands.c        DIR/TYPE/COPY/COMP/FC/DEL/REN/MD/RD/CD/VOL/
│   │                         ATTRIB/ECHO/CLS/DOSKEY/DEBUG/EXIT + dos_read_line()
│   ├── cmd_main.c            Standalone CMD.EXE entry point
│   └── dos.h
├── os2api/
│   └── os2_api.c              DosOpen/DosRead/DosWrite/DosFindFirst/... bridge
├── fs/
│   ├── ext4/                  Read/write ext4 driver
│   ├── btrfs/                 Read/write copy-on-write btrfs driver
│   └── f2fs/                  Read/write log-structured f2fs driver
│                               (standalone - not wired into kernel boot)
├── lx/                        Host-side OS/2 LX builder tools
├── rexx/                      REXX interpreter (linked, not yet exposed)
├── tools/kcc/                 Kayte C Compiler (alternative cross-compiler)
├── kayte/                     Separate Free Pascal Kayte-language project
├── workplace/                 Qt6 Workplace Shell desktop (not in default build)
├── installer/                 C++ installer wizard
└── scripts/
    ├── mktestdisk.sh           ext4+btrfs GPT test disk builder
    ├── mksdcard.py             QEMU_RISCV test SD card (FAT32 + ext4, MBR)
    ├── mkbtrfs_image.py        Minimal btrfs image builder (no mkfs.btrfs on macOS)
    ├── check_btrfs_image.py    Independent btrfs structural verifier
    ├── test_ext4_driver.sh     Host-native ext4 write-path test (real mke2fs/e2fsck)
    ├── test_f2fs_driver.sh     Host-native f2fs write-path test (self-check only)
    └── build_iso.sh            Bootable ISO builder
```

---

## 14. Contributing

Areas of interest:

- **Storage:** a real VirtIO block driver (`blkdev.c`'s `virtio_blk_*` are
  still stubs); btrfs extent-tree/checksum-tree maintenance for real
  Linux/`btrfs-progs` interop; wiring `fs/f2fs/` into `ifs_loader_init()`/
  boot mounting the way ext4/btrfs already are; getting `f2fs-tools`
  building on macOS (or access to a Linux box) to finally cross-check the
  f2fs driver against something other than itself.
- **DEBUG:** `A`/`U` (an AArch64 assembler/disassembler), `G`/`P`/`T`
  (breakpoint/single-step support in the exception vectors).
- **Shell:** wiring `rexx/` up to a command.
- **Networking:** there is none — no driver, no TCP/IP stack.
- **Documentation:** the Doxygen output in `docs/html/` (from the root
  `Doxyfile`) is the closest thing to an API reference right now.

---

## 15. License

GPLv3 — see `LICENSE`.

---

**Status:** Boots on QEMU virt (serial console + GUI) | Boots on Raspberry Pi
3+ (less exercised) | BeagleBone Black and BeagleV-Ahead serial-console
ports (smoke-tested in QEMU only) | Active development.
