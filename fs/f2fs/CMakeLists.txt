# fs/f2fs/CMakeLists.txt - f2fs Filesystem Driver

# f2fs driver sources
set(F2FS_SOURCES
        f2fs.c
        f2fs_write.c
)

# f2fs driver headers
set(F2FS_HEADERS
        f2fs.h
        f2fs_disk.h
        f2fs_internal.h
)

# Create f2fs driver library
add_library(f2fs STATIC ${F2FS_SOURCES})

# Include directories
target_include_directories(f2fs PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/kernel/include
)

# Compiler flags for f2fs
target_compile_options(f2fs PRIVATE
        -Wall
        -Wextra
        -Werror=implicit-function-declaration
)

# Install headers to kernel include directory (optional)
install(FILES ${F2FS_HEADERS}
        DESTINATION ${CMAKE_SOURCE_DIR}/kernel/include/fs
)

message(STATUS "f2fs filesystem driver configured")
