#include "rexx.h"
#include <string.h>
#include "orexx.h"

extern int sys_write(Context*, AST*);
extern int sys_exit(Context*, AST*);
extern int sys_exec(Context*, AST*);

void runtime_register_builtin(Context* ctx, const char* name, BuiltinFunc fn)
{
    if (ctx->builtin_count >= MAX_BUILTINS)
        return;

    Builtin* b = &ctx->builtins[ctx->builtin_count++];
    copy_name(b->name, name, 31);
    b->fn = fn;
}

void runtime_init(Context* ctx)
{
    ctx->var_count = 0;
    ctx->proc_count = 0;
    ctx->builtin_count = 0;

    runtime_register_builtin(ctx, "WRITE", sys_write);
    runtime_register_builtin(ctx, "EXIT",  sys_exit);
    runtime_register_builtin(ctx, "EXEC",  sys_exec);

    orexx_init();
}

void copy_name(char* dst, const char* src, size_t len)
{
    size_t i;
    for (i = 0; i < len && i < 31; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

int names_equal(const char* a, const char* b)
{
    size_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

void runtime_set(Context* ctx, const char* name, const char* value)
{
    for (size_t i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->vars[i].name, name) == 0) {
            strncpy(ctx->vars[i].value, value, sizeof(ctx->vars[i].value) - 1);
            ctx->vars[i].value[sizeof(ctx->vars[i].value) - 1] = 0;
            return;
        }
    }

    strncpy(ctx->vars[ctx->var_count].name, name,
        sizeof(ctx->vars[ctx->var_count].name) - 1);
    ctx->vars[ctx->var_count].name[
            sizeof(ctx->vars[ctx->var_count].name) - 1] = 0;
    ctx->var_count++;
}

const char* runtime_get(Context* ctx, const char* name)
{
    for (size_t i = 0; i < ctx->var_count; i++)
        if (strcmp(ctx->vars[i].name, name) == 0)
            return ctx->vars[i].value;

    return "";
}
