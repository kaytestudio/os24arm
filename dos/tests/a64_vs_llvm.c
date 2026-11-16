/* dos/tests/a64_vs_llvm.c - dos/a64.c's disassembly of every instruction
 * llvm-objdump printed (its output on stdin: "addr: word \tmnemonic\toperands")
 * against LLVM's own text, LLVM's "<symbol>" and "// comment" left out. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a64.h"

static void norm(char *s)
{
    char *c;
    if ((c = strstr(s, " <"))) *c = 0;
    if ((c = strstr(s, "//"))) *c = 0;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n')) s[--n] = 0;
}
/* not covered: FP/SIMD data processing, and SIMD structure loads/stores (ld1-ld4, st1-st4) */
static int is_simd_dp(unsigned w)
{
    unsigned op0 = (w >> 25) & 15;
    if ((op0 & 7) == 7) return 1;
    return (op0 & 5) == 4 && ((w >> 26) & 1) && ((w >> 27) & 7) == 1 && !((w >> 31) & 1);
}

int main(int argc, char **argv)
{
    (void)argv;
    char line[512], mine[256];
    long total = 0, same = 0, simd = 0;
    struct { char mn[24]; long n; char ex[200]; } bad[256];
    int nbad = 0;
    int verbose = argc > 1;
    while (fgets(line, sizeof line, stdin)) {
        unsigned long long addr; unsigned w;
        char *tab = strchr(line, '\t');
        if (!tab || sscanf(line, " %llx: %x", &addr, &w) != 2) continue;
        char *theirs = tab + 1;
        norm(theirs);
        if (!strncmp(theirs, "<unknown>", 9) || !strncmp(theirs, ".word", 5) || !*theirs) continue;   /* (data) */
        total++;
        a64_disasm(w, addr, mine, sizeof mine);
        norm(mine);
        if (is_simd_dp(w)) { simd++; continue; }
        if (!strcmp(mine, theirs)) { same++; continue; }
        char mnem[24]; int k = 0;
        while (theirs[k] && theirs[k] != '\t' && k < 23) { mnem[k] = theirs[k]; k++; }
        mnem[k] = 0;
        int j;
        for (j = 0; j < nbad && strcmp(bad[j].mn, mnem); j++) { }
        if (j == nbad && nbad < 256) { strcpy(bad[j].mn, mnem); bad[j].n = 0; snprintf(bad[j].ex, sizeof bad[j].ex, "%08x  llvm: %s  |  a64: %s", w, theirs, mine); nbad++; }
        if (j < nbad) bad[j].n++;
        if (verbose) printf("%08x  llvm: %-40s a64: %s\n", w, theirs, mine);
    }
    long cmp = total - simd;
    printf("integer and load/store: %ld of %ld the same (%.2f%%); FP/SIMD data processing (not covered): %ld\n",
           same, cmp, cmp ? 100.0 * same / cmp : 0.0, simd);
    for (int a = 0; a < nbad; a++) for (int c = a + 1; c < nbad; c++) if (bad[c].n > bad[a].n) { __typeof__(bad[0]) t = bad[a]; bad[a] = bad[c]; bad[c] = t; }
    for (int a = 0; a < nbad && a < (getenv("TOP") ? atoi(getenv("TOP")) : 40); a++) printf("%6ld %-10s %s\n", bad[a].n, bad[a].mn, bad[a].ex);
    return same == cmp ? 0 : 1;
}
