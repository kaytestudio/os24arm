# ============================================================================
# arm64-toolchain.cmake - Example ARM64 Cross-Compilation Toolchain
# ============================================================================

# Target system
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross-compilation tools
# Install one of these toolchains and set TOOLCHAIN_PREFIX below:
#   - macOS: brew install aarch64-elf-gcc
#   - Linux: apt-get install gcc-aarch64-linux-gnu
#   - Or use a custom toolchain path

# Option 1: Generic ARM64 ELF (bare-metal)
set(TOOLCHAIN_PREFIX aarch64-elf-)

# Option 2: ARM64 Linux GNU
# set(TOOLCHAIN_PREFIX aarch64-linux-gnu-)

# Option 3: Custom toolchain location
# set(TOOLCHAIN_ROOT /opt/arm-toolchain)
# set(TOOLCHAIN_PREFIX ${TOOLCHAIN_ROOT}/bin/aarch64-none-elf-)

# Compilers
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)

# Utilities
set(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}objdump)
set(CMAKE_SIZE ${TOOLCHAIN_PREFIX}size)
set(CMAKE_AR ${TOOLCHAIN_PREFIX}ar)
set(CMAKE_RANLIB ${TOOLCHAIN_PREFIX}ranlib)

# Compiler flags for ARM64
set(CMAKE_C_FLAGS_INIT "-march=armv8-a")
set(CMAKE_CXX_FLAGS_INIT "-march=armv8-a")
set(CMAKE_ASM_FLAGS_INIT "-march=armv8-a")

# For OS/2-style bare metal
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -static")

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ============================================================================
# Usage:
#   mkdir build && cd build
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=../arm64-toolchain.cmake
#   make
# ============================================================================
