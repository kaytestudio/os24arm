# fs/hpfs/CMakeLists.txt - HPFS Filesystem Driver

add_library(hpfs STATIC hpfs.c)

target_include_directories(hpfs PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/kernel/include
)

target_compile_options(hpfs PRIVATE
        -Wall
        -Wextra
        -Werror=implicit-function-declaration
)

message(STATUS "HPFS filesystem driver configured")
