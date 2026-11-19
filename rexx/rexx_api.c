#include "rexx.h"
#include "rexx_api.h"
#include "os2_api.h"

/* Forward declarations */
extern void runtime_init(Context* ctx);
extern AST* parse_statement(Parser* p);
extern void eval(Context* ctx, AST* node);

/* Minimal DOS API hooks used by the current REXX runtime. */
static void DosExecPgm(const char* program)
{
    (void)program;
    /* Program execution is not available in the current runtime. */
}

static void DosExit(int action, int result)
{
    (void)action;
    (void)result;
    /* The runtime does not support process exit yet. */
}

__attribute__((used))
void rexx_execute(const char* script)
{
    Parser parser;
    parser_init(&parser, script);

    Context ctx;
    runtime_init(&ctx);

    while (1) {
        AST* stmt = parse_statement(&parser);
        if (!stmt)
            break;

        eval(&ctx, stmt);
    }

}

int rexx_write(Context* ctx, AST* args)
{
    const char* str = args->value;
    ULONG written;
    DosWrite(1, str, args->length, &written);
    return 0;
}

int rexx_exec(Context* ctx, AST* args)
{
    DosExecPgm(args->value);
    return 0;
}

int rexx_exit(Context* ctx, AST* args)
{
    DosExit(0, 0);
    return 0;
}
