/* Services the kernel hands an LX program's entry point (x0).
 *
 * LX programs (CMD.EXE, ...) run in the kernel's address space but link
 * their own copy of kernel/src/uart.c, which can only poll the PL011.
 * Console input that needs the kernel's drivers - the USB keyboard - comes
 * through this table instead. Fields may be NULL; programs started some
 * other way get a NULL table and fall back to the UART alone. */
#ifndef LX_SERVICES_H
#define LX_SERVICES_H

#define LX_SERVICES_VERSION 2      /* what the kernel's own RUN (EL1) hands over */
#define LX_SERVICES_USER    3      /* a program running as a process (EL0) */

struct lx_vfs_ops;                 /* lx_vfs.h */

struct lx_services {
    unsigned int version;          /* LX_SERVICES_VERSION */
    int (*kbd_ready)(void);        /* nonzero if kbd_getc() has a byte */
    int (*kbd_getc)(void);         /* next byte, or -1 */
    /* version 2: */
    const struct lx_vfs_ops *vfs;  /* the kernel's file systems (mounts included) */
    const char *cwd;               /* the starting program's current directory */
    /* version 3 - the program is a process of its own (kernel/src/linux,
     * lx_user.c): no UART, no timers, no cache maintenance of its own.
     * Console output and input are its stdout and stdin. */
    void (*putc)(char c);                           /* to stdout */
    void (*idle)(void);                             /* wait a little (5 ms, or until input) */
    unsigned long (*time)(void);                    /* seconds since 1970 */
    int (*make_exec)(void *addr, unsigned long len); /* code it copied in: executable */
    void (*exit)(int status);                       /* (stdin ended: nothing more to do) */
};

/* kernel/src/uart.c (non-kernel builds): extra console input source. */
void uart_set_input_services(const struct lx_services *svc);
/* ... and those services, when the program runs as a process (version 3),
 * else NULL. */
const struct lx_services *uart_user_services(void);

#endif
