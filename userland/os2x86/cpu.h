/* userland/os2x86/cpu.h - a software i386 for 32-bit OS/2 programs.
 *
 * Uses the flat model OS/2 2.x gives its programs: CS, DS, ES and SS cover
 * all 4 GB from 0, and FS points at the thread information block. For the
 * 16-bit calls (VIO, KBD; C runtimes reach them through a 16-bit stack and
 * code segment) it also has OS/2's tiled LDT: selector n*8+7 is the 64 KB at
 * n*64K, as 16-bit code or stack. Guest memory is one host block (mem, size
 * bytes; guest address 0 is mem[0]); anything outside it or below lo traps.
 * Implements the 386/486 integer instructions (plus CMOV, BSWAP, CPUID, RDTSC)
 * and the x87, using doubles internally. */
#ifndef OS2X86_CPU_H
#define OS2X86_CPU_H
#include <stdint.h>

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

/* EFLAGS bits */
#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

/* why cpu_run stopped */
enum {
    CPU_STEPS,      /* it ran as many instructions as it was given */
    CPU_THUNK,      /* EIP reached the API area: the program calls the system */
    CPU_FAULT,      /* a page fault (fault_addr: where) */
    CPU_ILLEGAL,    /* an instruction it doesn't know */
    CPU_INT,        /* INT n (int_no), INT3, INTO, HLT */
    CPU_DIVIDE,     /* division by zero, or a quotient too big */
};

typedef struct cpu {
    uint32_t r[8];
    uint32_t eip, eflags;
    uint32_t fs_base, gs_base;
    uint32_t es_base, cs_base, ss_base, ds_base;   /* 0 unless a tiled selector is loaded */
    int code16, stack16;            /* CS / SS is a tiled 16-bit segment */
    uint32_t tib;                   /* FS base for the GDT selector 0x150b */
    uint16_t sreg[6];               /* ES CS SS DS FS GS selectors */
    uint8_t *mem;
    uint32_t size;                  /* bytes of guest memory */
    uint32_t lo;                    /* below this there's no memory (the 64 KB NULL area) */
    uint32_t thunk_lo, thunk_hi;    /* EIP in [lo, hi): CPU_THUNK */
    uint32_t fault_addr, op_eip;    /* address that trapped; start of the current instruction */
    int int_no;
    /* x87 */
    double st[8];
    int top;
    uint16_t fcw, fsw, ftw;         /* control, status, tag (empty slots are in tag_empty) */
    uint8_t empty[8];
    uint64_t tsc;
} cpu_t;

void cpu_reset(cpu_t *c);
void cpu_load_seg(cpu_t *c, int seg, uint16_t sel);   /* seg: 0 ES 1 CS 2 SS 3 DS 4 FS 5 GS */
int  cpu_run(cpu_t *c, long steps);

/* the guest's memory, checked */
int  mem_ok(const cpu_t *c, uint32_t a, uint32_t n);
uint8_t  rd8(cpu_t *c, uint32_t a);
uint16_t rd16(cpu_t *c, uint32_t a);
uint32_t rd32(cpu_t *c, uint32_t a);
void wr8(cpu_t *c, uint32_t a, uint8_t v);
void wr16(cpu_t *c, uint32_t a, uint16_t v);
void wr32(cpu_t *c, uint32_t a, uint32_t v);
void push32(cpu_t *c, uint32_t v);
uint32_t pop32(cpu_t *c);

#endif
