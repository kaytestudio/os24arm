# ── Kayte host tool (FreePascal) ────────────────────────────────────────────
# kayte/kayte.lpr is a native, OS-hosted Lazarus/FreePascal program (uses
# SysUtils, Process, fphttpclient, Zipper, sockets...) - it runs on the BUILD
# machine, the same way tools/kcc and lx/lxbuild do. It is not freestanding
# code and cannot run inside the bare-metal kernel image as-is.
find_program(FPC_EXECUTABLE NAMES fpc)

if(FPC_EXECUTABLE)
    get_filename_component(FPC_BIN_DIR ${FPC_EXECUTABLE} DIRECTORY)

    # A bare `fpc` invocation needs to find its own fpc.cfg to locate the RTL
    # unit search path (system.ppu etc). A normal Homebrew/apt install finds
    # this automatically; some fpcupdeluxe layouts keep fpc.cfg under
    # <fpcroot>/etc instead of next to the fpc binary, so a bare invocation
    # fails with "Can't find unit system". Probe for that, and only fall
    # back to an explicit @config file if the bare probe fails.
    set(FPC_PROBE_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/fpc_probe.pas)
    file(WRITE ${FPC_PROBE_SOURCE} "program fpc_probe;\nbegin\nend.\n")

    execute_process(
            COMMAND ${FPC_EXECUTABLE} -n ${FPC_PROBE_SOURCE}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            RESULT_VARIABLE FPC_BARE_RESULT
            OUTPUT_QUIET ERROR_QUIET
    )

    set(FPC_CONFIG_ARGS "")
    if(FPC_BARE_RESULT EQUAL 0)
        message(STATUS "Found fpc: ${FPC_EXECUTABLE} (default config works)")
    else()
        set(FPC_CONFIG_CANDIDATES
                ${FPC_BIN_DIR}/fpc.cfg
                ${FPC_BIN_DIR}/../fpc.cfg
                ${FPC_BIN_DIR}/../etc/fpc.cfg
                ${FPC_BIN_DIR}/../../etc/fpc.cfg
                /etc/fpc.cfg
                $ENV{HOME}/.fpc.cfg
        )
        foreach(candidate ${FPC_CONFIG_CANDIDATES})
            if(NOT FPC_CONFIG_ARGS AND EXISTS ${candidate})
                execute_process(
                        COMMAND ${FPC_EXECUTABLE} -n @${candidate} ${FPC_PROBE_SOURCE}
                        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                        RESULT_VARIABLE FPC_CFG_RESULT
                        OUTPUT_QUIET ERROR_QUIET
                )
                if(FPC_CFG_RESULT EQUAL 0)
                    set(FPC_CONFIG_ARGS -n @${candidate})
                    message(STATUS "Found fpc: ${FPC_EXECUTABLE} (using config ${candidate})")
                endif()
            endif()
        endforeach()

        if(NOT FPC_CONFIG_ARGS)
            message(WARNING "fpc found at ${FPC_EXECUTABLE} but no working fpc.cfg was found "
                    "(tried: ${FPC_CONFIG_CANDIDATES}) - kayte host tool build disabled")
            set(FPC_EXECUTABLE FPC_EXECUTABLE-NOTFOUND)
        endif()
    endif()
endif()

if(FPC_EXECUTABLE)
    file(GLOB KAYTE_PASCAL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.pas)
    set(KAYTE_BINARY ${CMAKE_CURRENT_BINARY_DIR}/kayte CACHE FILEPATH "Path to host kayte tool" FORCE)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/units)

    add_custom_command(
            OUTPUT ${KAYTE_BINARY}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/units
            COMMAND ${FPC_EXECUTABLE} ${FPC_CONFIG_ARGS}
                    -FE${CMAKE_CURRENT_BINARY_DIR}
                    -FU${CMAKE_CURRENT_BINARY_DIR}/units
                    ${CMAKE_CURRENT_SOURCE_DIR}/kayte.lpr
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kayte.lpr ${KAYTE_PASCAL_SOURCES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Building kayte host tool with FreePascal (${FPC_EXECUTABLE})"
            VERBATIM
    )
    add_custom_target(kayte_host_tool ALL DEPENDS ${KAYTE_BINARY})
    message(STATUS "Kayte host tool (FreePascal) will be built at: ${KAYTE_BINARY}")
else()
    message(STATUS "fpc not found - kayte host tool build disabled (install FreePascal, "
            "e.g. 'brew install fpc', to enable it)")
endif()

# ── FreePascal bare-metal (aarch64-embedded) scaffolding ────────────────────
# kayte.lpr itself is far from freestanding-portable (SysUtils, Process,
# fphttpclient, Zipper, sockets) - actually running Kayte's own code inside
# the kernel image needs that dependency surface stripped out first, which
# is a separate, larger rewrite. This block only wires up detection of an
# FPC aarch64-embedded (bare-metal) cross-compiler, so that work can start
# later without a second round of build-system plumbing. It does not build
# anything freestanding yet.
option(KAYTE_ENABLE_EMBEDDED_TARGET "Probe for an FPC aarch64-embedded (bare-metal) cross-compiler" OFF)

if(KAYTE_ENABLE_EMBEDDED_TARGET)
    find_program(FPC_EMBEDDED_EXECUTABLE
            NAMES ppcrossa64 ppcaarch64 fpc
            PATHS
            ${FPC_BIN_DIR}/../aarch64-embedded
            $ENV{HOME}/fpcupdeluxe/fpc/bin/aarch64-embedded
            NO_DEFAULT_PATH
    )
    if(FPC_EMBEDDED_EXECUTABLE)
        message(STATUS "Found FPC aarch64-embedded cross-compiler: ${FPC_EMBEDDED_EXECUTABLE}")
    else()
        message(STATUS "No FPC aarch64-embedded cross-compiler found. In fpcupdeluxe, add the "
                "cross target 'AArch64-Embedded' to start bare-metal Kayte work.")
    endif()
endif()
