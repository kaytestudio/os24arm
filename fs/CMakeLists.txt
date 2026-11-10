# fs/CMakeLists.txt - Filesystem Drivers

message(STATUS "Configuring filesystem drivers")

# ── Filesystem Drivers ────────────────────────────────────────────────────

# ext4 filesystem
add_subdirectory(ext4)

# btrfs filesystem
add_subdirectory(btrfs)

# f2fs filesystem
add_subdirectory(f2fs)

# FAT32 filesystem (if you want to move it here)
# add_subdirectory(fat32)

# HPFS filesystem (future)
# add_subdirectory(hpfs)

# NTFS filesystem (future)
# add_subdirectory(ntfs)

# ISO9660/CDFS filesystem (future)
# add_subdirectory(cdfs)

# Create combined filesystem library
add_library(filesystems INTERFACE)

# Link all filesystem drivers
target_link_libraries(filesystems INTERFACE
        ext4
        btrfs
        f2fs
        # fat32
        # hpfs
        # ntfs
        # cdfs
)

message(STATUS "Filesystem drivers configured:")
message(STATUS "  - ext4")
message(STATUS "  - btrfs")
message(STATUS "  - f2fs")
# message(STATUS "  - fat32")
# message(STATUS "  - hpfs")