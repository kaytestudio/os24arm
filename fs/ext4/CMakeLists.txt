# fs/ext4/CMakeLists.txt - ext4 Filesystem Driver

# ext4 driver sources
set(EXT4_SOURCES
        ext4.c
        ext4_write.c
)

# ext4 driver headers
set(EXT4_HEADERS
        ext4.h
        ext4_internal.h
)

# Create ext4 driver library
add_library(ext4 STATIC ${EXT4_SOURCES})

# Include directories
target_include_directories(ext4 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/kernel/include
)

# Compiler flags for ext4
target_compile_options(ext4 PRIVATE
        -Wall
        -Wextra
        -Werror=implicit-function-declaration
)

# Install headers to kernel include directory (optional)
# This makes ext4.h available as #include "ext4.h"
install(FILES ${EXT4_HEADERS}
        DESTINATION ${CMAKE_SOURCE_DIR}/kernel/include/fs
)

message(STATUS "ext4 filesystem driver configured")