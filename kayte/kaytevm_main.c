/**
 * @file kaytevm_main.c
 * @brief Standalone entry point for KAYTEVM.EXE - a Kayte bytecode interpreter
 *
 * Executes .bytecode files produced by the kayte host tool's --compile
 * (kayte/cli.pas TBytecodeGenerator.SaveProgramToFile - see that file for
 * the authoritative on-disk layout this parser mirrors byte-for-byte).
 * Opcodes follow kayte/bytecodetypes.pas's TByteCodeOp; no Pascal VM in
 * this repo actually executes that opcode set today (bytecodevm.pas
 * targets a different, incompatible opcode/value format and needs
 * LCL Forms/Dialogs; virtualmachine.pas is a stub that never inspects
 * an instruction's opcode at all) - this is the real thing, and it runs
 * on the target, not the build host.
 *
 * Wrapped as a genuine OS/2 LX KAYTEVM.EXE (see the KAYTEVM_EXE build
 * target in the top-level CMakeLists.txt), loadable by the kernel's
 * built-in LX loader the same way CMD.EXE/HELLO.EXE are: `RUN KAYTEVM.EXE`.
 * The loader's entry point takes no arguments (int(*)(void), see
 * kernel/src/lx_loader.c) so there's no way to pass a filename in -
 * KAYTEVM.EXE always runs a fixed file, KAYTE.BC, resolved relative to
 * its (root) current directory.
 *
 * DosOpen/DosRead/DosClose here are resolved through DOSCALLS.DLL - a
 * real OS/2 LX DLL that KAYTEVM.EXE embeds a build-time copy of
 * (os2api/doscalls_embed.S.in) and loads via the kernel's built-in DLL
 * loader (kernel/src/lx_dll_loader.c) - see os2api/os2_api_import.h/.c.
 * The DLL owns its own private vfs.c/blkdev.c/ext4.c/btrfs.c state and
 * mounts it lazily, on whichever Dos* call touches storage first - this
 * program's own mem_init() below is unrelated, just this VM's own working
 * memory (bytecode, strings, variables).
 */

#include "dos.h"
#include "uart.h"
#include "kprintf.h"
#include "os2_api_import.h"
#include "string.h"

/* types.h leaves uint64_t undefined under this kernel's -nostdinc build
 * (same workaround as fs/btrfs/btrfs_disk.h and kernel/src/lx_loader.c). */
#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

extern void  mem_init(uintptr_t start, size_t size);
extern void *mem_alloc(size_t size);
extern void  mem_free(void *ptr);

/* Private heap, same convention as CMD.EXE (dos/cmd_main.c): placed just
 * past the loader's LX_LOAD_BASE/LX_LOAD_MAX_SIZE window so it can't
 * collide with this program's own loaded code. Only one LX app is ever
 * resident at a time in this project, so reusing the same address is
 * safe. */
#define KAYTEVM_HEAP_START 0x49000000UL
#define KAYTEVM_HEAP_SIZE  (4u * 1024u * 1024u)

#define KAYTE_BYTECODE_FILE "KAYTE.BC"
#define KAYTE_FILE_MAX      (64u * 1024u)

/* ── kayte/bytecodetypes.pas: TByteCodeOp (ordinal order matters - this
 * mirrors it exactly, since that ordinal is what's on disk) ──────────── */
enum {
    BC_NOP = 0, BC_HALT, BC_LOAD_INT, BC_LOAD_STRING, BC_LOAD_VAR,
    BC_STORE_VAR, BC_ASSIGN, BC_ADD, BC_SUB, BC_MUL, BC_DIV, BC_PRINT,
    BC_INPUT, BC_JUMP, BC_JUMP_IF_FALSE, BC_CALL, BC_RETURN,
    BC_POP, BC_NEG, BC_NOT, BC_EQ, BC_NEQ, BC_GT, BC_GE, BC_LT, BC_LE
};

typedef struct {
    int32_t opcode;
    int32_t operand1;
    int32_t operand2;
    int32_t operand3;
} kbc_instruction_t;

typedef enum { KV_NULL = 0, KV_INT, KV_STRING } kayte_value_kind_t;

typedef struct {
    kayte_value_kind_t kind;
    int64_t     as_int;
    const char *as_str;
} kayte_value_t;

typedef struct {
    char              *title;
    kbc_instruction_t *instructions;
    int32_t            instruction_count;
    char             **string_literals;
    int32_t            string_count;
    int64_t           *int_literals;
    int32_t            int_count;
    int32_t            variable_count;
} kayte_program_t;

