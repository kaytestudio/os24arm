cmake_minimum_required(VERSION 3.21 FATAL_ERROR)

# ── Platform Selection ────────────────────────────────────────────────────────
# Set target platform: QEMU (default), RPI3, BBB (BeagleBone Black),
# BEAGLEV_AHEAD (BeagleV-Ahead) or QEMU_RISCV (QEMU riscv64 "virt")
set(TARGET_PLATFORM "QEMU" CACHE STRING "Target platform: QEMU, RPI3, BBB, BEAGLEV_AHEAD or QEMU_RISCV")
set_property(CACHE TARGET_PLATFORM PROPERTY STRINGS "QEMU" "RPI3" "BBB" "BEAGLEV_AHEAD" "QEMU_RISCV")

# Each platform implies a CPU architecture. QEMU and RPI3 run the full
# AArch64 kernel; BBB (TI AM335x, Cortex-A8) is 32-bit ARMv7-A and
# BEAGLEV_AHEAD (T-Head TH1520, XuanTie C910) is RV64 - those two build a
# serial-console kernel only (kernel/src/beagle_main.c), since the MMU,
# GUI and LX (.EXE/.DLL) pipeline are still AArch64-specific. QEMU_RISCV
# is that same RV64 kernel on QEMU's riscv64 "virt" machine, for running
# and debugging the RISC-V port without a board (make run).
if(TARGET_PLATFORM STREQUAL "QEMU" OR TARGET_PLATFORM STREQUAL "RPI3")
    set(TARGET_ARCH "aarch64")
elseif(TARGET_PLATFORM STREQUAL "BBB")
    set(TARGET_ARCH "arm")
elseif(TARGET_PLATFORM STREQUAL "BEAGLEV_AHEAD" OR TARGET_PLATFORM STREQUAL "QEMU_RISCV")
    set(TARGET_ARCH "riscv64")
else()
    message(FATAL_ERROR "Unknown TARGET_PLATFORM '${TARGET_PLATFORM}' (expected QEMU, RPI3, BBB, BEAGLEV_AHEAD or QEMU_RISCV)")
endif()

# ── Compiler Selection ─────────────────────────────────────────────────────
set(USE_KCC OFF CACHE BOOL "Use KCC (Kayte C Compiler) instead of clang")
if(USE_KCC AND NOT TARGET_ARCH STREQUAL "aarch64")
    message(FATAL_ERROR "USE_KCC only supports AArch64 platforms (QEMU, RPI3)")
endif()

# ── Tell CMake this is a bare-metal target (suppresses host OS linker flags) ──
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ${TARGET_ARCH})

# Prevent CMake from testing the cross-compiler with a host-style link
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(OS2WarpARM64 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Platform detection ────────────────────────────────────────────────────────
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(HOST_PLATFORM "Linux")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(HOST_PLATFORM "macOS")
else()
    set(HOST_PLATFORM "Unknown")
    message(WARNING "Unknown host platform: ${CMAKE_HOST_SYSTEM_NAME}")
endif()

message(STATUS "Host platform: ${HOST_PLATFORM}")

# ── Toolchain compiler family ────────────────────────────────────────────────
# CMAKE_C_COMPILER_ID reflects whichever compiler ended up configured: clang
# by default, or GNU when a GCC-based cross-toolchain was supplied via
# -DCMAKE_TOOLCHAIN_FILE=cmake/arm64-baremetal-toolchain.cmake (which sets
# CMAKE_C_COMPILER to aarch64-*-gcc before this project() call runs). Several
# flags/tools below (clang's -target, LLVM's ar/ranlib/nm/lld) apply only to
# the clang path and are skipped for a GCC-based toolchain.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(USING_CLANG_TOOLCHAIN TRUE)
else()
    set(USING_CLANG_TOOLCHAIN FALSE)
    message(STATUS "Detected ${CMAKE_C_COMPILER_ID} toolchain: ${CMAKE_C_COMPILER}")
endif()

# ── KCC (Kayte C Compiler) Integration ──────────────────────────────────────
if(USE_KCC)
    message(STATUS "")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "Using KCC (Kayte C Compiler) for ARM64 cross-compilation")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "")

    # Build KCC as an external project
    include(ExternalProject)

    set(KCC_SOURCE_DIR ${CMAKE_BINARY_DIR}/kcc-src)
    set(KCC_BUILD_DIR ${CMAKE_BINARY_DIR}/kcc-build)
    set(KCC_INSTALL_DIR ${CMAKE_BINARY_DIR}/kcc-install)

    ExternalProject_Add(kcc_compiler
            GIT_REPOSITORY https://github.com/ringsce/kcc.git
            GIT_TAG master
            SOURCE_DIR ${KCC_SOURCE_DIR}
            BINARY_DIR ${KCC_BUILD_DIR}
            INSTALL_DIR ${KCC_INSTALL_DIR}

            # Configure step - try CMake first, then fallback to Makefile
            CONFIGURE_COMMAND
            ${CMAKE_COMMAND} -E echo "Configuring KCC..." &&
    (
            ${CMAKE_COMMAND} ${KCC_SOURCE_DIR} -B ${KCC_BUILD_DIR} ||
            ${CMAKE_COMMAND} -E echo "No CMakeLists.txt found, will use Makefile"
    )

            # Build step
            BUILD_COMMAND
            ${CMAKE_COMMAND} -E echo "Building KCC..." &&
    (
            ${CMAKE_COMMAND} --build ${KCC_BUILD_DIR} ||
            make -C ${KCC_SOURCE_DIR} ||
            ${CMAKE_COMMAND} -E echo "Warning: KCC build may have failed"
    )

            # Install step
            INSTALL_COMMAND
            ${CMAKE_COMMAND} -E make_directory ${KCC_INSTALL_DIR}/bin &&
    (
            ${CMAKE_COMMAND} -E copy ${KCC_BUILD_DIR}/kcc ${KCC_INSTALL_DIR}/bin/kcc ||
            ${CMAKE_COMMAND} -E copy ${KCC_SOURCE_DIR}/kcc ${KCC_INSTALL_DIR}/bin/kcc ||
            ${CMAKE_COMMAND} -E copy ${KCC_SOURCE_DIR}/bin/kcc ${KCC_INSTALL_DIR}/bin/kcc ||
            ${CMAKE_COMMAND} -E echo "Warning: Could not find kcc binary"
    )

            BUILD_ALWAYS FALSE
            LOG_DOWNLOAD TRUE
            LOG_CONFIGURE TRUE
            LOG_BUILD TRUE
            LOG_INSTALL TRUE
    )

    # Set KCC as the compiler
    set(KCC_BINARY ${KCC_INSTALL_DIR}/bin/kcc)
    set(CMAKE_C_COMPILER ${KCC_BINARY} CACHE FILEPATH "KCC compiler" FORCE)
    set(CMAKE_ASM_COMPILER ${KCC_BINARY} CACHE FILEPATH "KCC assembler" FORCE)

    message(STATUS "KCC will be built at: ${KCC_BINARY}")
    message(STATUS "")

    # KCC-specific compiler flags (adjust as needed)
    set(ARCH_FLAGS
            -target aarch64-none-elf
            -mcpu=cortex-a72
            -ffreestanding
            -fno-builtin
            -nostdlib
            ${PLATFORM_DEFINES}
    )

else()
    # Default: Use clang
    message(STATUS "Using clang for cross-compilation (default)")
    message(STATUS "To use KCC instead: cmake -DUSE_KCC=ON ..")
    message(STATUS "")

    if(NOT CMAKE_C_COMPILER)
        find_program(CLANG_EXECUTABLE NAMES clang)
        if(CLANG_EXECUTABLE)
            set(CMAKE_C_COMPILER ${CLANG_EXECUTABLE} CACHE FILEPATH "C compiler")
            set(CMAKE_ASM_COMPILER ${CLANG_EXECUTABLE} CACHE FILEPATH "ASM compiler")
            message(STATUS "Using clang: ${CLANG_EXECUTABLE}")
        else()
            message(FATAL_ERROR "clang not found! Install with: brew install llvm (macOS) or apt install clang (Linux)")
        endif()
    endif()

    # Clang-specific flags
    set(ARCH_FLAGS
            -target aarch64-none-elf
            -mcpu=cortex-a72
            -mstrict-align
            -ffreestanding
            -fno-builtin
            -fno-stack-protector
            -fno-pie
            -fno-pic
            -nostdlib
            -nostdinc
            ${PLATFORM_DEFINES}
    )
endif()

# ── Platform-Specific Configuration ───────────────────────────────────────────
if(TARGET_PLATFORM STREQUAL "RPI3")
    # Raspberry Pi 3+ Configuration
    set(PLATFORM_DEFINES
            -DPLATFORM_RPI3
            -DUART_BASE=0x3F201000
            -DBCM2837_PERI_BASE=0x3F000000
    )
    set(KERNEL_LOAD_ADDR "0x80000")
    set(HEAP_SIZE "32 MB")
    set(PLATFORM_NAME "Raspberry Pi 3+")
    set(KERNEL_IMAGE "kernel8.img")

    message(STATUS "")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "Target Platform: Raspberry Pi 3+")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "  UART Base:     0x3F201000")
    message(STATUS "  Load Address:  0x80000")
    message(STATUS "  Heap Size:     32 MB")
    message(STATUS "  Output:        kernel8.img")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "")

elseif(TARGET_PLATFORM STREQUAL "BBB")
    # BeagleBone Black (TI AM335x, ARM Cortex-A8). U-Boot's `bootz` hands
    # over in SVC mode with the MMU and caches off (see kernel/src/bbb_boot.S);
    # 0x82000000 is the stock BeagleBone ${loadaddr}, clear of U-Boot, which
    # relocates itself to the top of the 512 MB of DDR at 0x80000000.
    set(PLATFORM_DEFINES
            -DPLATFORM_BBB
            -DUART_BASE=0x44E09000
            -DUART_REG_SHIFT=2
            -DSD_HOST_AM335X_MMCHS          # microSD: MMCHS0, 96 MHz functional clock
            -DSD_BASE=0x48060000
            -DSD_BASE_CLK_HZ=96000000
    )
    set(KERNEL_LOAD_ADDR "0x82000000")
    set(HEAP_SIZE "32 MB")
    set(PLATFORM_NAME "BeagleBone Black")
    set(KERNEL_IMAGE "os2warp-bbb.img")

    message(STATUS "")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "Target Platform: BeagleBone Black (AM335x, ARMv7-A)")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "  UART Base:     0x44E09000 (UART0, 16550)")
    message(STATUS "  Load Address:  ${KERNEL_LOAD_ADDR}")
    message(STATUS "  Heap Size:     ${HEAP_SIZE}")
    message(STATUS "  Output:        ${KERNEL_IMAGE} (boot with U-Boot bootz)")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "")

