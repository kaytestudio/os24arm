#!/bin/bash
# Fix: Use linker scripts instead of lld

echo "Fixing commands build to use linker scripts..."

cp kernel/commands/CMakeLists.txt kernel/commands/CMakeLists.txt.backup.final

cat > kernel/commands/CMakeLists.txt << 'ENDCMAKE'
# ============================================================================
# kernel/commands/CMakeLists.txt
# ============================================================================

if(CMAKE_CROSSCOMPILING)
    set(LX_TOOLS_DEP lx_host_tools)
else()
    set(LX_TOOLS_DEP lxbuild)
endif()

set(CMD_C_FLAGS -ffreestanding -nostdlib -fno-stack-protector -fPIC -Os -mstrict-align)
set(CMD_LINK_FLAGS -nostdlib)
set(CMD_INCLUDES "-I${CMAKE_CURRENT_SOURCE_DIR}/../include" "-I${CMAKE_SOURCE_DIR}/rexx" "-I${CMAKE_SOURCE_DIR}/os2")
set(ENTRY_ADDRESS 0x10000)
set(DEFAULT_STACK 65536)
set(LARGE_STACK 131072)

function(build_lx_exe EXE_NAME SOURCE_FILE STACK_SIZE)
    string(TOUPPER ${EXE_NAME} TARGET_NAME)
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.o
        COMMAND ${CMAKE_C_COMPILER} ${CMD_C_FLAGS} ${CMD_INCLUDES}
            -c ${CMAKE_CURRENT_SOURCE_DIR}/../src/${SOURCE_FILE}
            -o ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.o
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../src/${SOURCE_FILE}
        COMMENT "Compiling ${SOURCE_FILE}"
        VERBATIM
    )
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.ld
        COMMAND ${CMAKE_COMMAND} -E echo "SECTIONS { . = ${ENTRY_ADDRESS}\\; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }"
            > ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.ld
        COMMENT "Creating linker script"
        VERBATIM
    )
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
        COMMAND ${CMAKE_C_COMPILER} ${CMD_LINK_FLAGS}
            -Wl,-T,${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.ld
            ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.o
            -o ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.o ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.ld
        COMMENT "Linking ${TARGET_NAME}.elf"
        VERBATIM
    )
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary
            ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
            ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
        COMMENT "Extracting binary"
        VERBATIM
    )
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${EXE_NAME}.EXE
        COMMAND ${LXBUILD} -o ${EXE_NAME}.EXE --entry 0 --stack ${STACK_SIZE}
            ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
        DEPENDS ${LX_TOOLS_DEP} ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Creating ${EXE_NAME}.EXE"
        VERBATIM
    )
    
    add_custom_target(${TARGET_NAME}_exe ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${EXE_NAME}.EXE)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${EXE_NAME}.EXE DESTINATION bin)
endfunction()

function(build_lx_exe_multi EXE_NAME STACK_SIZE)
    string(TOUPPER ${EXE_NAME} TARGET_NAME)
    set(SOURCE_FILES ${ARGN})
    set(OBJECT_FILES "")
    
    foreach(SOURCE_FILE ${SOURCE_FILES})
        get_filename_component(BASE_NAME ${SOURCE_FILE} NAME_WE)
        set(OBJ_FILE ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}_${BASE_NAME}.o)
        add_custom_command(
            OUTPUT ${OBJ_FILE}
            COMMAND ${CMAKE_C_COMPILER} ${CMD_C_FLAGS} ${CMD_INCLUDES}
                -c ${CMAKE_CURRENT_SOURCE_DIR}/../src/${SOURCE_FILE} -o ${OBJ_FILE}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../src/${SOURCE_FILE}
            COMMENT "Compiling ${SOURCE_FILE}"
            VERBATIM
        )
        list(APPEND OBJECT_FILES ${OBJ_FILE})
    endforeach()
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.ld
        COMMAND ${CMAKE_COMMAND} -E echo "SECTIONS { . = ${ENTRY_ADDRESS}\\; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }"
            > ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.ld
        COMMENT "Creating linker script"
        VERBATIM
    )
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
        COMMAND ${CMAKE_C_COMPILER} ${CMD_LINK_FLAGS}
            -Wl,-T,${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.ld ${OBJECT_FILES}
            -o ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
        DEPENDS ${OBJECT_FILES} ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.ld
        COMMENT "Linking ${TARGET_NAME}.elf"
        VERBATIM
    )
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
        COMMAND ${CMAKE_OBJCOPY} -O binary
            ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
            ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.elf
        COMMENT "Extracting binary"
        VERBATIM
    )
    
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${EXE_NAME}.EXE
        COMMAND ${LXBUILD} -o ${EXE_NAME}.EXE --entry 0 --stack ${STACK_SIZE}
            ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
        DEPENDS ${LX_TOOLS_DEP} ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Creating ${EXE_NAME}.EXE"
        VERBATIM
    )
    
    add_custom_target(${TARGET_NAME}_exe ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${EXE_NAME}.EXE)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${EXE_NAME}.EXE DESTINATION bin)
endfunction()

build_lx_exe(BAS basic.c ${LARGE_STACK})
build_lx_exe_multi(CLI ${LARGE_STACK} cli.c cli_fs.c)

add_custom_target(all_commands DEPENDS BAS_exe CLI_exe)

message(STATUS "Commands: BAS.EXE, CLI.EXE")
ENDCMAKE

echo "Fixed! Now using linker scripts instead of lld"
echo ""
echo "Changes:"
echo "  - Generates a simple linker script for each command"
echo "  - Uses -T script.ld (works with any linker)"
echo "  - No lld dependency"
echo ""
echo "Rebuild:"
echo "   cd build"
echo "   cmake .."
echo "   make"
