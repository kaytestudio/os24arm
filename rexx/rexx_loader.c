#include "rexx.h"
#include "../os2/os2_api.h"

/* Load a script through the OS/2 file APIs. */
extern APIRET DosRead(ULONG handle, void* buf, ULONG len, ULONG* read);

void rexx_run_script(const char* path)
{
    Context ctx;
    runtime_init(&ctx);

    ULONG handle;
    if (DosOpen(path, &handle) != 0)
        return;

    static char buffer[4096];
    ULONG read;

    DosRead(handle, buffer, sizeof(buffer)-1, &read);
    buffer[read] = 0;

    DosClose(handle);

    Parser p;
    parser_init(&p, buffer);

    AST* program = parse_program(&p);
    eval(&ctx, program);
}