/* ── Bytecode file parsing ─────────────────────────────────────────────
 * Mirrors kayte/cli.pas TBytecodeGenerator.SaveProgramToFile: every
 * integer is a 4-byte little-endian value (native on both this AArch64
 * target and the macOS/Apple Silicon host that produces these files, so
 * no byteswap is needed), and every string is a 4-byte length prefix
 * followed by raw bytes (no NUL, no quotes).
 */

typedef struct {
    const uint8_t *data;
    uint32_t       len;
    uint32_t       pos;
    int            error;
} kbc_reader_t;

static int32_t rd_i32(kbc_reader_t *r)
{
    int32_t v;
    if (r->error || r->pos + 4 > r->len) { r->error = 1; return 0; }
    memcpy(&v, r->data + r->pos, 4);
    r->pos += 4;
    return v;
}

static int64_t rd_i64(kbc_reader_t *r)
{
    int64_t v;
    if (r->error || r->pos + 8 > r->len) { r->error = 1; return 0; }
    memcpy(&v, r->data + r->pos, 8);
    r->pos += 8;
    return v;
}

/* Reads a length-prefixed string as a freshly heap-allocated, NUL-
 * terminated C string (never freed - this VM runs one program, then the
 * whole heap goes away when the LX image is unloaded). */
static char *rd_string(kbc_reader_t *r)
{
    int32_t len = rd_i32(r);
    if (r->error || len < 0 || (uint32_t)len > r->len - r->pos) {
        r->error = 1;
        return NULL;
    }
    char *s = (char *)mem_alloc((size_t)len + 1);
    if (!s) { r->error = 1; return NULL; }
    if (len > 0)
        memcpy(s, r->data + r->pos, (size_t)len);
    s[len] = '\0';
    r->pos += (uint32_t)len;
    return s;
}

/* Reads and discards a length-prefixed string (used for map keys the VM
 * doesn't need at runtime - see kayte_program_load's comment). */
static void rd_string_skip(kbc_reader_t *r)
{
    int32_t len = rd_i32(r);
    if (r->error || len < 0 || (uint32_t)len > r->len - r->pos) {
        r->error = 1;
        return;
    }
    r->pos += (uint32_t)len;
}

/* Parses the whole KBC file into `prog`. Only the pieces the VM actually
 * executes are kept (title, instructions, string/int literal pools, and
 * the variable count, used to size the variable slot table). Variable
 * slot *indices* are assigned sequentially by the compiler
 * (kayte/parser.pas's Op_Variable -> TByteCodeProgram.AddVariable), so
 * operand values are already 0..variable_count-1 - the map's names and
 * the subroutine/form maps aren't needed to execute the opcode set
 * below, but are still walked so a truncated tail is caught. Returns 1
 * on success, 0 on a truncated/malformed file. */
static int kayte_program_load(kayte_program_t *prog, const uint8_t *data, uint32_t len)
{
    kbc_reader_t r;
    int32_t i, sub_count, form_count;

    r.data = data; r.len = len; r.pos = 0; r.error = 0;

    prog->title = rd_string(&r);

    prog->instruction_count = rd_i32(&r);
    if (!r.error && prog->instruction_count > 0) {
        prog->instructions = (kbc_instruction_t *)mem_alloc(
            (size_t)prog->instruction_count * sizeof(kbc_instruction_t));
        if (!prog->instructions) r.error = 1;
        for (i = 0; !r.error && i < prog->instruction_count; i++) {
            prog->instructions[i].opcode   = rd_i32(&r);
            prog->instructions[i].operand1 = rd_i32(&r);
            prog->instructions[i].operand2 = rd_i32(&r);
            prog->instructions[i].operand3 = rd_i32(&r);
        }
    }

    prog->string_count = rd_i32(&r);
    if (!r.error && prog->string_count > 0) {
        prog->string_literals = (char **)mem_alloc((size_t)prog->string_count * sizeof(char *));
        if (!prog->string_literals) r.error = 1;
        for (i = 0; !r.error && i < prog->string_count; i++)
            prog->string_literals[i] = rd_string(&r);
    }

    prog->int_count = rd_i32(&r);
    if (!r.error && prog->int_count > 0) {
        prog->int_literals = (int64_t *)mem_alloc((size_t)prog->int_count * sizeof(int64_t));
        if (!prog->int_literals) r.error = 1;
        for (i = 0; !r.error && i < prog->int_count; i++)
            prog->int_literals[i] = rd_i64(&r);
    }

    prog->variable_count = rd_i32(&r);
    for (i = 0; !r.error && i < prog->variable_count; i++) {
        rd_string_skip(&r);  /* variable name */
        rd_i32(&r);          /* variable index (== i, by construction) */
    }

    /* SubroutineMap, then FormMap - not used by this VM (BC_CALL treats
     * its operand as a direct instruction index), but still parsed so a
     * truncated tail is caught rather than silently ignored. */
    sub_count = rd_i32(&r);
    for (i = 0; !r.error && i < sub_count; i++) { rd_string_skip(&r); rd_i32(&r); }
    form_count = rd_i32(&r);
    for (i = 0; !r.error && i < form_count; i++) { rd_string_skip(&r); rd_i32(&r); }

    return !r.error;
}

/* ── Value helpers ─────────────────────────────────────────────────── */

static kayte_value_t vnull(void)         { kayte_value_t v; v.kind = KV_NULL;   v.as_int = 0; v.as_str = NULL; return v; }
static kayte_value_t vint(int64_t i)     { kayte_value_t v; v.kind = KV_INT;    v.as_int = i;  v.as_str = NULL; return v; }
static kayte_value_t vstr(const char *s) { kayte_value_t v; v.kind = KV_STRING; v.as_int = 0;  v.as_str = s;    return v; }

static int value_truthy(kayte_value_t v)
{
    switch (v.kind) {
        case KV_INT:    return v.as_int != 0;
        case KV_STRING: return v.as_str && v.as_str[0] != '\0';
        default:        return 0;
    }
}

static int value_equal(kayte_value_t a, kayte_value_t b)
{
    if (a.kind != b.kind)
        return 0;
    switch (a.kind) {
        case KV_INT:    return a.as_int == b.as_int;
        case KV_STRING: return strcmp(a.as_str ? a.as_str : "", b.as_str ? b.as_str : "") == 0;
        default:        return 1; /* both KV_NULL */
    }
}

/* Ordering compare for GT/GE/LT/LE: only defined between two ints or two
 * strings (lexicographic). Anything else (mismatched types, either side
 * null) is reported as incomparable via *ok = 0, and the VM treats that
 * as false rather than raising an error. */
static int value_compare(kayte_value_t a, kayte_value_t b, int *ok)
{
    *ok = 1;
    if (a.kind == KV_INT && b.kind == KV_INT) {
        if (a.as_int < b.as_int) return -1;
        if (a.as_int > b.as_int) return 1;
        return 0;
    }
    if (a.kind == KV_STRING && b.kind == KV_STRING)
        return strcmp(a.as_str ? a.as_str : "", b.as_str ? b.as_str : "");
    *ok = 0;
    return 0;
}

