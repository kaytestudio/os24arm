/* userland/os2x86/cpu.h - an i386 in software, for 32-bit OS/2 programs.
 *
 * Flat memory model as OS/2 2.x+ gives its programs: CS, DS, ES, SS all
 * cover the whole 4 GB from 0, FS points at the thread's information
 * block. The guest's memory is one block of the host's (mem, size bytes:
 * guest address 0 is mem[0]); an access outside it, or below lo, is a
 * trap. Integer
 * instructions of the 386/486 (and CMOV, BSWAP, CPUID, RDTSC) and the
 * x87, with doubles inside. */
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
    uint32_t fs_base, gs_base;      /* (the other segments start at 0) */
    uint16_t sreg[6];               /* ES CS SS DS FS GS: the selectors, as read */
    uint8_t *mem;
    uint32_t size;                  /* bytes of guest memory */
    uint32_t lo;                    /* below this: no memory (the NULL page's 64 KB) */
    uint32_t thunk_lo, thunk_hi;    /* EIP in [lo, hi): CPU_THUNK */
    uint32_t fault_addr, op_eip;    /* the trap's address; the instruction's */
    int int_no;
    /* x87 */
    double st[8];
    int top;
    uint16_t fcw, fsw, ftw;         /* control, status, tag (0: empty slots in tag_empty) */
    uint8_t empty[8];
    uint64_t tsc;
} cpu_t;

void cpu_reset(cpu_t *c);
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