elseif(TARGET_PLATFORM STREQUAL "BEAGLEV_AHEAD")
    # BeagleV-Ahead (T-Head TH1520, 4x XuanTie C910 RV64GC). DDR starts at
    # 0x0 with OpenSBI in the first 2 MB; U-Boot's `booti` enters the
    # kernel in S-mode at DDR base + the image header's text_offset
    # (0x200000, same slot as Linux - see kernel/src/beaglev_boot.S).
    set(PLATFORM_DEFINES
            -DPLATFORM_BEAGLEV_AHEAD
            -DUART_BASE=0xFFE7014000
            -DUART_REG_SHIFT=2
            -DSD_BASE=0xFFE7090000          # microSD: SDIO0 (DWC MSHC), 198 MHz
            -DSD_BASE_CLK_HZ=198000000
    )
    set(KERNEL_LOAD_ADDR "0x00200000")
    set(HEAP_SIZE "32 MB")
    set(PLATFORM_NAME "BeagleV-Ahead")
    set(KERNEL_IMAGE "os2warp-beaglev-ahead.img")

    message(STATUS "")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "Target Platform: BeagleV-Ahead (TH1520, RV64GC)")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "  UART Base:     0xFFE7014000 (UART0, DW 16550)")
    message(STATUS "  Load Address:  ${KERNEL_LOAD_ADDR}")
    message(STATUS "  Heap Size:     ${HEAP_SIZE}")
    message(STATUS "  Output:        ${KERNEL_IMAGE} (boot with U-Boot booti)")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "")

elseif(TARGET_PLATFORM STREQUAL "QEMU_RISCV")
    # QEMU riscv64 "virt": the BeagleV-Ahead kernel on emulated hardware.
    # OpenSBI (-bios default) enters S-mode at DRAM base (0x80000000) + the
    # image header's 2 MB text_offset, like booti does on the board. The
    # console is virt's ns16550a (byte-spaced registers); the SD card sits
    # behind a PCI sdhci-pci controller whose BAR0 kernel/src/qemu_pci.c
    # assigns to SD_BASE, inside virt's PCI MMIO window.
    set(PLATFORM_DEFINES
            -DPLATFORM_QEMU_RISCV
            -DUART_BASE=0x10000000
            -DUART_REG_SHIFT=0
            -DSD_BASE=0x40000000
            -DSD_BASE_CLK_HZ=52000000
    )
    set(KERNEL_LOAD_ADDR "0x80200000")
    set(HEAP_SIZE "32 MB")
    set(PLATFORM_NAME "QEMU RISC-V virt")
    set(KERNEL_IMAGE "os2warp-qemu-riscv.img")

    message(STATUS "")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "Target Platform: QEMU riscv64 virt (RV64)")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "  UART Base:     0x10000000 (ns16550a)")
    message(STATUS "  Load Address:  ${KERNEL_LOAD_ADDR}")
    message(STATUS "  Heap Size:     ${HEAP_SIZE}")
    message(STATUS "  Output:        ${KERNEL_IMAGE} (make run)")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "")

else()
    # QEMU virt machine Configuration (default)
    set(PLATFORM_DEFINES
            -DPLATFORM_QEMU
            -DUART_BASE=0x09000000
    )
    set(KERNEL_LOAD_ADDR "0x40000000")
    set(HEAP_SIZE "8 MB")
    set(PLATFORM_NAME "QEMU virt")
    set(KERNEL_IMAGE "os2warp.img")

    message(STATUS "")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "Target Platform: QEMU virt machine")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "  UART Base:     0x09000000")
    message(STATUS "  Load Address:  0x40000000")
    message(STATUS "  Heap Size:     8 MB")
    message(STATUS "  Output:        os2warp.img")
    message(STATUS "═══════════════════════════════════════════════════════════")
    message(STATUS "")
endif()

# ── Explicitly set objcopy (use llvm-objcopy for clang, objcopy for KCC) ──────
if(USE_KCC)
    find_program(CMAKE_OBJCOPY
            NAMES
            aarch64-none-elf-objcopy
            aarch64-linux-gnu-objcopy
            objcopy
            PATHS
            /usr/bin
            /usr/local/bin
            REQUIRED
    )
else()
    find_program(CMAKE_OBJCOPY
            NAMES
            llvm-objcopy
            llvm-objcopy-17
            llvm-objcopy-16
            llvm-objcopy-15
            PATHS
            /opt/homebrew/opt/llvm/bin
            /usr/local/opt/llvm/bin
            /usr/bin
            /usr/local/bin
            REQUIRED
    )
endif()

# ── Explicitly set objcopy (use llvm-objcopy for clang) ───────────────────────
find_program(CMAKE_OBJCOPY
        NAMES
        llvm-objcopy
        llvm-objcopy-17
        llvm-objcopy-16
        llvm-objcopy-15
        PATHS
        /opt/homebrew/opt/llvm/bin
        /usr/local/opt/llvm/bin
        /usr/bin
        /usr/local/bin
        REQUIRED
)
message(STATUS "objcopy: ${CMAKE_OBJCOPY}")

# ── nm + python3 (for computing LX entry-point offsets) ───────────────────────
if(USING_CLANG_TOOLCHAIN)
    find_program(LLVM_NM_EXECUTABLE
            NAMES llvm-nm llvm-nm-17 llvm-nm-16 llvm-nm-15
            PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /usr/bin /usr/local/bin
            REQUIRED
    )
else()
    # Use the GCC cross-toolchain's own nm (matching CMAKE_C_COMPILER's
    # directory/prefix, e.g. aarch64-none-elf-gcc -> aarch64-none-elf-nm)
    # rather than llvm-nm, which may not be installed at all.
    get_filename_component(TOOLCHAIN_BIN_DIR ${CMAKE_C_COMPILER} DIRECTORY)
    get_filename_component(TOOLCHAIN_CC_NAME ${CMAKE_C_COMPILER} NAME)
    string(REGEX REPLACE "gcc$" "nm" TOOLCHAIN_NM_NAME "${TOOLCHAIN_CC_NAME}")
    find_program(LLVM_NM_EXECUTABLE
            NAMES ${TOOLCHAIN_NM_NAME} nm
            PATHS ${TOOLCHAIN_BIN_DIR}
            REQUIRED
    )
endif()
find_program(Python3_EXECUTABLE NAMES python3 REQUIRED)
message(STATUS "nm: ${LLVM_NM_EXECUTABLE}")
message(STATUS "python3: ${Python3_EXECUTABLE}")

if(USING_CLANG_TOOLCHAIN)
    # Explicitly set ar/ranlib (system ar/ranlib only understand Mach-O and
    # silently produce broken archives out of ELF .o files, which then drop
    # symbols at final link time). CMAKE_AR/CMAKE_RANLIB are pre-populated by
    # CMake's own compiler-ABI detection during project(), so a plain
    # find_program() is a no-op here — it must be forced. ────────────────────
    find_program(LLVM_AR_EXECUTABLE
            NAMES llvm-ar llvm-ar-17 llvm-ar-16 llvm-ar-15
            PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /usr/bin /usr/local/bin
            REQUIRED
    )
    find_program(LLVM_RANLIB_EXECUTABLE
            NAMES llvm-ranlib llvm-ranlib-17 llvm-ranlib-16 llvm-ranlib-15
            PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /usr/bin /usr/local/bin
            REQUIRED
    )
    set(CMAKE_AR ${LLVM_AR_EXECUTABLE} CACHE FILEPATH "archiver" FORCE)
    set(CMAKE_RANLIB ${LLVM_RANLIB_EXECUTABLE} CACHE FILEPATH "ranlib" FORCE)
    # project()'s compiler-ABI detection already created CMAKE_AR/CMAKE_RANLIB as normal
    # (non-cache) variables; the CACHE FORCE above updates the cache entry but does not
    # clear that shadow, so plain ${CMAKE_AR} reads here and in every add_subdirectory()
    # below would still resolve to the old system ar/ranlib. Set the normal variable too.
    set(CMAKE_AR "${LLVM_AR_EXECUTABLE}")
    set(CMAKE_RANLIB "${LLVM_RANLIB_EXECUTABLE}")
else()
    # A GCC cross-toolchain's own ar/ranlib (matching its compiler prefix) is
    # already ELF-aware and correctly auto-detected by CMake's compiler-ABI
    # detection during project() above - no override needed.
endif()
message(STATUS "ar: ${CMAKE_AR}")
message(STATUS "ranlib: ${CMAKE_RANLIB}")

# ── Per-architecture target triple and CPU flags ─────────────────────────────
if(TARGET_ARCH STREQUAL "arm")
    # Cortex-A8 has neither a hardware divider nor (with bootz's MMU-off
    # handoff) any memory type that allows unaligned access. Soft-float
    # avoids having to enable VFP/NEON in bbb_boot.S before C code runs.
    set(ARCH_TRIPLE armv7a-none-eabi)
    set(ARCH_CPU_FLAGS
            -mcpu=cortex-a8
            -marm
            -mfloat-abi=soft
            -mno-unaligned-access
            -fno-unwind-tables
            -fno-asynchronous-unwind-tables
    )
elseif(TARGET_ARCH STREQUAL "riscv64")
    # No F/D: sstatus.FS stays Off, so the kernel never touches FP state.
    # -mno-relax: beaglev_boot.S doesn't set up gp for linker relaxation.
    set(ARCH_TRIPLE riscv64-unknown-elf)
    set(ARCH_CPU_FLAGS
            -march=rv64imac_zicsr_zifencei
            -mabi=lp64
            -mcmodel=medany
            -mno-relax
    )
else()
    set(ARCH_TRIPLE aarch64-none-elf)
    set(ARCH_CPU_FLAGS
            -mcpu=cortex-a72
            -mstrict-align
    )
endif()

# ── Cross-compiler flags ──────────────────────────────────────────────────────
if(USING_CLANG_TOOLCHAIN)
    set(ARCH_FLAGS
            -target ${ARCH_TRIPLE}
            ${ARCH_CPU_FLAGS}
            -ffreestanding
            -fno-builtin
            -fstack-protector-strong
            -fno-pie
            -fno-pic
            -nostdlib
            -nostdinc
            ${PLATFORM_DEFINES}
    )
else()
    # A GCC cross-toolchain's compiler binary is already architecture-specific
    # (aarch64-none-elf-gcc), unlike clang which is retargeted per invocation
    # via -target - so no -target flag here.
    set(ARCH_FLAGS
            ${ARCH_CPU_FLAGS}
            -ffreestanding
            -fno-builtin
            -fstack-protector-strong
            -fno-pie
            -fno-pic
            -nostdlib
            -nostdinc
            ${PLATFORM_DEFINES}
    )
endif()

set(WARN_FLAGS
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
)

set(OPT_FLAGS -O2 -g)

add_compile_options(${ARCH_FLAGS} ${WARN_FLAGS} ${OPT_FLAGS})
if(USING_CLANG_TOOLCHAIN)
    # -fuse-ld=lld: the freestanding aarch64-none-elf target needs a linker that
    # understands GNU linker scripts (-T). macOS's default cc/ld64 driver rejects
    # -T/-Bstatic/-EL outright, so force LLVM's lld (works with both Apple clang
    # and Homebrew clang, and with system clang/lld on Linux).
    add_link_options(${ARCH_FLAGS} -nostdlib -fuse-ld=lld)
else()
    # The GCC cross-toolchain's own linker already understands -T GNU linker
    # scripts; no -fuse-ld override needed.
    add_link_options(${ARCH_FLAGS} -nostdlib)
endif()

# Clang-only pieces of a few standalone compile/link invocations further
# below (the LX toolchain, CMD.EXE) that don't go through ARCH_FLAGS/
# add_link_options: -target retargets clang per-invocation, and -fuse-ld=lld
# forces LLVM's linker - neither applies to a GCC cross-toolchain (its
# compiler binary is already architecture-specific, and it ships its own
# linker), same reasoning as above.
if(USING_CLANG_TOOLCHAIN)
    set(LX_TARGET_FLAGS -target aarch64-none-elf)
    set(LX_LINKER_FLAGS -fuse-ld=lld)
else()
    set(LX_TARGET_FLAGS "")
    set(LX_LINKER_FLAGS "")
endif()

# ── Include dirs shared across sub-projects ───────────────────────────────────
include_directories(
        ${CMAKE_SOURCE_DIR}/kernel/include
        ${CMAKE_SOURCE_DIR}/fs/ext4          # IFS ext4 driver
)

# ── Sub-projects ──────────────────────────────────────────────────────────────
add_subdirectory(bootloader)
add_subdirectory(kernel)
if(TARGET_ARCH STREQUAL "aarch64")
    # DOS commands, REXX and the filesystem drivers are only linked into
    # the AArch64 kernel so far (see kernel/CMakeLists.txt).
    add_subdirectory(rexx)
    add_subdirectory(os2api)
    add_subdirectory(dos)
    # Add filesystems subdirectory
    add_subdirectory(fs)
else()
    # The BeagleBone Black and BeagleV-Ahead kernels link the OS/2 API
    # (DosOpen, DosRead, KernVMAlloc, ...) on top of the VFS, with no
    # filesystem mounted yet.
    add_subdirectory(os2api)
endif()
# Kayte host tool (FreePascal) - a build-machine tool, not freestanding code
add_subdirectory(kayte)

# Make kernel depend on KCC if using KCC
if(USE_KCC)
    add_dependencies(os2warp_kernel kcc_compiler)
endif()

# ═════════════════════════════════════════════════════════════════════════════
# Begin AArch64-only section: the LX tools, the KCC/LX app pipeline,
# DOSCALLS.DLL, CMD.EXE and KAYTEVM.EXE. LX images here carry ARM64 code
# and are loaded by the AArch64 kernel's LX loader, so none of this is
# built for BBB or BEAGLEV_AHEAD. Ends at "End AArch64-only section" below.
# ═════════════════════════════════════════════════════════════════════════════
if(TARGET_ARCH STREQUAL "aarch64")

# ── LX Tools (Host-Native Build) ──────────────────────────────────────────────
# The lx tools must be built with the host compiler, not cross-compiled.
# We use ExternalProject to build them separately with native toolchain.

