#!/usr/bin/env python3
"""lx/lx_export_offsets.py — print "name:offset" pairs for a list of
symbols within an LX DLL's flat image, for makedll's export table.

Same idea as lx_entry_offset.py (one symbol, for an EXE's eip), but for
however many named exports a DLL wants to advertise: looks each symbol's
real address up via llvm-nm and subtracts the known link base (see
lx/lx_app.ld / kernel/include/lx_loader.h's LX_LOAD_BASE) to get its
byte offset within the flat image makedll wraps.

Usage: lx_export_offsets.py <elf_file> <base_addr_hex> <symbol> [symbol...] [-- <llvm-nm path>]

Prints one "name:offset" line per symbol, in the order given, to stdout.
Exits non-zero (with a message on stderr) if any symbol isn't found -
silently dropping an export would produce a DLL that looks fine but is
missing entries, which is worse than failing the build.
"""
import subprocess
import sys


def main():
    args = sys.argv[1:]
    nm = "llvm-nm"
    if "--" in args:
        idx = args.index("--")
        nm = args[idx + 1]
        args = args[:idx]

    if len(args) < 3:
        print("usage: lx_export_offsets.py <elf> <base_hex> <symbol> [symbol...] [-- <nm>]",
              file=sys.stderr)
        return 1

    elf_file, base_hex = args[0], args[1]
    symbols = args[2:]
    base = int(base_hex, 16)

    out = subprocess.run([nm, elf_file], capture_output=True, text=True, check=True).stdout
    addrs = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            try:
                addrs[parts[-1]] = int(parts[0], 16)
            except ValueError:
                continue

    missing = [s for s in symbols if s not in addrs]
    if missing:
        print(f"lx_export_offsets.py: symbol(s) not found in {elf_file}: {', '.join(missing)}",
              file=sys.stderr)
        return 1

    for s in symbols:
        print(f"{s}:{addrs[s] - base}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
