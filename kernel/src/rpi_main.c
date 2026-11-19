#include "os2.h"
#include "keyboard.h"
#include "vfs.h"
#include "blkdev.h"

/* Memory heap configuration for RPi 3+ */
extern uint8_t _bss_end;
#define HEAP_START  ((uintptr_t)&_bss_end + 0x1000u)
#define HEAP_SIZE   (32u * 1024u * 1024u)   /* 32 MB - RPi has 1GB total */

extern void mem_init(uintptr_t start, size_t size);
extern void scheduler_init(void);

/* Raspberry Pi 3+ Hardware Addresses */

/* BCM2837 Peripheral base */
#define BCM2837_PERI_BASE   0x3F000000UL

/* GPIO */
#define GPIO_BASE           (BCM2837_PERI_BASE + 0x200000)
#define GPFSEL1             ((volatile ULONG *)(GPIO_BASE + 0x04))
#define GPPUD               ((volatile ULONG *)(GPIO_BASE + 0x94))
#define GPPUDCLK0           ((volatile ULONG *)(GPIO_BASE + 0x98))

/* PL011 UART0 */
#define UART0_BASE          (BCM2837_PERI_BASE + 0x201000)
#define UART0_DR            ((volatile ULONG *)(UART0_BASE + 0x00))
#define UART0_FR            ((volatile ULONG *)(UART0_BASE + 0x18))
#define UART0_IBRD          ((volatile ULONG *)(UART0_BASE + 0x24))
#define UART0_FBRD          ((volatile ULONG *)(UART0_BASE + 0x28))
#define UART0_LCRH          ((volatile ULONG *)(UART0_BASE + 0x2C))
#define UART0_CR            ((volatile ULONG *)(UART0_BASE + 0x30))
#define UART0_IMSC          ((volatile ULONG *)(UART0_BASE + 0x38))
#define UART0_ICR           ((volatile ULONG *)(UART0_BASE + 0x44))

/* UART FR bits */
#define UART_FR_TXFF        (1u << 5)
#define UART_FR_RXFE        (1u << 4)

/* Delay function */

static void delay(int count)
{
    volatile int i;
    for (i = 0; i < count; i++)
        __asm__ volatile("nop");
}

/* UART Initialization for RPi */

static void uart_init(void)
{
    /* Disable UART */
    *UART0_CR = 0;
    
    /* Setup GPIO pins 14 and 15 for UART */
    ULONG ra = *GPFSEL1;
    ra &= ~((7 << 12) | (7 << 15));  /* Clear bits for GPIO 14 and 15 */
    ra |= (4 << 12) | (4 << 15);     /* Alt function 0 for UART */
    *GPFSEL1 = ra;
    
    /* Disable pull up/down for pins 14 and 15 */
    *GPPUD = 0;
    delay(150);
    *GPPUDCLK0 = (1 << 14) | (1 << 15);
    delay(150);
    *GPPUDCLK0 = 0;
    
    /* Clear pending interrupts */
    *UART0_ICR = 0x7FF;
    
    /* Set baud rate to 115200 */
    /* UART clock = 48MHz, divisor = 48000000 / (16 * 115200) = 26.041666... */
    /* Integer part = 26, fractional part = 0.041666... * 64 = 2.666... ≈ 3 */
    *UART0_IBRD = 26;
    *UART0_FBRD = 3;
    
    /* Enable FIFO, 8-bit data, no parity, 1 stop bit */
    *UART0_LCRH = (1 << 4) | (3 << 5);
    
    /* Mask all interrupts */
    *UART0_IMSC = 0;
    
    /* Enable UART: transmit, receive, and UART enable */
    *UART0_CR = (1 << 0) | (1 << 8) | (1 << 9);
}

/* Low-level UART helpers */

static void uart_putc(char c)
{
    while (*UART0_FR & UART_FR_TXFF)
        ;
    *UART0_DR = (ULONG)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static char uart_getc(void)
{
    while (*UART0_FR & UART_FR_RXFE)
        ;
    return (char)(*UART0_DR & 0xFF);
}

static void uart_puthex(ULONG val)
{
    const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        uart_putc(hex[(val >> shift) & 0xF]);
}

/* String utilities */

static int my_strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static int str_tokenize(char *str, char **tokens, int max_tokens)
{
    int count = 0;
    char *p = str;
    
    while (*p && count < max_tokens) {
        while (*p == ' ' || *p == '\t')
            p++;
        
        if (*p == '\0')
            break;
        
        tokens[count++] = p;
        
        while (*p && *p != ' ' && *p != '\t')
            p++;
        
        if (*p)
            *p++ = '\0';
    }
    
    return count;
}

static void str_toupper(char *s)
{
    while (*s) {
        if (*s >= 'a' && *s <= 'z')
            *s = *s - 'a' + 'A';
        s++;
    }
}

/* Commands */

static void cmd_ver(void)
{
    uart_puts("\r\n");
    uart_puts("OS/2 Warp Version 4.52 (ARM64 Edition)\r\n");
    uart_puts("Platform: Raspberry Pi 3+\r\n");
    uart_puts("CPU: ARM Cortex-A53 (64-bit)\r\n");
    uart_puts("Kernel: Bare Metal Build\r\n");
    uart_puts("\r\n");
}

static void cmd_help(void)
{
    uart_puts("\r\n");
    uart_puts("Available commands:\r\n");
    uart_puts("  VER       - Display OS version\r\n");
    uart_puts("  HELP      - Display this help\r\n");
    uart_puts("  MEM       - Display memory information\r\n");
    uart_puts("  ECHO      - Display message\r\n");
    uart_puts("  EXIT      - Exit to idle loop\r\n");
    uart_puts("\r\n");
}

static void cmd_mem(void)
{
    uart_puts("\r\n");
    uart_puts("Memory Information:\r\n");
    uart_puts("  Heap start: ");
    uart_puthex(HEAP_START);
    uart_puts("\r\n");
    uart_puts("  Heap size:  ");
    uart_puthex(HEAP_SIZE);
    uart_puts(" (32 MB)\r\n");
    uart_puts("  Total RAM:  1 GB (Raspberry Pi 3+)\r\n");
    uart_puts("\r\n");
}

static void cmd_echo(int argc, char **argv)
{
    uart_puts("\r\n");
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            uart_putc(' ');
        uart_puts(argv[i]);
    }
    uart_puts("\r\n");
}

/* Command dispatcher */

static void execute_command(char *cmdline)
{
    char *argv[16];
    int argc = str_tokenize(cmdline, argv, 16);
    
    if (argc == 0)
        return;
    
    str_toupper(argv[0]);
    
    if (my_strcmp(argv[0], "VER") == 0) {
        cmd_ver();
    }
    else if (my_strcmp(argv[0], "HELP") == 0 || my_strcmp(argv[0], "?") == 0) {
        cmd_help();
    }
    else if (my_strcmp(argv[0], "MEM") == 0) {
        cmd_mem();
    }
    else if (my_strcmp(argv[0], "ECHO") == 0) {
        cmd_echo(argc, argv);
    }
    else if (my_strcmp(argv[0], "EXIT") == 0) {
        uart_puts("\r\n[KERNEL] Exiting to idle loop...\r\n\r\n");
        while (1) {
            __asm__ volatile("wfe");
        }
    }
    else {
        uart_puts("\r\nBad command or file name: ");
        uart_puts(argv[0]);
        uart_puts("\r\nType HELP for list of commands\r\n");
    }
}

/* Kernel shell */

static void print_banner(void)
{
    uart_puts("\r\n");
    uart_puts("  ___  ____    ______     _  _  __              \r\n");
    uart_puts(" / _ \\/ ___|  / /___ \\   | || |/ /___ _  __    \r\n");
    uart_puts("| | | \\___ \\ / /  __) |  | || '_// _ \\ \\/ /   \r\n");
    uart_puts("| |_| |___) / /  / __/   |__   _|  __/>  <    \r\n");
    uart_puts(" \\___/|____/_/  |_____|     |_|  \\___/_/\\_\\  \r\n");
    uart_puts("\r\n");
    uart_puts("  OS/2 Warp 4.52  -  ARM64 Bare-Metal Kernel\r\n");
    uart_puts("  Platform : Raspberry Pi 3+\r\n");
    uart_puts("  CPU      : ARM Cortex-A53 (64-bit)\r\n");
    uart_puts("  UART     : PL011 @ ");
    uart_puthex(UART0_BASE);
    uart_puts("\r\n\r\n");
}

static void kernel_shell(void)
{
    char buffer[256];
    int pos = 0;
    
    uart_puts("\r\n");
    uart_puts("OS/2 Command Prompt - Raspberry Pi Edition\r\n");
    uart_puts("Type HELP for available commands\r\n");
    uart_puts("\r\n");
    uart_puts("[C:\\]> ");
    
    while (1) {
        char c = uart_getc();
        
        if (c == '\r' || c == '\n') {
            uart_puts("\r\n");
            
            if (pos > 0) {
                buffer[pos] = '\0';
                execute_command(buffer);
                pos = 0;
            }
            
            uart_puts("[C:\\]> ");
        }
        else if (c == 0x7F || c == 0x08) {
            if (pos > 0) {
                pos--;
                uart_putc('\b');
                uart_putc(' ');
                uart_putc('\b');
            }
        }
        else if (c == 0x03) {
            uart_puts("^C\r\n[C:\\]> ");
            pos = 0;
        }
        else if (c >= 32 && c < 127) {
            if (pos < 255) {
                buffer[pos++] = c;
                uart_putc(c);
            }
        }
    }
}

/* Kernel entry point */

void kernel_main(void)
{
    uart_init();
    
    print_banner();
    
    uart_puts("[KERNEL] Subsystems initializing...\r\n");
    
    mem_init(HEAP_START, HEAP_SIZE);
    uart_puts("[MEM]    Allocator    : initialized\r\n");
    
    scheduler_init();
    uart_puts("[SCHED]  Scheduler    : initialized\r\n");
    
    uart_puts("[IPC]    Semaphores   : stub\r\n");
    
    /* VFS and block devices disabled for initial RPi boot */
    uart_puts("[VFS]    Disabled for initial boot\r\n");
    uart_puts("[BLK]    Disabled for initial boot\r\n");
    
    uart_puts("\r\n[KERNEL] Boot complete on Raspberry Pi 3+\r\n");
    uart_puts("\r\n");
    
    /* Keyboard driver disabled - using direct UART */
    
    kernel_shell();
}
