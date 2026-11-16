/* dos/a64.h - an AArch64 (A64) disassembler and assembler for DEBUG's U and A
 * commands (dos/a64.c): the integer instruction set, floating point and
 * Advanced SIMD (dos/a64_simd.inc; FP16 and the newest extensions partly);
 * anything else disassembles as ".inst 0x...". Output follows
 * LLVM's (llvm-objdump) spelling: its aliases (mov, cmp, lsl, cset, ...),
 * hex immediates, absolute branch targets. */
#ifndef A64_H
#define A64_H
/* (its own integer types: this builds with the kernel's own stdint.h, which has no 64-bit ones, and the host's) */
typedef unsigned int a64_u32;
typedef unsigned short a64_u16;
typedef unsigned long long a64_u64;
typedef long long a64_s64;

/* One instruction at pc into out ("mnemonic\toperands"); 1 if it was known, 0 for ".inst". */
int a64_disasm(a64_u32 insn, a64_u64 pc, char *out, unsigned size);
/* One line of assembly (the disassembler's syntax) at pc: 1 and *insn, or 0 and a message in err.
 * Immediates take # (decimal, or 0x hex); a number without # is an address (a branch's, ADR's, a
 * literal load's) and is hex, as DEBUG's addresses are - 0x optional. */
int a64_asm(const char *line, a64_u64 pc, a64_u32 *insn, char *err, unsigned errsize);

#endif
