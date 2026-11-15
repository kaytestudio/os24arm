#!/usr/bin/env python3
"""userland/os2x86/test/elf2lx.py - an i386 ELF object (.o) as an OS/2 LX .EXE.

    elf2lx.py in.o out.exe [entry-symbol]

Just enough of a linker for os2x86's tests: code (.text*) becomes object
1 at 0x10000, data (.rodata*, .data*, .bss*) object 2 at the next 64 KB,
then a 64 KB stack; R_386_32 relocations become LX internal fixups,
calls to Dos* functions (undefined symbols) import fixups by ordinal from
DOSCALLS (NLS, MSG: as the table says). A DOS "MZ" stub goes first, as
real OS/2 programs have it. The C is compiled freestanding:
    clang --target=i386-unknown-none-elf -march=pentiumpro -mno-sse -mno-mmx
          -ffreestanding -fno-pic -O1 -c x.c
"""
import struct, sys

IMPORTS = {   # name: (module, ordinal)
    'DosWrite': ('DOSCALLS', 282), 'DosRead': ('DOSCALLS', 281), 'DosExit': ('DOSCALLS', 234),
    'DosOpen': ('DOSCALLS', 273), 'DosClose': ('DOSCALLS', 257), 'DosAllocMem': ('DOSCALLS', 299),
    'DosFreeMem': ('DOSCALLS', 304), 'DosGetInfoBlocks': ('DOSCALLS', 312), 'DosScanEnv': ('DOSCALLS', 227),
    'DosQueryCurrentDir': ('DOSCALLS', 274), 'DosQueryCurrentDisk': ('DOSCALLS', 275),
    'DosFindFirst': ('DOSCALLS', 264), 'DosFindNext': ('DOSCALLS', 265), 'DosFindClose': ('DOSCALLS', 263),
    'DosSetExceptionHandler': ('DOSCALLS', 354), 'DosUnsetExceptionHandler': ('DOSCALLS', 355),
    'DosSetFilePtr': ('DOSCALLS', 256), 'DosDelete': ('DOSCALLS', 259), 'DosQueryPathInfo': ('DOSCALLS', 223),
    'DosQuerySysInfo': ('DOSCALLS', 348), 'DosGetDateTime': ('DOSCALLS', 230), 'DosExecPgm': ('DOSCALLS', 283),
    'DosQueryHType': ('DOSCALLS', 224), 'DosSleep': ('DOSCALLS', 229), 'DosCreateDir': ('DOSCALLS', 270),
    'DosDeleteDir': ('DOSCALLS', 226), 'DosMove': ('DOSCALLS', 271), 'DosQueryCtryInfo': ('NLS', 5),
    'DosPutMessage': ('MSG', 5),
    'DosCreateThread': ('DOSCALLS', 311), 'DosWaitThread': ('DOSCALLS', 349), 'DosKillThread': ('DOSCALLS', 111),
    'DosCreateMutexSem': ('DOSCALLS', 331), 'DosRequestMutexSem': ('DOSCALLS', 334),
    'DosReleaseMutexSem': ('DOSCALLS', 335), 'DosCreateEventSem': ('DOSCALLS', 324),
    'DosPostEventSem': ('DOSCALLS', 328), 'DosWaitEventSem': ('DOSCALLS', 329), 'DosCloseEventSem': ('DOSCALLS', 326),
    'DosCloseMutexSem': ('DOSCALLS', 333), 'DosCreateMuxWaitSem': ('DOSCALLS', 337),
    'DosWaitMuxWaitSem': ('DOSCALLS', 340), 'DosFlatToSel': ('DOSCALLS', 425),
    'VIO16WRTTTY': ('VIOCALLS', 19), 'VIO16GETMODE': ('VIOCALLS', 21), 'VIO16GETCP': ('VIOCALLS', 40),
    'KBD16PEEK': ('KBDCALLS', 22),
    'DosCreateQueue': ('QUECALLS', 16), 'DosWriteQueue': ('QUECALLS', 14), 'DosReadQueue': ('QUECALLS', 9),
    'DosCloseQueue': ('QUECALLS', 11), 'DosQueryQueue': ('QUECALLS', 12), 'DosStartSession': ('SESMGR', 37),
    'WinInitialize': ('PMWIN', 763), 'DosQueryModuleName': ('DOSCALLS', 320),
}

def main():
    src, out = sys.argv[1], sys.argv[2]
    entry_name = sys.argv[3] if len(sys.argv) > 3 else 'start'
    e = open(src, 'rb').read()
    assert e[:4] == b'\x7fELF' and e[4] == 1, 'not ELF32'
    shoff, = struct.unpack_from('<I', e, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', e, 0x2E)
    secs = []
    for i in range(shnum):
        secs.append(struct.unpack_from('<IIIIIIIIII', e, shoff + i * shentsize))
    strtab_sec = secs[shstrndx]
    def secname(s):
        o = strtab_sec[4] + s[0]
        return e[o:e.index(b'\0', o)].decode()
    names = [secname(s) for s in secs]
    # place the sections
    CODE, DATA, STACK = 0x10000, None, None
    base = {}
    code = bytearray()
    for i, s in enumerate(secs):
        if s[1] == 1 and (s[2] & 4) and (s[2] & 2):        # PROGBITS, ALLOC|EXEC
            al = max(s[8], 1); code += b'\0' * (-len(code) % al)
            base[i] = CODE + len(code); code += e[s[4]:s[4] + s[5]]
    DATA = (CODE + len(code) + 0xFFFF) & ~0xFFFF
    if DATA == CODE: DATA += 0x10000
    data = bytearray(); bss = 0
    for i, s in enumerate(secs):
        if i in base or not (s[2] & 2): continue
        if s[1] == 1:
            al = max(s[8], 1); data += b'\0' * (-len(data) % al)
            base[i] = DATA + len(data); data += e[s[4]:s[4] + s[5]]
    dsize = len(data)
    for i, s in enumerate(secs):
        if i in base or not (s[2] & 2): continue
        if s[1] == 8:                                       # NOBITS
            al = max(s[8], 1); dsize += -dsize % al
            base[i] = DATA + dsize; dsize += s[5]
    if dsize == 0: dsize = 4; data += b'\0' * 4
    STACK = (DATA + dsize + 0xFFFF) & ~0xFFFF
    # symbols
    symsec = next(s for s in secs if s[1] == 2)
    strs = secs[symsec[6]]
    syms = []
    for k in range(symsec[5] // 16):
        nm, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', e, symsec[4] + 16 * k)
        o = strs[4] + nm
        syms.append((e[o:e.index(b'\0', o)].decode(), val, shndx))
    def symaddr(k):
        nm, val, shndx = syms[k]
        if shndx == 0: return None, nm
        return base[shndx] + val, nm
    entry = None
    for nm, val, shndx in syms:
        if nm == entry_name and shndx: entry = base[shndx] + val
    assert entry is not None, 'no ' + entry_name
    img = {CODE: code, DATA: data}
    def where(addr):
        for b0, buf in img.items():
            if b0 <= addr < b0 + len(buf): return b0, buf
        raise SystemExit('relocation outside the image')
    fix = []            # (addr, kind, target): kind 'int32' target=addr, 'imp32'/'imprel' target=(mod, ord)
    for i, s in enumerate(secs):
        if s[1] != 9: continue                              # REL
        tgt = s[7]
        if tgt not in base or tgt not in [k for k in base if secs[k][1] == 1]: continue
        for k in range(s[5] // 8):
            off, info = struct.unpack_from('<II', e, s[4] + 8 * k)
            typ, sym = info & 0xFF, info >> 8
            addr = base[tgt] + off
            b0, buf = where(addr)
            A, = struct.unpack_from('<i', buf, addr - b0)
            S, nm = symaddr(sym)
            if typ == 1:
                if S is None:
                    fix.append((addr, 'imp32', IMPORTS[nm]))
                else:
                    struct.pack_into('<I', buf, addr - b0, (S + A) & 0xFFFFFFFF)
                    fix.append((addr, 'int32', (S + A) & 0xFFFFFFFF))
            elif typ in (2, 4):
                if S is None:
                    if nm not in IMPORTS: raise SystemExit('unknown import ' + nm)
                    fix.append((addr, 'imprel', IMPORTS[nm]))
                else:
                    struct.pack_into('<i', buf, addr - b0, S + A - addr)
            else:
                raise SystemExit('relocation type %d' % typ)
    # import modules
    mods = []
    for _, kind, t in fix:
        if kind != 'int32' and t[0] not in mods: mods.append(t[0])
    # objects: (base, vsize, flags, bytes)
    objs = [(CODE, max(len(code), 1), 0x2005, bytes(code)), (DATA, dsize, 0x2003, bytes(data)),
            (STACK, 0x10000, 0x2003, b'')]
    pages = []          # (obj index, page in obj, bytes)
    ptab = []           # per object: first page (1-based), count
    for oi, (b0, vs, fl, by) in enumerate(objs):
        n = (len(by) + 4095) // 4096
        ptab.append((len(pages) + 1, n))
        for p in range(n): pages.append((oi, p, by[p * 4096:(p + 1) * 4096]))
    def objof(addr):
        for oi, (b0, vs, fl, by) in enumerate(objs):
            if b0 <= addr < b0 + vs: return oi
        raise SystemExit('fixup target 0x%x outside' % addr)
    # fixup records per page
    recs = []
    for gi, (oi, p, by) in enumerate(pages):
        pstart = objs[oi][0] + p * 4096
        r = bytearray()
        for addr, kind, t in fix:
            if not (pstart - 3 <= addr < pstart + 4096): continue
            so = addr - pstart
            if kind == 'int32':
                to = objof(t)
                r += struct.pack('<BBhBI', 0x07, 0x10, so, to + 1, t - objs[to][0])
            else:
                r += struct.pack('<BBhBH', 0x07 if kind == 'imp32' else 0x08, 0x01, so, mods.index(t[0]) + 1, t[1])
        recs.append(bytes(r))
    # the file
    LX = 0x80
    hdr_size = 0xC4
    objtab = b''.join(struct.pack('<IIIIII', vs, b0, fl, ptab[i][0], ptab[i][1], 0) for i, (b0, vs, fl, by) in enumerate(objs))
    optab = bytearray()
    resn = bytes([len('OS2TEST')]) + b'OS2TEST' + b'\0\0' + b'\0'
    entryt = b'\0'
    fpt = bytearray(); frt = bytearray()
    for r in recs: fpt += struct.pack('<I', len(frt)); frt += r
    fpt += struct.pack('<I', len(frt))
    impmod = b''.join(bytes([len(m)]) + m.encode() for m in mods)
    impproc = b'\0'
    o = hdr_size
    o_obj = o; o += len(objtab)
    o_opt = o; o += 8 * len(pages)
    o_res = o; o += len(resn)
    o_ent = o; o += len(entryt)
    o_fpt = o; o += len(fpt)
    o_frt = o; o += len(frt)
    o_imp = o; o += len(impmod)
    o_ipr = o; o += len(impproc)
    data_off = LX + o
    pdata = bytearray()
    for oi, p, by in pages:
        optab += struct.pack('<IHH', len(pdata), len(by), 0)
        pdata += by
    h = bytearray(hdr_size)
    h[0:4] = b'LX\0\0'
    struct.pack_into('<IHHII', h, 4, 0, 2, 1, 0, 0x200)
    struct.pack_into('<IIIIIIII', h, 0x14, len(pages), 1, entry - CODE, 3, 0x10000, 4096, 0, len(fpt) + len(frt))
    struct.pack_into('<II', h, 0x38, o_fpt - o_obj, 0)
    struct.pack_into('<IIIIIIIII', h, 0x40, o_obj, len(objs), o_opt, 0, 0, 0, o_res, o_ent, 0)
    struct.pack_into('<IIIIIII', h, 0x64, 0, o_fpt, o_frt, o_imp, len(mods), o_ipr, 0)
    struct.pack_into('<I', h, 0x80, data_off)
    struct.pack_into('<I', h, 0x94, 2)
    mz = bytearray(LX)
    mz[0:2] = b'MZ'
    struct.pack_into('<I', mz, 0x3C, LX)
    f = mz + h + objtab + optab + resn + entryt + fpt + frt + impmod + impproc
    assert len(f) == data_off
    open(out, 'wb').write(bytes(f + pdata))

main()
