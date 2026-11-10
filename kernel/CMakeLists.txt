# kernel/CMakeLists.txt - Updated with IFS and ext4 support

cmake_minimum_required(VERSION 3.21)

# ── Kernel source files ───────────────────────────────────────────────────────
if(TARGET_ARCH STREQUAL "aarch64")
set(KERNEL_SOURCES
        src/mem.c
        src/uart.c
        src/scheduler.c
        src/keyboard.c
        src/vfs.c
        src/blkdev.c
        src/gpt.c
        src/fat32.c             # ← FAT16/FAT32 (the GPT "BOOT" partition)
        src/string.c
        src/kprintf.c
        src/ifs_loader.c        # ← IFS loader
        src/ai.c                # ← on-box AI assistant command
        src/basic.c
        src/kio.c
        src/lx_loader.c         # ← built-in OS/2 LX (.EXE) loader
        src/stack_protector.c   # ← -fstack-protector-strong runtime support
        src/unixroot.c          # ← EMX-style UNIXROOT directory tree (SET UNIXROOT=)

        # Add GUI sources:
        src/gui/framebuffer.c
        src/gui/wayland.c
        src/gui/gui_demo.c
        src/gui/ramfb.c
        src/gui/virtio_input.c
        src/gui/virtio_gpu.c
        ../drivers/video/virtiofb.c
)

# ── Add string.c if it exists ─────────────────────────────────────────────────
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/string.c)
    list(APPEND KERNEL_SOURCES src/string.c)
endif()

# ── Platform-specific sources ─────────────────────────────────────────────────
if(TARGET_PLATFORM STREQUAL "RPI3")
    list(APPEND KERNEL_SOURCES
            src/rpi_boot.S
            src/rpi_main.c
    )
    set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/rpi_linker.ld)
else()
    list(APPEND KERNEL_SOURCES
            src/boot.S
            src/main.c
            src/mmu.c    # ← identity-mapped MMU bring-up (QEMU virt memory map only)
            src/hyp.c           # ← EL2 hypervisor: stage 2, vCPU run loop, VM command
            src/hyp_entry.S     # ← EL2 vectors + guest/host world switch
            src/vgic.c          # ← emulated GICv2 the guest sees
            src/gicv3.c         # ← host GICv3: timer PPIs -> EL2 (preemption)
            src/vm_pl011.c      # ← emulated PL011 device model
            src/vm_cmd.c        # ← VM shell command / VM run loop
            src/vm_guest_hello.S  # ← built-in test guest for VM TEST
    )
    set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld)
endif()

else()
    # BeagleBone Black (ARMv7) / BeagleV-Ahead + QEMU_RISCV (RV64): serial-console kernel
    # only - the shared sources below are architecture-neutral C, everything
    # else (MMU, GUI, LX loader, OS/2 API, filesystems) is still AArch64-only.
    set(KERNEL_SOURCES
            src/mem.c
            src/scheduler.c
            src/string.c
            src/stack_protector.c
            src/ns16550.c           # ← 16550 UART (AM335x UART0 / TH1520 UART0)
            src/beagle_main.c       # ← kernel_main + serial shell for both boards
            src/vfs.c               # ← VFS under the OS/2 API
            src/kprintf.c
            src/kio.c
            src/sdhci.c             # ← microSD (AM335x MMCHS / TH1520 DWC MSHC)
            src/blkdev.c
            src/mbr.c               # ← MBR partitions (SD cards)
            src/gpt.c
            src/fat32.c             # ← FAT32 (the cards' boot partition)
            ${CMAKE_SOURCE_DIR}/fs/ext4/ext4.c
            ${CMAKE_SOURCE_DIR}/fs/ext4/ext4_write.c
    )
    if(TARGET_PLATFORM STREQUAL "BBB")
        list(APPEND KERNEL_SOURCES
                src/bbb_boot.S
                src/arm_aeabi.c     # ← __aeabi_mem* helpers clang emits on ARMv7
        )
        set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/bbb_linker.ld)
    else()
        # BeagleV-Ahead and QEMU_RISCV share the RV64 entry code and linker
        # script; only the link address differs (KERNEL_LOAD_ADDR, passed
        # to the script below as a symbol).
        list(APPEND KERNEL_SOURCES src/beaglev_boot.S)
        if(TARGET_PLATFORM STREQUAL "QEMU_RISCV")
            list(APPEND KERNEL_SOURCES src/qemu_pci.c)   # ← finds sdhci-pci, assigns its BAR
        endif()
        set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/beaglev_linker.ld)
    endif()
endif()

# ── Kernel executable ─────────────────────────────────────────────────────────
add_executable(os2warp_kernel ${KERNEL_SOURCES})

# ── Include directories ───────────────────────────────────────────────────────
target_include_directories(os2warp_kernel PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/fs/ext4
        ${CMAKE_SOURCE_DIR}/fs/btrfs
)

# ── Link libraries ────────────────────────────────────────────────────────────
if(TARGET_ARCH STREQUAL "aarch64")
    target_link_libraries(os2warp_kernel
            os2api
            dos             # ← DOS command set (calls os2api, not vfs_* directly)
            rexx
            ext4            # ← ext4 filesystem driver
            btrfs           # ← btrfs filesystem driver
    )
else()
    # BeagleBone Black / BeagleV-Ahead: OS/2 API over the VFS, microSD
    # mounted at boot (see beagle_main.c)
    target_link_libraries(os2warp_kernel os2api)
    target_compile_definitions(os2warp_kernel PRIVATE HAVE_OS2API HAVE_SD)
endif()

# ── Linker options ────────────────────────────────────────────────────────────
target_link_options(os2warp_kernel PRIVATE
        -T ${LINKER_SCRIPT}
        -nostdlib
        -static
)
if(TARGET_ARCH STREQUAL "riscv64")
    target_link_options(os2warp_kernel PRIVATE -Wl,--defsym=KERNEL_LOAD_ADDR=${KERNEL_LOAD_ADDR})
endif()

# ── Set linker script dependency ──────────────────────────────────────────────
set_target_properties(os2warp_kernel PROPERTIES
        LINK_DEPENDS ${LINKER_SCRIPT}
)

# ── Print configuration ───────────────────────────────────────────────────────
message(STATUS "Kernel configuration:")
message(STATUS "  Platform:      ${TARGET_PLATFORM}")
message(STATUS "  Linker script: ${LINKER_SCRIPT}")
message(STATUS "  Sources:       ${KERNEL_SOURCES}")