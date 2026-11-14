/* lx_stubs.S - the services an OS/2 (LX) program gets when it runs as a
 * process (linux.c, "OS/2 programs as processes"): EL0 code, copied into
 * a page of the program's own (LX_STUBS_VA), that the lx_services table
 * there points to. Position independent: only relative branches.
 *
 * Console output and input are the process's stdout and stdin (Linux
 * write/read/ppoll); the file system calls (struct lx_vfs_ops) go to the
 * kernel's VFS through one system call, LX_SYS (x8), the operation in x7. */

#define SYS_read          63
#define SYS_write         64
#define SYS_ppoll         73
#define SYS_exit_group    94
#define SYS_clock_gettime 113
#define LX_SYS            0x4F32

    .text
    .balign 16
    .global lx_stubs_start, lx_stubs_end
    .global lxs_exit, lxs_putc, lxs_getc, lxs_ready, lxs_idle, lxs_time, lxs_vfs
lx_stubs_start:

/* main() returned here (x30): its value is the exit status */
lxs_exit:
    mov     x8, #SYS_exit_group
    svc     #0
    b       lxs_exit

/* putc(c): one byte to stdout */
lxs_putc:
    sub     sp, sp, #16
    strb    w0, [sp]
1:  mov     x0, #1
    mov     x1, sp
    mov     x2, #1
    mov     x8, #SYS_write
    svc     #0
    cmn     x0, #4                      /* -EINTR: again */
    b.eq    1b
    add     sp, sp, #16
    ret

/* getc(): the next byte from stdin (waits), -1 at its end */
lxs_getc:
    sub     sp, sp, #16
1:  mov     x0, #0
    mov     x1, sp
    mov     x2, #1
    mov     x8, #SYS_read
    svc     #0
    cmn     x0, #4
    b.eq    1b
    cmp     x0, #1
    b.ne    2f
    ldrb    w0, [sp]
    add     sp, sp, #16
    ret
2:  mov     x0, #-1
    add     sp, sp, #16
    ret

/* poll stdin for input, waiting up to x1 nanoseconds: x0 = ready? */
poll_in:
    sub     sp, sp, #32
    mov     w2, #1                      /* struct pollfd { 0, POLLIN, 0 } */
    str     wzr, [sp]
    strh    w2, [sp, #4]
    strh    wzr, [sp, #6]
    str     xzr, [sp, #16]              /* struct timespec { 0, x1 } */
    str     x1, [sp, #24]
    mov     x0, sp
    mov     x1, #1
    add     x2, sp, #16
    mov     x3, #0
    mov     x4, #0
    mov     x8, #SYS_ppoll
    svc     #0
    cmp     x0, #0
    cset    x0, gt
    add     sp, sp, #32
    ret

/* ready(): is there input? */
lxs_ready:
    stp     x29, x30, [sp, #-16]!
    mov     x1, #0
    bl      poll_in
    ldp     x29, x30, [sp], #16
    ret

/* idle(): wait 5 ms, or until there's input */
lxs_idle:
    stp     x29, x30, [sp, #-16]!
    movz    x1, #0x4B40                 /* 5000000 ns */
    movk    x1, #0x4C, lsl #16
    bl      poll_in
    ldp     x29, x30, [sp], #16
    ret

/* time(): seconds since 1970 */
lxs_time:
    sub     sp, sp, #16
    mov     x0, #0                      /* CLOCK_REALTIME */
    mov     x1, sp
    mov     x8, #SYS_clock_gettime
    svc     #0
    ldr     x0, [sp]
    add     sp, sp, #16
    ret

/* the file system calls (and make_exec): lxs_vfs + 8 * op sets x7 = op */
    .balign 8
lxs_vfs:
    .irp op, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    mov     x7, #\op
    b       lx_sys
    .endr
lx_sys:
    mov     x8, #LX_SYS
    svc     #0
    ret

lx_stubs_end:
