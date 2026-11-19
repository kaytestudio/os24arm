/**
 * @file ai.c
 * @brief The AI command: command usage and tips (see kernel/include/ai.h)
 */

#include "ai.h"
#include "types.h"
#include "uart.h"
#include "kprintf.h"
#include "string.h"

extern void mem_get_stats(uintptr_t *used, uintptr_t *total);

/* Commands with a tip. AI, HELP, CHKDSK, FORMAT and EXIT are left out: nobody
 * needs a tip for those. */
typedef struct {
    const char *name;
    const char *tip;
} known_cmd_t;

static const known_cmd_t known_commands[] = {
    { "VER",    "Type VER to see the kernel/build version banner." },
    { "DIR",    "Type DIR to list files in the current directory." },
    { "TYPE",   "Type TYPE <file> to print a file's contents." },
    { "COPY",   "Type COPY <src> <dst> to duplicate a file." },
    { "DEL",    "Type DEL <file> (or ERASE) to remove a file." },
    { "REN",    "Type REN <old> <new> (or RENAME) to rename a file." },
    { "MD",     "Type MD <dir> (or MKDIR) to create a directory." },
    { "RD",     "Type RD <dir> (or RMDIR) to remove an empty directory." },
    { "CD",     "Type CD <dir> (or CHDIR), or just CD alone to show the current directory." },
    { "VOL",    "Type VOL to see the volume label." },
    { "ATTRIB", "Type ATTRIB <file> to see a file's attributes." },
    { "ECHO",   "Type ECHO <text> to print text back to the console." },
    { "CLS",    "Type CLS to clear the screen." },
    { "MEM",    "Type MEM to check heap usage." },
    { "IFS",    "Type IFS to list the loaded filesystem drivers." },
    { "RUN",    "Type RUN <file.EXE> to load and execute an OS/2 LX binary." },
    { "BASIC",  "Type BASIC to drop into the built-in QuickBASIC interpreter." },
    { "GUI",    "Type GUI to start the Workplace Shell desktop." },
};
#define NUM_KNOWN_COMMANDS (int)(sizeof(known_commands) / sizeof(known_commands[0]))

/* Shown once every command above has been used. */
static const char *generic_tips[] = {
    "The kernel heap is a bump allocator and never shrinks. MEM shows how full it is.",
    "In the GUI, Cmd+Escape switches between the 4 workplaces.",
    "RUN runs an OS/2 LX .EXE, e.g. RUN CMD.EXE for the standalone command interpreter.",
};
#define NUM_GENERIC_TIPS (int)(sizeof(generic_tips) / sizeof(generic_tips[0]))

/* ── Session-only usage tracking ─────────────────────────────────────────── */

#define MAX_TRACKED 24

typedef struct {
    const char *name; /* points at known_commands[i].name, not argv[0],
                        * which the shell overwrites on the next line */
    uint32_t    count;
} tracked_cmd_t;

static tracked_cmd_t tracked[MAX_TRACKED];
static int tracked_count = 0;
static uint32_t total_commands = 0;
static int next_generic_tip = 0;

void ai_record_command(const char *name)
{
    int idx = -1;
    for (int i = 0; i < NUM_KNOWN_COMMANDS; i++) {
        if (strcmp(name, known_commands[i].name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;

    const char *canonical = known_commands[idx].name;
    total_commands++;

    for (int i = 0; i < tracked_count; i++) {
        if (tracked[i].name == canonical) {
            tracked[i].count++;
            return;
        }
    }

    if (tracked_count < MAX_TRACKED) {
        tracked[tracked_count].name = canonical;
        tracked[tracked_count].count = 1;
        tracked_count++;
    }
}

static uint32_t tracked_count_for(const char *name)
{
    for (int i = 0; i < tracked_count; i++) {
        if (strcmp(tracked[i].name, name) == 0)
            return tracked[i].count;
    }
    return 0;
}

/* An unused command's tip first, then the generic tips in turn. */
static const char *next_tip(void)
{
    for (int i = 0; i < NUM_KNOWN_COMMANDS; i++) {
        if (tracked_count_for(known_commands[i].name) == 0)
            return known_commands[i].tip;
    }

    const char *tip = generic_tips[next_generic_tip];
    next_generic_tip = (next_generic_tip + 1) % NUM_GENERIC_TIPS;
    return tip;
}

/* ── Subcommands ─────────────────────────────────────────────────────────── */

static void ai_help(void)
{
    uart_puts("\r\n");
    uart_puts("AI - which commands you've used this session, and tips\r\n");
    uart_puts("\r\n");
    uart_puts("  AI STATUS  - session summary and a suggestion\r\n");
    uart_puts("  AI STATS   - how often each command was used\r\n");
    uart_puts("  AI TIP     - a tip, starting with commands you haven't used\r\n");
    uart_puts("  AI HELP    - this text\r\n");
    uart_puts("\r\n");
    uart_puts("The counts start over at each boot.\r\n");
    uart_puts("\r\n");
}

static void ai_status(void)
{
    uintptr_t used, total;
    mem_get_stats(&used, &total);
    uint32_t pct = (total > 0) ? (uint32_t)((used * 100) / total) : 0;

    const char *top_name = NULL;
    uint32_t top_count = 0;
    for (int i = 0; i < tracked_count; i++) {
        if (tracked[i].count > top_count) {
            top_count = tracked[i].count;
            top_name = tracked[i].name;
        }
    }

    uart_puts("\r\n");
    uart_puts("Session summary\r\n");
    uart_puts("\r\n");
    kprintf("  Commands run:            %u\r\n", total_commands);
    kprintf("  Distinct commands tried: %d / %d known\r\n", tracked_count, NUM_KNOWN_COMMANDS);
    kprintf("  Heap usage:              %u%% (%u of %u bytes)\r\n",
            pct, (uint32_t)used, (uint32_t)total);
    if (top_name) {
        uart_puts("  Most-used command:       ");
        uart_puts(top_name);
        kprintf(top_count == 1 ? " (once)\r\n" : " (%u times)\r\n", top_count);
    }
    uart_puts("\r\n");
    uart_puts("  Tip: ");
    uart_puts(next_tip());
    uart_puts("\r\n\r\n");
}

static void ai_stats(void)
{
    uart_puts("\r\n");
    if (tracked_count == 0) {
        uart_puts("No commands used yet.\r\n\r\n");
        return;
    }

    /* at most MAX_TRACKED entries: insertion sort by count */
    tracked_cmd_t sorted[MAX_TRACKED];
    memcpy(sorted, tracked, sizeof(tracked_cmd_t) * (size_t)tracked_count);
    for (int i = 1; i < tracked_count; i++) {
        tracked_cmd_t key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j].count < key.count) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    uart_puts("Command usage this session:\r\n\r\n");
    for (int i = 0; i < tracked_count; i++) {
        uart_puts("  ");
        uart_puts(sorted[i].name);
        for (size_t p = strlen(sorted[i].name); p < 8; p++)
            uart_putc(' ');
        kprintf("%u\r\n", sorted[i].count);
    }
    uart_puts("\r\n");
}

static void ai_tip(void)
{
    uart_puts("\r\n");
    uart_puts(next_tip());
    uart_puts("\r\n\r\n");
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

static void str_toupper_local(char *s)
{
    while (*s) {
        if (*s >= 'a' && *s <= 'z')
            *s = *s - 'a' + 'A';
        s++;
    }
}

void ai_run(int argc, char **argv)
{
    if (argc < 2) {
        ai_status();
        return;
    }

    str_toupper_local(argv[1]);

    if (strcmp(argv[1], "STATUS") == 0) {
        ai_status();
    } else if (strcmp(argv[1], "STATS") == 0) {
        ai_stats();
    } else if (strcmp(argv[1], "TIP") == 0) {
        ai_tip();
    } else if (strcmp(argv[1], "HELP") == 0) {
        ai_help();
    } else {
        uart_puts("\r\nUnknown AI subcommand: ");
        uart_puts(argv[1]);
        uart_puts("\r\nType AI HELP for usage.\r\n");
    }
}