static void format_int(int64_t value, char *out, size_t outsz)
{
    char tmp[24];
    int  n = 0;
    int  neg = 0;

    if (value < 0) { neg = 1; value = -value; }
    if (value == 0) tmp[n++] = '0';
    while (value > 0 && (size_t)n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    if (neg && (size_t)n < sizeof(tmp)) tmp[n++] = '-';

    size_t o = 0;
    while (n > 0 && o < outsz - 1) out[o++] = tmp[--n];
    out[o] = '\0';
}

/* Returns a printable C string for the value - literal for strings,
 * freshly formatted (into `scratch`) for integers/null. */
static const char *value_to_cstring(kayte_value_t v, char *scratch, size_t scratch_size)
{
    switch (v.kind) {
        case KV_STRING: return v.as_str ? v.as_str : "";
        case KV_INT:    format_int(v.as_int, scratch, scratch_size); return scratch;
        default:        return "";
    }
}

/* ── VM ────────────────────────────────────────────────────────────── */

#define KAYTE_STACK_MAX     256
#define KAYTE_CALLSTACK_MAX 64

typedef struct {
    kayte_program_t *prog;
    kayte_value_t     stack[KAYTE_STACK_MAX];
    int32_t           sp;
    int32_t           call_stack[KAYTE_CALLSTACK_MAX];
    int32_t           csp;
    kayte_value_t    *variables;
} kayte_vm_t;

static void vm_push(kayte_vm_t *vm, kayte_value_t v)
{
    if (vm->sp >= KAYTE_STACK_MAX) {
        kprintf("[KAYTEVM] Stack overflow\n");
        return;
    }
    vm->stack[vm->sp++] = v;
}

static kayte_value_t vm_pop(kayte_vm_t *vm)
{
    if (vm->sp <= 0) {
        kprintf("[KAYTEVM] Stack underflow\n");
        return vnull();
    }
    return vm->stack[--vm->sp];
}

static void vm_run(kayte_vm_t *vm)
{
    kayte_program_t *prog = vm->prog;
    int32_t ip = 0;

    while (ip < prog->instruction_count) {
        kbc_instruction_t in = prog->instructions[ip];
        int32_t this_ip = ip;
        ip++;

        switch (in.opcode) {
        case BC_NOP:
            break;
        case BC_HALT:
            return;
        case BC_LOAD_INT:
            if (in.operand1 < 0 || in.operand1 >= prog->int_count) {
                kprintf("[KAYTEVM] Bad int literal index %d at %d\n", in.operand1, this_ip);
                return;
            }
            vm_push(vm, vint(prog->int_literals[in.operand1]));
            break;
        case BC_LOAD_STRING:
            if (in.operand1 < 0 || in.operand1 >= prog->string_count) {
                kprintf("[KAYTEVM] Bad string literal index %d at %d\n", in.operand1, this_ip);
                return;
            }
            vm_push(vm, vstr(prog->string_literals[in.operand1]));
            break;
        case BC_LOAD_VAR:
            if (in.operand1 < 0 || in.operand1 >= prog->variable_count) {
                kprintf("[KAYTEVM] Bad variable index %d at %d\n", in.operand1, this_ip);
                return;
            }
            vm_push(vm, vm->variables[in.operand1]);
            break;
        case BC_STORE_VAR:
        case BC_ASSIGN:
            if (in.operand1 < 0 || in.operand1 >= prog->variable_count) {
                kprintf("[KAYTEVM] Bad variable index %d at %d\n", in.operand1, this_ip);
                return;
            }
            vm->variables[in.operand1] = vm_pop(vm);
            break;
        case BC_ADD: {
            kayte_value_t b = vm_pop(vm);
            kayte_value_t a = vm_pop(vm);
            if (a.kind == KV_STRING || b.kind == KV_STRING) {
                char sa[24], sb[24];
                const char *ca = value_to_cstring(a, sa, sizeof(sa));
                const char *cb = value_to_cstring(b, sb, sizeof(sb));
                size_t la = strlen(ca), lb = strlen(cb);
                char *out = (char *)mem_alloc(la + lb + 1);
                if (out) {
                    memcpy(out, ca, la);
                    memcpy(out + la, cb, lb);
                    out[la + lb] = '\0';
                }
                vm_push(vm, vstr(out ? out : ""));
            } else {
                vm_push(vm, vint(a.as_int + b.as_int));
            }
            break;
        }
        case BC_SUB: { kayte_value_t b = vm_pop(vm), a = vm_pop(vm); vm_push(vm, vint(a.as_int - b.as_int)); break; }
        case BC_MUL: { kayte_value_t b = vm_pop(vm), a = vm_pop(vm); vm_push(vm, vint(a.as_int * b.as_int)); break; }
        case BC_DIV: {
            kayte_value_t b = vm_pop(vm), a = vm_pop(vm);
            if (b.as_int == 0) {
                kprintf("[KAYTEVM] Division by zero at %d\n", this_ip);
                vm_push(vm, vint(0));
            } else {
                vm_push(vm, vint(a.as_int / b.as_int));
            }
            break;
        }
        case BC_PRINT: {
            /* Prints everything currently on the stack, bottom-to-top,
             * concatenated with no separator - matches how PrintStatement
             * (kayte/parser.pas) emits one LOAD per comma-separated
             * argument followed by a single trailing BC_PRINT. */
            char scratch[24];
            int32_t i;
            for (i = 0; i < vm->sp; i++)
                uart_puts(value_to_cstring(vm->stack[i], scratch, sizeof(scratch)));
            vm->sp = 0;
            uart_puts("\r\n");
            break;
        }
        case BC_INPUT: {
            char line[256];
            if (in.operand1 < 0 || in.operand1 >= prog->variable_count) {
                kprintf("[KAYTEVM] Bad variable index %d at %d\n", in.operand1, this_ip);
                return;
            }
            dos_read_line("", line, sizeof(line), NULL);
            char *s = (char *)mem_alloc(strlen(line) + 1);
            if (s) strcpy(s, line);
            vm->variables[in.operand1] = vstr(s ? s : "");
            break;
        }
        case BC_JUMP:
            if (in.operand1 < 0 || in.operand1 > prog->instruction_count) {
                kprintf("[KAYTEVM] Bad jump target %d at %d\n", in.operand1, this_ip);
                return;
            }
            ip = in.operand1;
            break;
        case BC_JUMP_IF_FALSE: {
            kayte_value_t cond = vm_pop(vm);
            if (in.operand1 < 0 || in.operand1 > prog->instruction_count) {
                kprintf("[KAYTEVM] Bad jump target %d at %d\n", in.operand1, this_ip);
                return;
            }
            if (!value_truthy(cond))
                ip = in.operand1;
            break;
        }
        case BC_CALL:
            if (vm->csp >= KAYTE_CALLSTACK_MAX) {
                kprintf("[KAYTEVM] Call stack overflow at %d\n", this_ip);
                return;
            }
            if (in.operand1 < 0 || in.operand1 > prog->instruction_count) {
                kprintf("[KAYTEVM] Bad call target %d at %d\n", in.operand1, this_ip);
                return;
            }
            vm->call_stack[vm->csp++] = ip;
            ip = in.operand1;
            break;
        case BC_RETURN:
            if (vm->csp <= 0)
                return; /* No caller to return to - treat like HALT. */
            ip = vm->call_stack[--vm->csp];
            break;
        case BC_POP:
            vm_pop(vm);
            break;
        case BC_NEG: {
            kayte_value_t a = vm_pop(vm);
            if (a.kind != KV_INT) {
                kprintf("[KAYTEVM] Cannot negate a non-integer at %d\n", this_ip);
                vm_push(vm, vint(0));
            } else {
                vm_push(vm, vint(-a.as_int));
            }
            break;
        }
        case BC_NOT: {
            kayte_value_t a = vm_pop(vm);
            vm_push(vm, vint(value_truthy(a) ? 0 : 1));
            break;
        }
        case BC_EQ: {
            kayte_value_t b = vm_pop(vm), a = vm_pop(vm);
            vm_push(vm, vint(value_equal(a, b) ? 1 : 0));
            break;
        }
        case BC_NEQ: {
            kayte_value_t b = vm_pop(vm), a = vm_pop(vm);
            vm_push(vm, vint(value_equal(a, b) ? 0 : 1));
            break;
        }
        case BC_GT: case BC_GE: case BC_LT: case BC_LE: {
            kayte_value_t b = vm_pop(vm), a = vm_pop(vm);
            int ok, cmp = value_compare(a, b, &ok);
            int result;
            if (!ok) {
                result = 0;
            } else {
                switch (in.opcode) {
                    case BC_GT: result = cmp >  0; break;
                    case BC_GE: result = cmp >= 0; break;
                    case BC_LT: result = cmp <  0; break;
                    default:    result = cmp <= 0; break; /* BC_LE */
                }
            }
            vm_push(vm, vint(result ? 1 : 0));
            break;
        }
        default:
            kprintf("[KAYTEVM] Unknown opcode %d at %d\n", in.opcode, this_ip);
            return;
        }
    }
}

int main(void)
{
    HFILE h;
    uint32_t action, total, got;
    APIRET rc;
    uint8_t *filebuf;
    kayte_program_t prog;
    kayte_vm_t vm;
    int32_t i;

    mem_init(KAYTEVM_HEAP_START, KAYTEVM_HEAP_SIZE);

    uart_puts("\r\nOS/2 Warp ARM64 - KAYTEVM\r\n");

    if (os2_api_import_init() != 0) {
        uart_puts("KAYTEVM: failed to load DOSCALLS.DLL\r\n");
        return 1;
    }

    rc = DosOpen(KAYTE_BYTECODE_FILE, &h, &action, 0, 0, 0, O_RDONLY);
    if (rc != NO_ERROR) {
        kprintf("KAYTEVM: could not open %s (error %u)\n", KAYTE_BYTECODE_FILE, (unsigned)rc);
        return 1;
    }

    filebuf = (uint8_t *)mem_alloc(KAYTE_FILE_MAX);
    if (!filebuf) {
        kprintf("KAYTEVM: out of memory reading %s\n", KAYTE_BYTECODE_FILE);
        DosClose(h);
        return 1;
    }

    total = 0;
    while (total < KAYTE_FILE_MAX &&
           DosRead(h, filebuf + total, KAYTE_FILE_MAX - total, &got) == NO_ERROR && got > 0) {
        total += got;
    }
    DosClose(h);

    memset(&prog, 0, sizeof(prog));
    if (!kayte_program_load(&prog, filebuf, total)) {
        kprintf("KAYTEVM: %s is truncated or malformed\n", KAYTE_BYTECODE_FILE);
        return 1;
    }

    memset(&vm, 0, sizeof(vm));
    vm.prog = &prog;
    vm.variables = (prog.variable_count > 0)
        ? (kayte_value_t *)mem_alloc((size_t)prog.variable_count * sizeof(kayte_value_t))
        : NULL;
    for (i = 0; i < prog.variable_count; i++)
        vm.variables[i] = vnull();

    vm_run(&vm);

    uart_puts("KAYTEVM: program finished\r\n");
    return 0;
}
