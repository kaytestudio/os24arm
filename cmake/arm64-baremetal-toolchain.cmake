# ============================================================================
# arm64-gcc-toolchain.cmake - ARM64 GCC Cross-Compilation (macOS + Linux)
# Uses: aarch64-none-elf-gcc (or aarch64-elf-gcc)
# ============================================================================

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Try multiple GCC toolchain prefixes (in order of preference)
set(TOOLCHAIN_PREFIXES
        aarch64-none-elf-
        aarch64-elf-
        aarch64-linux-gnu-
        aarch64-unknown-linux-gnu-
)

# Search for the toolchain
foreach(PREFIX ${TOOLCHAIN_PREFIXES})
    find_program(TOOLCHAIN_GCC NAMES ${PREFIX}gcc)
    if(TOOLCHAIN_GCC)
        set(TOOLCHAIN_PREFIX ${PREFIX})
        get_filename_component(TOOLCHAIN_BIN_DIR ${TOOLCHAIN_GCC} DIRECTORY)
        message(STATUS "Found ARM64 GCC toolchain: ${PREFIX}")
        message(STATUS "  Location: ${TOOLCHAIN_BIN_DIR}")
        break()
    endif()
endforeach()

if(NOT TOOLCHAIN_GCC)
    message(FATAL_ERROR
            "\n"
            "════════════════════════════════════════════════════════════\n"
            "ARM64 GCC toolchain not found!\n"
            "════════════════════════════════════════════════════════════\n"
            "\n"
            "macOS: brew install aarch64-elf-gcc\n"
            "Linux: sudo apt install gcc-aarch64-linux-gnu\n"
            "\n"
            "════════════════════════════════════════════════════════════\n"
    )
endif()

# Set compilers with full paths
set(CMAKE_C_COMPILER ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}gcc CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}g++ CACHE FILEPATH "C++ compiler")
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}gcc CACHE FILEPATH "ASM compiler")

# Set binutils with full paths
set(CMAKE_OBJCOPY ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}objcopy CACHE FILEPATH "objcopy")
set(CMAKE_OBJDUMP ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}objdump CACHE FILEPATH "objdump")
set(CMAKE_SIZE ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}size CACHE FILEPATH "size")
set(CMAKE_AR ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}ar CACHE FILEPATH "ar")
set(CMAKE_RANLIB ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}ranlib CACHE FILEPATH "ranlib")
set(CMAKE_STRIP ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}strip CACHE FILEPATH "strip")

# Compiler flags for bare metal ARM64
# NOTE: GCC doesn't use -target (that's clang-only)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a72 -march=armv8-a")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a72 -march=armv8-a")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cortex-a72 -march=armv8-a")

# Bare metal - no standard library
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -static")

# Search for programs only in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Search for libraries and headers only in the target directories
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Don't try to compile test programs
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

message(STATUS "")
message(STATUS "════════════════════════════════════════════════════════════")
message(STATUS "ARM64 GCC Cross-Compilation Toolchain")
message(STATUS "════════════════════════════════════════════════════════════")
message(STATUS "Toolchain:  ${TOOLCHAIN_PREFIX}gcc")
message(STATUS "Location:   ${TOOLCHAIN_BIN_DIR}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "Objcopy:    ${CMAKE_OBJCOPY}")
message(STATUS "Target:     ${CMAKE_SYSTEM_PROCESSOR} bare metal")
message(STATUS "Flags:      -mcpu=cortex-a72 -march=armv8-a")
message(STATUS "")
message(STATUS "✅ Ready for macOS and Linux builds")
message(STATUS "════════════════════════════════════════════════════════════")
message(STATUS "")