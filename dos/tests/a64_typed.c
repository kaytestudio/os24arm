/* dos/tests/a64_typed.c - lines as someone would type them at DEBUG's A
 * prompt (stdin, one a line: "address<TAB>instruction"), assembled by
 * dos/a64.c: "address word" for each, or "address error: why". */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "a64.h"
int main(void)
{
    char line[256], err[128];
    while (fgets(line, sizeof line, stdin)) {
        unsigned long long pc; char *tab = strchr(line, '\t');
        if (!tab || sscanf(line, "%llx", &pc) != 1) continue;
        line[strcspn(line, "\n")] = 0;
        uint32_t w;
        if (a64_asm(tab + 1, pc, &w, err, sizeof err)) printf("%08x\n", w); else printf("error: %s\n", err);
    }
    return 0;
}