if(CMAKE_CROSSCOMPILING)
    # Cross-compiling: Build lx tools as external project with host compiler
    include(ExternalProject)

    message(STATUS "Cross-compiling detected: Building lx tools with host compiler")

    # Detect host compilers
    if(APPLE)
        set(HOST_C_COMPILER "/usr/bin/cc")
        set(HOST_CXX_COMPILER "/usr/bin/c++")
    else()
        set(HOST_C_COMPILER "gcc")
        set(HOST_CXX_COMPILER "g++")
    endif()

    ExternalProject_Add(lx_host_tools
            SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lx
            BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/lx-host-build
            CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${HOST_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${HOST_CXX_COMPILER}
            -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
            BUILD_ALWAYS FALSE
            INSTALL_COMMAND ""
            LOG_BUILD TRUE
    )

    # Set paths to the host-built tools
    set(LXBUILD ${CMAKE_CURRENT_BINARY_DIR}/lx-host-build/lxbuild CACHE FILEPATH "Path to lxbuild tool")
    set(MAKEEXE ${CMAKE_CURRENT_BINARY_DIR}/lx-host-build/makeexe CACHE FILEPATH "Path to makeexe tool")
    set(MAKEDLL ${CMAKE_CURRENT_BINARY_DIR}/lx-host-build/makedll CACHE FILEPATH "Path to makedll tool")

    message(STATUS "LX tools will be built in: ${CMAKE_CURRENT_BINARY_DIR}/lx-host-build")
    message(STATUS "  Using host compiler: ${HOST_C_COMPILER}")
    message(STATUS "  LXBUILD: ${LXBUILD}")
    message(STATUS "  MAKEEXE: ${MAKEEXE}")
    message(STATUS "  MAKEDLL: ${MAKEDLL}")

else()
    # Native build: Build lx tools normally
    add_subdirectory(lx)

    # Set paths to the tools
    set(LXBUILD ${CMAKE_CURRENT_BINARY_DIR}/lx/lxbuild CACHE FILEPATH "Path to lxbuild tool")
    set(MAKEEXE ${CMAKE_CURRENT_BINARY_DIR}/lx/makeexe CACHE FILEPATH "Path to makeexe tool")
    set(MAKEDLL ${CMAKE_CURRENT_BINARY_DIR}/lx/makedll CACHE FILEPATH "Path to makedll tool")

endif()

# Every LX app links at this fixed address (lx/lx_app.ld) so the kernel's
# built-in loader (kernel/include/lx_loader.h) can copy pages verbatim with
# no relocation. Must match LX_LOAD_BASE there. Set unconditionally (not
# just under the KCC block below) since the CMD.EXE target also needs it.
set(LX_APP_LD ${CMAKE_SOURCE_DIR}/lx/lx_app.ld)
set(LX_LOAD_BASE_HEX 0x48000000)

# DLLs link at their own fixed address (lx/lx_dll.ld), distinct from
# LX_APP_LD's LX_LOAD_BASE - an EXE that embeds and loads a DLL at
# runtime (kernel/src/lx_dll_loader.c) is itself already resident at
# LX_LOAD_BASE, and this project's flat, no-MMU address space can't have
# two images share an address. Must match LX_DLL_LOAD_BASE in
# kernel/include/lx_dll_loader.h.
set(LX_DLL_LD ${CMAKE_SOURCE_DIR}/lx/lx_dll.ld)
set(LX_DLL_LOAD_BASE_HEX 0x4A000000)
set(LX_ENTRY_OFFSET_SCRIPT ${CMAKE_SOURCE_DIR}/lx/lx_entry_offset.py)
set(LX_EXPORT_OFFSETS_SCRIPT ${CMAKE_SOURCE_DIR}/lx/lx_export_offsets.py)

# ── KCC Host Tool + LX Application Toolchain ──────────────────────────────────
# Independent of USE_KCC (which would replace clang as the KERNEL's own
# compiler — not viable yet, KCC's C-language coverage is far from complete).
# This instead builds KCC as a host-native tool from the local tools/kcc/
# source, for a separate pipeline: compile a freestanding C program with KCC
# (--bare-metal), cross-assemble/link the resulting ARM64 asm with this
# project's own clang/lld cross-toolchain, extract raw machine code, and wrap
# it as a genuine OS/2 LX .EXE/.DLL via the makeexe/makedll tools set up above.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/kcc/CMakeLists.txt)
    include(ExternalProject)

    if(NOT DEFINED HOST_C_COMPILER)
        if(APPLE)
            set(HOST_C_COMPILER "/usr/bin/cc")
            set(HOST_CXX_COMPILER "/usr/bin/c++")
        else()
            set(HOST_C_COMPILER "gcc")
            set(HOST_CXX_COMPILER "g++")
        endif()
    endif()

    ExternalProject_Add(kcc_host_tool
            SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/kcc
            BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/kcc-host-build
            CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${HOST_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${HOST_CXX_COMPILER}
            -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
            -DBUILD_SATURN_TOOLS=OFF
            -DBUILD_GCC_SH2=OFF
            -DINSTALL_GCC_SH2=OFF
            BUILD_ALWAYS FALSE
            INSTALL_COMMAND ""
            LOG_BUILD TRUE
    )

    set(KCC_BINARY ${CMAKE_CURRENT_BINARY_DIR}/kcc-host-build/bin/kcc CACHE FILEPATH "Path to host kcc tool")
    message(STATUS "KCC host tool (LX app pipeline) will be built at: ${KCC_BINARY}")

    # add_lx_executable(name source.c)
    # Compiles source.c with KCC (--bare-metal), cross-assembles/links the
    # result with this project's clang/lld toolchain at the LX app base
    # address, computes the real byte offset of `main` in the linked image
    # (it isn't necessarily at offset 0), extracts a raw binary, and wraps
    # it as name.EXE (OS/2 LX format, ARM64) via makeexe - loadable by the
    # kernel's built-in LX loader.
    function(add_lx_executable NAME SOURCE)
        get_filename_component(SOURCE_ABS ${SOURCE} ABSOLUTE)
        set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
        set(ASM_FILE ${OUT_DIR}/${NAME}.s)
        set(ELF_FILE ${OUT_DIR}/${NAME}.elf)
        set(CODE_BIN_FILE ${OUT_DIR}/${NAME}.code.bin)
        set(DATA_BIN_FILE ${OUT_DIR}/${NAME}.data.bin)
        set(EXE_FILE ${OUT_DIR}/${NAME}.EXE)

        add_custom_command(
                OUTPUT ${EXE_FILE}
                COMMAND ${KCC_BINARY} --bare-metal -S ${SOURCE_ABS} -o ${OUT_DIR}/${NAME}
                COMMAND ${CMAKE_C_COMPILER} ${LX_TARGET_FLAGS} -mcpu=cortex-a72
                        -nostdlib ${LX_LINKER_FLAGS} -T ${LX_APP_LD}
                        -o ${ELF_FILE} ${ASM_FILE}
                # Extract code (.text+.rodata) and data (.data+.bss) as
                # separate binaries, not one combined blob - lx_app.ld pads
                # a gap between them up to the fixed 2MB W^X split point
                # (LX_CODE_REGION_SIZE), which a single `objcopy -O binary`
                # over the whole address range would otherwise store as
                # literal zero bytes.
                COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.text --only-section=.rodata ${ELF_FILE} ${CODE_BIN_FILE}
                COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.data --only-section=.bss ${ELF_FILE} ${DATA_BIN_FILE}
                COMMAND bash -c "\"${MAKEEXE}\" \"${CODE_BIN_FILE}\" \"${DATA_BIN_FILE}\" \"${EXE_FILE}\" $(\"${Python3_EXECUTABLE}\" \"${LX_ENTRY_OFFSET_SCRIPT}\" \"${ELF_FILE}\" main ${LX_LOAD_BASE_HEX} \"${LLVM_NM_EXECUTABLE}\")"
                DEPENDS ${SOURCE_ABS} ${LX_APP_LD} kcc_host_tool lx_host_tools
                WORKING_DIRECTORY ${OUT_DIR}
                COMMENT "Building LX executable ${NAME}.EXE (KCC -> LX) from ${SOURCE}"
                VERBATIM
        )
        add_custom_target(${NAME}_lxexe ALL DEPENDS ${EXE_FILE})
        add_dependencies(${NAME}_lxexe kcc_host_tool lx_host_tools)
    endfunction()

    # add_lx_library(name source.c EXPORTS export1 export2 ...)
    # Same pipeline, wrapped as name.DLL via makedll, with each export's
    # real offset within the linked ELF looked up via lx_export_offsets.py
    # (same idea as add_lx_executable's entry-offset lookup, just for
    # however many named exports the caller wants). lx_builder_write()
    # (lx/lxbuilder.c) now actually serializes a real LX entry table and
    # resident names table from these, so the exports genuinely exist on
    # disk - the kernel loader still doesn't do DLL import resolution
    # though (see kernel/include/lx_loader.h), so nothing can dynamically
    # call into them yet.
    function(add_lx_library NAME SOURCE)
        cmake_parse_arguments(LXLIB "" "" "EXPORTS" ${ARGN})
        # CMake lists interpolate with ';' - inside the bash -c "..." string
        # below, literal semicolons would be parsed as shell statement
        # separators (splitting one command into many, each failing), not
        # as word breaks. Join with spaces first so it reads as ordinary
        # shell arguments.
        list(JOIN LXLIB_EXPORTS " " LXLIB_EXPORTS)
        get_filename_component(SOURCE_ABS ${SOURCE} ABSOLUTE)
        set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
        set(ASM_FILE ${OUT_DIR}/${NAME}.s)
        set(ELF_FILE ${OUT_DIR}/${NAME}.elf)
        set(CODE_BIN_FILE ${OUT_DIR}/${NAME}.code.bin)
        set(DATA_BIN_FILE ${OUT_DIR}/${NAME}.data.bin)
        set(DLL_FILE ${OUT_DIR}/${NAME}.DLL)

        add_custom_command(
                OUTPUT ${DLL_FILE}
                COMMAND ${KCC_BINARY} --bare-metal -S ${SOURCE_ABS} -o ${OUT_DIR}/${NAME}
                COMMAND ${CMAKE_C_COMPILER} ${LX_TARGET_FLAGS} -mcpu=cortex-a72
                        -nostdlib ${LX_LINKER_FLAGS} -T ${LX_APP_LD}
                        -o ${ELF_FILE} ${ASM_FILE}
                COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.text --only-section=.rodata ${ELF_FILE} ${CODE_BIN_FILE}
                COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.data --only-section=.bss ${ELF_FILE} ${DATA_BIN_FILE}
                COMMAND bash -c "\"${MAKEDLL}\" \"${CODE_BIN_FILE}\" \"${DATA_BIN_FILE}\" \"${DLL_FILE}\" ${NAME} $(\"${Python3_EXECUTABLE}\" \"${LX_EXPORT_OFFSETS_SCRIPT}\" \"${ELF_FILE}\" ${LX_LOAD_BASE_HEX} ${LXLIB_EXPORTS} -- \"${LLVM_NM_EXECUTABLE}\")"
                DEPENDS ${SOURCE_ABS} ${LX_APP_LD} kcc_host_tool lx_host_tools
                WORKING_DIRECTORY ${OUT_DIR}
                COMMENT "Building LX library ${NAME}.DLL (KCC -> LX) from ${SOURCE}"
                VERBATIM
        )
        add_custom_target(${NAME}_lxdll ALL DEPENDS ${DLL_FILE})
        add_dependencies(${NAME}_lxdll kcc_host_tool lx_host_tools)
    endfunction()

    # Demo targets — prove the pipeline works through the normal build.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/kcc/examples/hello_min.c)
        add_lx_executable(HELLO ${CMAKE_CURRENT_SOURCE_DIR}/tools/kcc/examples/hello_min.c)
        add_lx_library(HELLODLL ${CMAKE_CURRENT_SOURCE_DIR}/tools/kcc/examples/hello_min.c EXPORTS main)
    endif()
else()
    message(STATUS "tools/kcc not found — LX application toolchain (add_lx_executable/add_lx_library) disabled")
endif()

# ── Embed HELLO.EXE as a loader self-test payload ──────────────────────────────
# Proves the kernel's built-in LX loader (kernel/src/lx_loader.c) actually
# works in QEMU without needing a populated disk image yet: RUNTEST loads
# this embedded copy of HELLO.EXE via lx_exec_image() and reports the
# return value from its main() (see tools/kcc/examples/hello_min.c).
if(TARGET HELLO_lxexe)
    set(HELLO_EXE_PATH ${CMAKE_CURRENT_BINARY_DIR}/HELLO.EXE)
    set(LX_TEST_PAYLOAD_S ${CMAKE_CURRENT_BINARY_DIR}/lx_test_payload.S)
    set(LX_TEST_PAYLOAD_TEMPLATE ${CMAKE_CURRENT_BINARY_DIR}/lx_test_payload.configured.S)
    configure_file(${CMAKE_SOURCE_DIR}/kernel/src/lx_test_payload.S.in ${LX_TEST_PAYLOAD_TEMPLATE} @ONLY)

    # configure_file() only substitutes @HELLO_EXE_PATH@ once, at configure
    # time - it doesn't track HELLO.EXE's actual byte content, so it alone
    # can't make the .incbin'd payload staleness-tracked. OBJECT_DEPENDS
    # (tried first) didn't reliably trigger reassembly for this ASM source
    # either. A real add_custom_command OUTPUT, depending on HELLO_EXE_PATH,
    # gives CMake a genuine dependency edge: touching HELLO.EXE re-copies
    # this file, which correctly cascades to reassemble + relink the kernel.
    add_custom_command(
            OUTPUT ${LX_TEST_PAYLOAD_S}
            COMMAND ${CMAKE_COMMAND} -E copy ${LX_TEST_PAYLOAD_TEMPLATE} ${LX_TEST_PAYLOAD_S}
            DEPENDS ${LX_TEST_PAYLOAD_TEMPLATE} ${HELLO_EXE_PATH}
            COMMENT "Refreshing lx_test_payload.S (tracks HELLO.EXE)"
    )

    # os2warp_kernel is defined in kernel/CMakeLists.txt (a different
    # directory scope), and a plain target_sources() there can't see this
    # custom command's generation rule reliably - an OBJECT library defined
    # in the SAME scope as the custom command, then linked in normally,
    # sidesteps that cross-directory visibility problem entirely.
    add_library(lx_test_payload OBJECT ${LX_TEST_PAYLOAD_S})
    target_link_libraries(os2warp_kernel lx_test_payload)
    add_dependencies(os2warp_kernel HELLO_lxexe)
    target_compile_definitions(os2warp_kernel PRIVATE LX_HAS_TEST_PAYLOAD=1)
endif()

# ── LX DLL generator ─────────────────────────────────────────────────────────
# add_lx_dll(name SOURCES src1.c src2.c ... EXPORTS export1 export2 ...)
#
# Real-toolchain counterpart to add_lx_library() above (which goes through
# the immature KCC pipeline): for genuine production C code - built with
# this project's own aarch64-none-elf clang/lld cross-toolchain, linked at
# LX_DLL_LOAD_BASE (lx/lx_dll.ld) rather than an EXE's LX_LOAD_BASE, since
# an EXE that loads this DLL at runtime is itself already resident there -
# then wrapped as a real OS/2 LX .DLL via makedll, with each export's
# actual offset in the linked ELF looked up by lx_export_offsets.py (same
# idea as the EXE targets' entry-offset lookup, just for however many
# named exports the caller wants).
#
# lx_builder_write() (lx/lxbuilder.c) actually serializes a real LX entry
# table and resident names table from these exports, and
# kernel/src/lx_dll_loader.c can load a DLL built this way and resolve its
# exports by name at runtime (see os2api/os2_api_import.c for a caller
# doing exactly that with DOSCALLS.DLL below) - previously (see git
# history on lx_builder_add_export/lx_create_dll) the export data was
# captured in memory but never written to the file at all, and there was
# no loader for the DLL side regardless.
function(add_lx_dll NAME)
    cmake_parse_arguments(LXDLL "" "" "SOURCES;EXPORTS" ${ARGN})
    # CMake lists interpolate with ';' - inside the bash -c "..." string
    # below, literal semicolons would be parsed as shell statement
    # separators (splitting one command into many, each failing), not as
    # word breaks. Join with spaces first so it reads as ordinary shell
    # arguments.
    list(JOIN LXDLL_EXPORTS " " LXDLL_EXPORTS)
    set(OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
    set(ELF_FILE ${OUT_DIR}/${NAME}.elf)
    set(CODE_BIN_FILE ${OUT_DIR}/${NAME}.code.bin)
    set(DATA_BIN_FILE ${OUT_DIR}/${NAME}.data.bin)
    set(DLL_FILE ${OUT_DIR}/${NAME}.DLL)

    add_custom_command(
            OUTPUT ${DLL_FILE}
            COMMAND ${CMAKE_C_COMPILER} ${ARCH_FLAGS}
            -fno-stack-protector
            -I${CMAKE_CURRENT_SOURCE_DIR}/kernel/include
            -I${CMAKE_CURRENT_SOURCE_DIR}/fs/ext4
            -I${CMAKE_CURRENT_SOURCE_DIR}/fs/btrfs
            -I${CMAKE_CURRENT_SOURCE_DIR}/os2api
            -I${CMAKE_CURRENT_SOURCE_DIR}/dos
            ${LX_LINKER_FLAGS} -T ${LX_DLL_LD}
            -o ${ELF_FILE}
            ${LXDLL_SOURCES}
            # Extract code (.text+.rodata) and data (.data+.bss) as
            # separate binaries, not one combined blob - lx_dll.ld pads a
            # gap between them up to the fixed 2MB W^X split point
            # (LX_DLL_CODE_REGION_SIZE), which a single `objcopy -O binary`
            # over the whole address range would otherwise store as
            # literal zero bytes.
            COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.text --only-section=.rodata ${ELF_FILE} ${CODE_BIN_FILE}
            COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.data --only-section=.bss ${ELF_FILE} ${DATA_BIN_FILE}
            COMMAND bash -c "\"${MAKEDLL}\" \"${CODE_BIN_FILE}\" \"${DATA_BIN_FILE}\" \"${DLL_FILE}\" ${NAME} $(\"${Python3_EXECUTABLE}\" \"${LX_EXPORT_OFFSETS_SCRIPT}\" \"${ELF_FILE}\" ${LX_DLL_LOAD_BASE_HEX} ${LXDLL_EXPORTS} -- \"${LLVM_NM_EXECUTABLE}\")"
            DEPENDS ${LXDLL_SOURCES} ${LX_DLL_LD}
            WORKING_DIRECTORY ${OUT_DIR}
            COMMENT "Building LX DLL ${NAME}.DLL"
            VERBATIM
    )
    add_custom_target(${NAME}_lxdll ALL DEPENDS ${DLL_FILE})
    add_dependencies(${NAME}_lxdll lx_host_tools)
endfunction()

# ── DOSCALLS.DLL ─────────────────────────────────────────────────────────────
# The os2api/ DOS-call bridge (DosOpen, DosRead, KernVMAlloc, ...),
# generated as a real OS/2 LX DLL via add_lx_dll() above, exporting the
# full public API declared in os2api/os2_api.h. It needs its own copy of
# the VFS/blkdev/ext4/btrfs stack it bridges onto (self-contained, same
# reasoning as CMD.EXE's own private copy - see cmd_main.c's header
# comment) and initializes it lazily, on whichever Dos* call touches
# storage first (see os2_api_ensure_init() in os2_api.c) - real OS/2
# programs never had to initialize DOSCALLS.DLL themselves either.
set(DOSCALLS_DLL_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/os2api/os2_api.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/vfs.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/blkdev.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/gpt.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/fat32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/string.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/kprintf.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/kio.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/uart.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/mem.c
        ${CMAKE_CURRENT_SOURCE_DIR}/fs/ext4/ext4.c
        ${CMAKE_CURRENT_SOURCE_DIR}/fs/ext4/ext4_write.c
        ${CMAKE_CURRENT_SOURCE_DIR}/fs/btrfs/btrfs.c
        ${CMAKE_CURRENT_SOURCE_DIR}/fs/btrfs/btrfs_write.c
)

add_lx_dll(DOSCALLS
        SOURCES ${DOSCALLS_DLL_SOURCES}
        EXPORTS
        DosOpen DosClose DosRead DosWrite DosChgFilePtr
        DosFindFirst DosFindNext DosFindClose
        DosQueryPathInfo DosDelete DosMove DosCreateDir DosDeleteDir
        DosSetCurrentDir DosQueryCurrentDir DosResolvePath
        vfs_to_os2_error os2_to_vfs_attrs vfs_to_os2_attrs
        KernThunkStackTo16 KernThunkStackTo32 KernThunkStackTo64
        KernSerialize16BitDD KernUnSerialize16BitDD
        KernCopyIn KernCopyOut
        KernVMAlloc KernVMFree KernVMLock KernVMUnlock
        KernLinToPageList KernVMSetMem KernSelToFlat KernDynamicAPI
        KernRASSysTrace
)

# ── DOSCALLS.DLL embedding ───────────────────────────────────────────────────
# CMD.EXE and KAYTEVM.EXE below don't statically link os2_api.c anymore -
# instead they embed a build-time copy of DOSCALLS.DLL (this .incbin
# pattern mirrors "Embed HELLO.EXE..." further down for the kernel's own
# LX-loader self-test) and load it via kernel/src/lx_dll_loader.c at
# startup (os2api/os2_api_import.c), resolving DosOpen/DosRead/... by
# name against its real exports - see os2api/os2_api_import.h.
set(DOSCALLS_DLL_PATH ${CMAKE_CURRENT_BINARY_DIR}/DOSCALLS.DLL)
set(DOSCALLS_EMBED_S ${CMAKE_CURRENT_BINARY_DIR}/doscalls_embed.S)
set(DOSCALLS_EMBED_TEMPLATE ${CMAKE_CURRENT_BINARY_DIR}/doscalls_embed.configured.S)
configure_file(${CMAKE_SOURCE_DIR}/os2api/doscalls_embed.S.in ${DOSCALLS_EMBED_TEMPLATE} @ONLY)

# configure_file() only substitutes @DOSCALLS_DLL_PATH@ once, at configure
# time - it doesn't track DOSCALLS.DLL's actual byte content, so it alone
# can't make the .incbin'd payload staleness-tracked (same issue/fix as
# lx_test_payload.S below). A real add_custom_command OUTPUT, depending on
# DOSCALLS_DLL_PATH, gives a genuine dependency edge: rebuilding
# DOSCALLS.DLL re-copies this file, which cascades into reassembling and
# relinking CMD.EXE/KAYTEVM.EXE.
add_custom_command(
        OUTPUT ${DOSCALLS_EMBED_S}
        COMMAND ${CMAKE_COMMAND} -E copy ${DOSCALLS_EMBED_TEMPLATE} ${DOSCALLS_EMBED_S}
        DEPENDS ${DOSCALLS_EMBED_TEMPLATE} ${DOSCALLS_DLL_PATH}
        COMMENT "Refreshing doscalls_embed.S (tracks DOSCALLS.DLL)"
)

# ── CMD.EXE (standalone DOS command interpreter) ──────────────────────────────
# Same dos_shell_dispatch() the in-kernel shell uses (kernel/src/main.c),
# built as its own flat freestanding ELF with this project's normal
# aarch64-none-elf clang/lld cross-toolchain (real production code, so this
# uses clang, not the immature KCC pipeline above), linked at the LX app
# base address (lx/lx_app.ld), and wrapped as a genuine OS/2 LX CMD.EXE via
# makeexe — loadable by the kernel's built-in LX loader (RUN CMD.EXE).
set(CMD_EXE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/dos/cmd_main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/dos/dos_commands.c
        ${CMAKE_CURRENT_SOURCE_DIR}/os2api/os2_api_import.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/lx_dll_loader.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/string.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/kprintf.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/kio.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/uart.c
        ${DOSCALLS_EMBED_S}
)

set(CMD_EXE_ELF ${CMAKE_CURRENT_BINARY_DIR}/CMD.elf)
set(CMD_EXE_CODE_BIN ${CMAKE_CURRENT_BINARY_DIR}/CMD.code.bin)
set(CMD_EXE_DATA_BIN ${CMAKE_CURRENT_BINARY_DIR}/CMD.data.bin)
set(CMD_EXE_FILE ${CMAKE_CURRENT_BINARY_DIR}/CMD.EXE)

add_custom_command(
        OUTPUT ${CMD_EXE_FILE}
        COMMAND ${CMAKE_C_COMPILER} ${ARCH_FLAGS}
        -fno-stack-protector
        -DOS2_API_USE_IMPORT_TABLE
        -I${CMAKE_CURRENT_SOURCE_DIR}/kernel/include
        -I${CMAKE_CURRENT_SOURCE_DIR}/fs/ext4
        -I${CMAKE_CURRENT_SOURCE_DIR}/fs/btrfs
        -I${CMAKE_CURRENT_SOURCE_DIR}/os2api
        -I${CMAKE_CURRENT_SOURCE_DIR}/dos
        ${LX_LINKER_FLAGS} -T ${LX_APP_LD}
        -o ${CMD_EXE_ELF}
        ${CMD_EXE_SOURCES}
        COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.text --only-section=.rodata ${CMD_EXE_ELF} ${CMD_EXE_CODE_BIN}
        COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.data --only-section=.bss ${CMD_EXE_ELF} ${CMD_EXE_DATA_BIN}
        COMMAND bash -c "\"${MAKEEXE}\" \"${CMD_EXE_CODE_BIN}\" \"${CMD_EXE_DATA_BIN}\" \"${CMD_EXE_FILE}\" $(\"${Python3_EXECUTABLE}\" \"${LX_ENTRY_OFFSET_SCRIPT}\" \"${CMD_EXE_ELF}\" main ${LX_LOAD_BASE_HEX} \"${LLVM_NM_EXECUTABLE}\")"
        DEPENDS ${CMD_EXE_SOURCES} ${LX_APP_LD}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Building standalone CMD.EXE (OS/2 LX) from dos/ command set"
        VERBATIM
)
add_custom_target(cmd_exe ALL DEPENDS ${CMD_EXE_FILE})
add_dependencies(cmd_exe lx_host_tools DOSCALLS_lxdll)

# ── KAYTEVM.EXE (Kayte bytecode interpreter) ────────────────────────────────
# kayte/kaytevm_main.c executes .bytecode files produced by the kayte host
# tool (kayte --compile, built above by kayte/CMakeLists.txt) - see that
# file's header comment for the on-disk format and opcode set. Built the
# same way as CMD.EXE just above: this project's own freestanding
# aarch64-none-elf clang/lld cross-toolchain, linked at the LX app base
# address, wrapped as a genuine OS/2 LX KAYTEVM.EXE via makeexe.
set(KAYTEVM_EXE_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/kayte/kaytevm_main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/dos/dos_commands.c
        ${CMAKE_CURRENT_SOURCE_DIR}/os2api/os2_api_import.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/lx_dll_loader.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/string.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/kprintf.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/kio.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/uart.c
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/src/mem.c
        ${DOSCALLS_EMBED_S}
)

set(KAYTEVM_EXE_ELF ${CMAKE_CURRENT_BINARY_DIR}/KAYTEVM.elf)
set(KAYTEVM_EXE_CODE_BIN ${CMAKE_CURRENT_BINARY_DIR}/KAYTEVM.code.bin)
set(KAYTEVM_EXE_DATA_BIN ${CMAKE_CURRENT_BINARY_DIR}/KAYTEVM.data.bin)
set(KAYTEVM_EXE_FILE ${CMAKE_CURRENT_BINARY_DIR}/KAYTEVM.EXE)

add_custom_command(
        OUTPUT ${KAYTEVM_EXE_FILE}
        COMMAND ${CMAKE_C_COMPILER} ${ARCH_FLAGS}
        -fno-stack-protector
        -DOS2_API_USE_IMPORT_TABLE
        -I${CMAKE_CURRENT_SOURCE_DIR}/kernel/include
        -I${CMAKE_CURRENT_SOURCE_DIR}/fs/ext4
        -I${CMAKE_CURRENT_SOURCE_DIR}/fs/btrfs
        -I${CMAKE_CURRENT_SOURCE_DIR}/os2api
        -I${CMAKE_CURRENT_SOURCE_DIR}/dos
        ${LX_LINKER_FLAGS} -T ${LX_APP_LD}
        -o ${KAYTEVM_EXE_ELF}
        ${KAYTEVM_EXE_SOURCES}
        COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.text --only-section=.rodata ${KAYTEVM_EXE_ELF} ${KAYTEVM_EXE_CODE_BIN}
        COMMAND ${CMAKE_OBJCOPY} -O binary --only-section=.data --only-section=.bss ${KAYTEVM_EXE_ELF} ${KAYTEVM_EXE_DATA_BIN}
        COMMAND bash -c "\"${MAKEEXE}\" \"${KAYTEVM_EXE_CODE_BIN}\" \"${KAYTEVM_EXE_DATA_BIN}\" \"${KAYTEVM_EXE_FILE}\" $(\"${Python3_EXECUTABLE}\" \"${LX_ENTRY_OFFSET_SCRIPT}\" \"${KAYTEVM_EXE_ELF}\" main ${LX_LOAD_BASE_HEX} \"${LLVM_NM_EXECUTABLE}\")"
        DEPENDS ${KAYTEVM_EXE_SOURCES} ${LX_APP_LD}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Building standalone KAYTEVM.EXE (OS/2 LX) - Kayte bytecode interpreter"
        VERBATIM
)
add_custom_target(kaytevm_exe ALL DEPENDS ${KAYTEVM_EXE_FILE})
add_dependencies(kaytevm_exe lx_host_tools DOSCALLS_lxdll)

endif()
# ═════════════════════════════════════════════════════════════════════════════
# End AArch64-only section
# ═════════════════════════════════════════════════════════════════════════════

# ── Final kernel image ────────────────────────────────────────────────────────
if(TARGET_ARCH STREQUAL "arm" OR TARGET_ARCH STREQUAL "riscv64")
    # BeagleBone Black / BeagleV-Ahead: a raw image carrying a Linux-style
    # boot header (zImage for bootz, RISC-V Image for booti - see
    # bbb_boot.S / beaglev_boot.S), plus a uEnv.txt that loads and boots it
    # from the SD card's first (FAT) partition.
    add_custom_target(${KERNEL_IMAGE} ALL
            COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:os2warp_kernel>
            ${CMAKE_BINARY_DIR}/${KERNEL_IMAGE}
            DEPENDS os2warp_kernel
            COMMENT "Building ${KERNEL_IMAGE} for ${PLATFORM_NAME}"
    )

    add_custom_target(kernel-image DEPENDS ${KERNEL_IMAGE})

    if(TARGET_PLATFORM STREQUAL "QEMU_RISCV")
        install(FILES ${CMAKE_BINARY_DIR}/${KERNEL_IMAGE}
                DESTINATION .
                PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
        )
    else()
        if(TARGET_PLATFORM STREQUAL "BBB")
            set(UENV_TEMPLATE ${CMAKE_SOURCE_DIR}/bootloader/bbb_uEnv.txt.in)
        else()
            set(UENV_TEMPLATE ${CMAKE_SOURCE_DIR}/bootloader/beaglev_ahead_uEnv.txt.in)
        endif()
        configure_file(${UENV_TEMPLATE} ${CMAKE_BINARY_DIR}/uEnv.txt @ONLY)

        install(FILES ${CMAKE_BINARY_DIR}/${KERNEL_IMAGE} ${CMAKE_BINARY_DIR}/uEnv.txt
                DESTINATION .
                PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
        )
    endif()

elseif(TARGET_PLATFORM STREQUAL "RPI3")
    # Raspberry Pi: kernel8.img
    add_custom_target(kernel8.img ALL
            COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:os2warp_kernel>
            ${CMAKE_BINARY_DIR}/kernel8.img
            DEPENDS os2warp_kernel
            COMMENT "Building kernel8.img for Raspberry Pi 3+"
    )

    # Alias for consistency
    add_custom_target(kernel-image DEPENDS kernel8.img)

    # Installation
    install(FILES ${CMAKE_BINARY_DIR}/kernel8.img
            DESTINATION .
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    )

else()
    # QEMU: os2warp.img
    add_custom_target(os2warp.img ALL
            COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:os2warp_kernel>
            ${CMAKE_BINARY_DIR}/os2warp.img
            DEPENDS os2warp_kernel
            COMMENT "Building os2warp.img for QEMU"
    )

    # Alias for consistency
    add_custom_target(kernel-image DEPENDS os2warp.img)

    # Installation
    install(FILES ${CMAKE_BINARY_DIR}/os2warp.img
            DESTINATION .
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    )
endif()

# ── QEMU Targets (only for QEMU platform) ─────────────────────────────────────
if(TARGET_PLATFORM STREQUAL "QEMU")
    find_program(QEMU_EXECUTABLE
            NAMES qemu-system-aarch64
            PATHS
            /usr/bin
            /usr/local/bin
            /opt/homebrew/bin
            /opt/local/bin
    )

    if(QEMU_EXECUTABLE)
        message(STATUS "QEMU: ${QEMU_EXECUTABLE}")
    else()
        message(WARNING "QEMU not found - 'make run' and 'make debug' targets will not work")
        if(HOST_PLATFORM STREQUAL "macOS")
            message(STATUS "Install with: brew install qemu")
        elseif(HOST_PLATFORM STREQUAL "Linux")
            message(STATUS "Install with: sudo apt install qemu-system-arm (Ubuntu/Debian)")
            message(STATUS "              sudo dnf install qemu-system-aarch64 (Fedora/RHEL)")
            message(STATUS "              sudo pacman -S qemu-system-aarch64 (Arch)")
        endif()
    endif()

    # ── Find UEFI firmware ────────────────────────────────────────────────────
    set(UEFI_FIRMWARE_PATHS
            # Linux paths
            /usr/share/AAVMF/AAVMF_CODE.fd
            /usr/share/qemu-efi-aarch64/QEMU_EFI.fd
            /usr/share/edk2/aarch64/QEMU_EFI.fd
            /usr/share/edk2-arm/aarch64/QEMU_EFI.fd
            /usr/share/edk2-armvirt/aarch64/QEMU_EFI.fd
            # macOS Homebrew paths (Apple Silicon)
            /opt/homebrew/share/qemu/edk2-aarch64-code.fd
            /opt/homebrew/Cellar/qemu/*/share/qemu/edk2-aarch64-code.fd
            # macOS Homebrew paths (Intel)
            /usr/local/share/qemu/edk2-aarch64-code.fd
            /usr/local/Cellar/qemu/*/share/qemu/edk2-aarch64-code.fd
    )

    set(UEFI_FIRMWARE "")
    foreach(UEFI_PATH ${UEFI_FIRMWARE_PATHS})
        # Handle glob patterns
        file(GLOB UEFI_FILES ${UEFI_PATH})
        if(UEFI_FILES)
            list(GET UEFI_FILES 0 UEFI_FIRMWARE)
            break()
        endif()
    endforeach()

    if(UEFI_FIRMWARE)
        message(STATUS "UEFI firmware: ${UEFI_FIRMWARE}")
    else()
        message(STATUS "UEFI firmware: not found (direct kernel boot only)")
    endif()

    # Detect host architecture
    execute_process(
            COMMAND uname -m
            OUTPUT_VARIABLE HOST_ARCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Set QEMU acceleration based on host
    if(HOST_PLATFORM STREQUAL "macOS")
        if(HOST_ARCH MATCHES "arm64")
            set(QEMU_ACCEL -accel hvf)
            set(QEMU_CPU "max")
        else()
            set(QEMU_ACCEL -accel hvf)
            set(QEMU_CPU "cortex-a72")
        endif()
    elseif(HOST_PLATFORM STREQUAL "Linux")
        set(QEMU_ACCEL -accel kvm)
        set(QEMU_CPU "max")
    else()
        set(QEMU_ACCEL -accel tcg)
        set(QEMU_CPU "cortex-a72")
    endif()

    message(STATUS "Host architecture: ${HOST_ARCH}")
    message(STATUS "CPU model: ${QEMU_CPU}")
    message(STATUS "Acceleration: ${QEMU_ACCEL}")

    if(QEMU_EXECUTABLE)
        # Run kernel directly (NO UEFI - Fast boot), with the real BOOT
        # (FAT) + C: (btrfs/ext4) test disk injected at blkdev.c's
        # PRELOADED_DISK_ADDR (see run-testdisk below) - without it disk0
        # is a zeroed RAM window and no BOOT/C:/CONFIG.SYS is found.
        add_custom_target(run
                COMMAND ${QEMU_EXECUTABLE}
                -machine virt,gic-version=3
                -cpu ${QEMU_CPU}
                ${QEMU_ACCEL}
                -m 2G
                -smp 4
                -kernel ${CMAKE_BINARY_DIR}/os2warp.img
                -device loader,file=${CMAKE_BINARY_DIR}/testdisk.img,addr=0x44000000,force-raw=on
                -serial mon:stdio
                -display none
                -nographic
                DEPENDS os2warp.img mktestdisk
                COMMENT "Running OS/2 kernel in QEMU with the real test disk"
        )

        # Same as run, but with EL2 present (virtualization=on), so the
        # kernel's hypervisor (kernel/src/hyp.c) is usable - type VM TEST.
        # Always TCG: HVF can't give a guest EL2 on this machine class, and
        # cortex-a72 keeps EL2 plain ARMv8.0 (no VHE) like real hardware
        # this is aimed at.
        add_custom_target(run-vm
                COMMAND ${QEMU_EXECUTABLE}
                -machine virt,gic-version=3,virtualization=on
                -cpu cortex-a72
                -accel tcg
                -m 2G
                -smp 1
                -kernel ${CMAKE_BINARY_DIR}/os2warp.img
                -device loader,file=${CMAKE_BINARY_DIR}/testdisk.img,addr=0x44000000,force-raw=on
                -serial mon:stdio
                -display none
                -nographic
                DEPENDS os2warp.img mktestdisk
                COMMENT "Running OS/2 kernel in QEMU with EL2 (type VM TEST to boot the hello guest)"
        )

        # Run with a real display (Cocoa window on macOS), a ramfb device
        # so the framebuffer/Wayland-style compositor GUI (type GUI at the
        # prompt) is actually visible, and virtio-keyboard/virtio-tablet
        # so real key/pointer events from that window - Cmd+Escape (cycles
        # the 4 workplaces) and a visible, clickable mouse cursor - reach
        # the guest. -global virtio-mmio.force-legacy=false is required:
        # the "virt" machine defaults virtio-mmio to legacy (v1) transport,
        # which kernel/src/gui/virtio_input.c (correctly) doesn't speak.
        # See kernel/src/gui/ramfb.c and kernel/src/gui/virtio_input.c.
        #
        # Also injects the real BOOT (FAT) + C: (btrfs/ext4) test disk at
        # blkdev.c's PRELOADED_DISK_ADDR (see run-testdisk below) so the
        # GUI session mounts a real filesystem instead of failing with
        # "Bad superblock magic" against the zeroed ramdisk window.
        add_custom_target(run-gui
                COMMAND ${QEMU_EXECUTABLE}
                -machine virt,gic-version=3
                -cpu ${QEMU_CPU}
                ${QEMU_ACCEL}
                -m 2G
                -smp 4
                -global virtio-mmio.force-legacy=false
                -kernel ${CMAKE_BINARY_DIR}/os2warp.img
                -device loader,file=${CMAKE_BINARY_DIR}/testdisk.img,addr=0x44000000,force-raw=on
                -device virtio-gpu-device
                -device ramfb
                -device virtio-keyboard-device
                -device virtio-tablet-device
                -serial mon:stdio
                -display cocoa
                DEPENDS os2warp.img mktestdisk
                COMMENT "Running OS/2 kernel in QEMU with a real display and real test disk (type GUI; Cmd+Escape switches workplaces, mouse is live)"
        )

        # Run kernel directly with disk image
        add_custom_target(run-direct
                COMMAND ${QEMU_EXECUTABLE}
                -machine virt,gic-version=3
                -cpu ${QEMU_CPU}
                -m 2G
                -smp 4
                -kernel ${CMAKE_BINARY_DIR}/os2warp.img
                -drive if=none,id=hd0,file=${CMAKE_SOURCE_DIR}/os2warp452.img,format=raw
                -device virtio-blk-pci,drive=hd0
                -serial mon:stdio
                -display none
                -device ramfb
                -nographic
                DEPENDS os2warp.img
                COMMENT "Running OS/2 kernel with disk image"
        )

        # Partition 2 (C:, mounted at /) filesystem for the test disk -
        # BOOT is always FAT16.
        set(TESTDISK_C_FS "btrfs" CACHE STRING "Test disk C: filesystem (btrfs or ext4)")
        set_property(CACHE TESTDISK_C_FS PROPERTY STRINGS btrfs ext4)

        # Build the small real test disk (scripts/mktestdisk.sh) —
        # needed because blkdev.c has no real VirtIO block driver yet, only
        # a fixed-physical-address "preloaded disk" window that QEMU's
        # -device loader can inject a real image into (see run-testdisk).
        add_custom_target(mktestdisk
                COMMAND ${CMAKE_COMMAND} -E env C_FS=${TESTDISK_C_FS}
                        ${CMAKE_SOURCE_DIR}/scripts/mktestdisk.sh ${CMAKE_BINARY_DIR}/testdisk.img
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Building GPT test disk: BOOT (FAT16) + C: (${TESTDISK_C_FS})"
        )
        if(TARGET cmd_exe)
            add_dependencies(mktestdisk cmd_exe)
        endif()
        if(TARGET HELLO_lxexe)
            add_dependencies(mktestdisk HELLO_lxexe)
        endif()
        if(TARGET kaytevm_exe)
            add_dependencies(mktestdisk kaytevm_exe)
        endif()
        if(TARGET kayte_host_tool)
            add_dependencies(mktestdisk kayte_host_tool)
        endif()

        # Run with the test disk injected at blkdev.c's preloaded address —
        # makes DIR/TYPE/RUN <file>.EXE work against real files.
        add_custom_target(run-testdisk
                COMMAND ${QEMU_EXECUTABLE}
                -machine virt,gic-version=3
                -cpu ${QEMU_CPU}
                ${QEMU_ACCEL}
                -m 2G
                -smp 1
                -kernel ${CMAKE_BINARY_DIR}/os2warp.img
                -device loader,file=${CMAKE_BINARY_DIR}/testdisk.img,addr=0x44000000,force-raw=on
                -serial mon:stdio
                -display none
                -nographic
                DEPENDS os2warp.img mktestdisk
                COMMENT "Running OS/2 kernel in QEMU with the real ext4 test disk"
        )

        # Debug with GDB/LLDB
        add_custom_target(debug
                COMMAND ${QEMU_EXECUTABLE}
                -machine virt,gic-version=3
                -cpu ${QEMU_CPU}
                ${QEMU_ACCEL}
                -m 2G
                -kernel ${CMAKE_BINARY_DIR}/os2warp.img
                -serial stdio
                -monitor none
                -nographic
                -s -S
                DEPENDS os2warp.img
                COMMENT "Debug mode - waiting for GDB/LLDB on :1234"
        )

        # Debug with disk
        add_custom_target(debug-direct
                COMMAND ${QEMU_EXECUTABLE}
                -machine virt,gic-version=3
                -cpu ${QEMU_CPU}
                ${QEMU_ACCEL}
                -m 2G
                -kernel ${CMAKE_BINARY_DIR}/os2warp.img
                -drive if=none,id=hd0,file=${CMAKE_SOURCE_DIR}/os2warp452.img,format=raw
                -device virtio-blk-pci,drive=hd0
                -serial stdio
                -monitor none
                -nographic
                -s -S
                DEPENDS os2warp.img
                COMMENT "Debug mode with disk - waiting for GDB/LLDB on :1234"
        )
    endif()

    # ── Helper targets for QEMU ───────────────────────────────────────────────
    # Create disk image (if mkdisk.sh exists)
    if(EXISTS ${CMAKE_SOURCE_DIR}/scripts/mkdisk.sh)
        add_custom_target(mkdisk
                COMMAND ${CMAKE_SOURCE_DIR}/scripts/mkdisk.sh ${CMAKE_SOURCE_DIR}/os2warp452.img
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Creating disk image with mkdisk.sh"
        )
    endif()

    # Install CONFIG.SYS to disk image
    if(EXISTS ${CMAKE_SOURCE_DIR}/scripts/install_config.sh AND EXISTS ${CMAKE_SOURCE_DIR}/CONFIG.SYS)
        add_custom_target(install-config
                COMMAND ${CMAKE_SOURCE_DIR}/scripts/install_config.sh
                ${CMAKE_SOURCE_DIR}/os2warp452.img
                ${CMAKE_SOURCE_DIR}/CONFIG.SYS
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Installing CONFIG.SYS to disk image"
        )

        # Combined target: create disk and install config
        add_custom_target(setup-disk
                COMMAND ${CMAKE_SOURCE_DIR}/scripts/mkdisk.sh ${CMAKE_SOURCE_DIR}/os2warp452.img
                COMMAND ${CMAKE_SOURCE_DIR}/scripts/install_config.sh
                ${CMAKE_SOURCE_DIR}/os2warp452.img
                ${CMAKE_SOURCE_DIR}/CONFIG.SYS
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                COMMENT "Creating disk image and installing CONFIG.SYS"
        )
    endif()

    # ── ISO Building (QEMU only) ──────────────────────────────────────────────
    find_program(XORRISO_EXECUTABLE
            NAMES xorriso
            PATHS /usr/bin /usr/local/bin /opt/homebrew/bin
    )

    if(XORRISO_EXECUTABLE)
        set(ISO_OUTPUT ${CMAKE_BINARY_DIR}/os2warp-arm64.iso)

        # Copy build script to build directory if it exists
        if(EXISTS ${CMAKE_SOURCE_DIR}/scripts/build_iso.sh)
            configure_file(
                    ${CMAKE_SOURCE_DIR}/scripts/build_iso.sh
                    ${CMAKE_BINARY_DIR}/build_iso.sh
                    COPYONLY
            )

            # Create ISO build target using the script
            add_custom_target(iso
                    COMMAND chmod +x ${CMAKE_BINARY_DIR}/build_iso.sh
                    COMMAND ${CMAKE_BINARY_DIR}/build_iso.sh ${CMAKE_BINARY_DIR}
                    DEPENDS os2warp.img
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                    COMMENT "Building bootable ISO image"
            )

            message(STATUS "ISO building enabled: 'make iso' will create ${ISO_OUTPUT}")
        endif()
    else()
        message(STATUS "ISO building disabled - install xorriso: brew install xorriso")
    endif()

endif() # End QEMU-only section

# ── QEMU RISC-V Targets (only for QEMU_RISCV platform) ────────────────────────
if(TARGET_PLATFORM STREQUAL "QEMU_RISCV")
    find_program(QEMU_RISCV_EXECUTABLE
            NAMES qemu-system-riscv64
            PATHS /usr/bin /usr/local/bin /opt/homebrew/bin /opt/local/bin
    )
    if(NOT QEMU_RISCV_EXECUTABLE)
        message(WARNING "qemu-system-riscv64 not found - 'make run' will not work")
        if(HOST_PLATFORM STREQUAL "macOS")
            message(STATUS "Install with: brew install qemu")
        else()
            message(STATUS "Install with: sudo apt install qemu-system-misc (Ubuntu/Debian)")
        endif()
    endif()

    # A small MBR test card (scripts/mksdcard.py): a FAT32 boot partition
    # (C:) with CONFIG.SYS, README.TXT and a subdirectory, plus an ext4
    # partition when mke2fs is available. Only built when missing, so files
    # the kernel writes to it survive between runs (delete it to reset).
    set(SDCARD_IMAGE ${CMAKE_BINARY_DIR}/sdcard.img)
    find_program(MKE2FS_EXECUTABLE
            NAMES mke2fs mkfs.ext4
            PATHS /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin /sbin /usr/sbin
    )
    if(MKE2FS_EXECUTABLE)
        set(MKSDCARD_EXT4_ARGS --mke2fs ${MKE2FS_EXECUTABLE})
    else()
        set(MKSDCARD_EXT4_ARGS "")
        message(STATUS "mke2fs not found - the QEMU_RISCV test SD card gets a FAT32 partition only")
    endif()
    add_custom_command(
            OUTPUT ${SDCARD_IMAGE}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mksdcard.py
                    ${MKSDCARD_EXT4_ARGS} ${SDCARD_IMAGE}
            DEPENDS ${CMAKE_SOURCE_DIR}/scripts/mksdcard.py
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Building QEMU RISC-V test SD card (sdcard.img)"
    )
    add_custom_target(mksdcard DEPENDS ${SDCARD_IMAGE})

    if(QEMU_RISCV_EXECUTABLE)
        set(QEMU_RISCV_ARGS
                -machine virt
                -smp 4
                -m 512M
                -bios default
                -kernel ${CMAKE_BINARY_DIR}/${KERNEL_IMAGE}
                -device sdhci-pci
                -nographic
                -serial mon:stdio
        )
        set(QEMU_RISCV_SD_ARGS
                -drive if=none,id=sd0,file=${SDCARD_IMAGE},format=raw
                -device sd-card,drive=sd0
        )

        # Boot with the test SD card - DIR/TYPE/CD then work on real files.
        # Ctrl-A X quits QEMU.
        add_custom_target(run
                COMMAND ${QEMU_RISCV_EXECUTABLE} ${QEMU_RISCV_ARGS} ${QEMU_RISCV_SD_ARGS}
                DEPENDS ${KERNEL_IMAGE} ${SDCARD_IMAGE}
                USES_TERMINAL
                COMMENT "Running OS/2 kernel in QEMU riscv64 virt with the test SD card (Ctrl-A X quits)"
        )

        # Same, with an empty SD slot.
        add_custom_target(run-nocard
                COMMAND ${QEMU_RISCV_EXECUTABLE} ${QEMU_RISCV_ARGS}
                DEPENDS ${KERNEL_IMAGE}
                USES_TERMINAL
                COMMENT "Running OS/2 kernel in QEMU riscv64 virt without an SD card"
        )

        # Paused at reset, waiting for GDB/LLDB on :1234, e.g.
        #   lldb build-qemu-riscv/kernel/os2warp_kernel -o 'gdb-remote 1234'
        add_custom_target(debug
                COMMAND ${QEMU_RISCV_EXECUTABLE} ${QEMU_RISCV_ARGS} ${QEMU_RISCV_SD_ARGS} -s -S
                DEPENDS ${KERNEL_IMAGE} ${SDCARD_IMAGE}
                USES_TERMINAL
                COMMENT "Debug mode - waiting for GDB/LLDB on :1234"
        )
    endif()
endif()

# ── Raspberry Pi Targets (only for RPI3 platform) ─────────────────────────────
if(TARGET_PLATFORM STREQUAL "RPI3")
    # SD card preparation script
    if(EXISTS ${CMAKE_SOURCE_DIR}/scripts/create_sdcard.sh)
        configure_file(
                ${CMAKE_SOURCE_DIR}/scripts/create_sdcard.sh
                ${CMAKE_BINARY_DIR}/create_sdcard.sh
                COPYONLY
        )

        add_custom_target(sdcard
                COMMAND chmod +x ${CMAKE_BINARY_DIR}/create_sdcard.sh
                COMMAND ${CMAKE_BINARY_DIR}/create_sdcard.sh ${CMAKE_BINARY_DIR}
                DEPENDS kernel8.img
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Preparing SD card files for Raspberry Pi 3+"
        )

        message(STATUS "")
        message(STATUS "Raspberry Pi targets:")
        message(STATUS "  make            - Build kernel8.img")
        message(STATUS "  make sdcard     - Prepare SD card files")
        message(STATUS "")
    else()
        message(STATUS "")
        message(STATUS "Note: scripts/create_sdcard.sh not found")
        message(STATUS "SD card preparation target not available")
        message(STATUS "")
    endif()
endif()

# ── Configuration Summary ─────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "OS/2 Warp 4.52 ARM64 — Build Configuration")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "Platform:        ${PLATFORM_NAME}")
message(STATUS "Host:            ${HOST_PLATFORM}")
if(USE_KCC)
    message(STATUS "Compiler:        KCC (Kayte C Compiler) for aarch64")
elseif(USING_CLANG_TOOLCHAIN)
    message(STATUS "Compiler:        clang -target ${ARCH_TRIPLE}")
else()
    message(STATUS "Compiler:        ${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER})")
endif()
message(STATUS "Load Address:    ${KERNEL_LOAD_ADDR}")
message(STATUS "Heap Size:       ${HEAP_SIZE}")
message(STATUS "Output:          ${KERNEL_IMAGE}")
message(STATUS "═══════════════════════════════════════════════════════════")

if(TARGET_PLATFORM STREQUAL "QEMU")
    message(STATUS "")
    message(STATUS "QEMU targets available:")
    message(STATUS "  make run        - Run in QEMU")
    message(STATUS "  make run-direct - Run with disk image")
    message(STATUS "  make debug      - Debug mode (GDB :1234)")
    message(STATUS "")
    message(STATUS "Compiler options:")
    if(USE_KCC)
        message(STATUS "  Currently using: KCC")
        message(STATUS "  Switch to clang: cmake -DUSE_KCC=OFF ..")
    elseif(USING_CLANG_TOOLCHAIN)
        message(STATUS "  Currently using: clang (default)")
        message(STATUS "  Switch to KCC:   cmake -DUSE_KCC=ON ..")
        message(STATUS "  Switch to GCC:   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm64-baremetal-toolchain.cmake ..")
    else()
        message(STATUS "  Currently using: ${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER})")
        message(STATUS "  Switch to clang: cmake --fresh -B ${CMAKE_BINARY_DIR} ..")
    endif()

    message(STATUS "")
    message(STATUS "To switch to Raspberry Pi:")
    message(STATUS "  cmake -DTARGET_PLATFORM=RPI3 ..")
elseif(TARGET_PLATFORM STREQUAL "RPI3")
    message(STATUS "")
    message(STATUS "Raspberry Pi targets available:")
    message(STATUS "  make            - Build kernel8.img")
    if(EXISTS ${CMAKE_SOURCE_DIR}/scripts/create_sdcard.sh)
        message(STATUS "  make sdcard     - Prepare SD card files")
    endif()
    message(STATUS "")
    message(STATUS "To switch to QEMU:")
    message(STATUS "  cmake -DTARGET_PLATFORM=QEMU ..")
elseif(TARGET_PLATFORM STREQUAL "QEMU_RISCV")
    message(STATUS "")
    message(STATUS "QEMU RISC-V targets available:")
    message(STATUS "  make run        - Run in QEMU with the test SD card (Ctrl-A X quits)")
    message(STATUS "  make run-nocard - Run with an empty SD slot")
    message(STATUS "  make debug      - Debug mode (GDB :1234)")
    message(STATUS "  make mksdcard   - (Re)build sdcard.img if missing")
    message(STATUS "")
    message(STATUS "To switch platform: cmake -DTARGET_PLATFORM=QEMU|RPI3|BBB|BEAGLEV_AHEAD|QEMU_RISCV ..")
else()
    message(STATUS "")
    message(STATUS "${PLATFORM_NAME} targets available:")
    message(STATUS "  make            - Build ${KERNEL_IMAGE} and uEnv.txt")
    message(STATUS "")
    message(STATUS "Copy both to the SD card's first (FAT) partition.")
    message(STATUS "To switch platform: cmake -DTARGET_PLATFORM=QEMU|RPI3|BBB|BEAGLEV_AHEAD|QEMU_RISCV ..")
endif()

message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS "")