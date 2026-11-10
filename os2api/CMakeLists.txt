add_library(os2api STATIC
        os2_api.c
)

target_include_directories(os2api PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/kernel/include
)

target_compile_options(os2api PRIVATE
        -Wall
        -Wextra
        -Werror=implicit-function-declaration
)

# Linked directly into the kernel image (via rexx) - kernel_main() already
# calls vfs_init/blk_init/ext4_init/btrfs_init once, in this exact address
# space, before any shell/REXX code can run, so os2_api.c's lazy self-init
# (needed only by the standalone DOSCALLS.DLL build) would be redundant
# here. See the comment above os2_api_ensure_init() in os2_api.c.
target_compile_definitions(os2api PRIVATE OS2_API_SKIP_LAZY_INIT)

if(TARGET rexx)
    target_link_libraries(rexx PRIVATE os2api)
endif()