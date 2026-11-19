# lx - OS/2 LX file tools

Host tools that wrap raw ARM64 machine code into OS/2 LX executables and
DLLs the kernel's LX loader can run. The main build compiles them into
`build/lx-host-build/`; `lx/CMakeLists.txt` also builds them on their own.

| file | what it is |
|---|---|
| `lxformat.h`, `lxbuilder.c` | the LX structures and the writer (`lxlib`) |
| `makeexe` | `makeexe code.bin data.bin OUT.EXE entry_offset` |
| `makedll` | `makedll code.bin data.bin OUT.DLL NAME [export:offset ...]` |
| `lxbuild` | the general tool: EXE, DLL or SYS, exports, imports, stack size (`lxbuild -h`) |
| `lx_app.ld`, `lx_dll.ld` | link scripts: apps and DLLs link at fixed addresses |
| `lx_entry_offset.py`, `lx_export_offsets.py` | find `main`'s / the exports' offsets in a linked ELF |

The top-level `CMakeLists.txt` uses these in `add_lx_executable()` and
`add_lx_library()`: compile with KCC (`tools/kcc`), link with `lx_app.ld` or
`lx_dll.ld`, split code and data with objcopy, then `makeexe` / `makedll`.

Examples:

    lxbuild -o HELLO.EXE --entry 0x1000 hello.bin
    lxbuild -t dll -o MYLIB.DLL -n MYLIB --export MyFunc1:0 --export MyFunc2:100 mylib.bin
