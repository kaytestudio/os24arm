/* dos/tests/a64_roundtrip.c - every instruction llvm-objdump printed (on
 * stdin), disassembled by dos/a64.c and assembled back: the same word?
 * (The disassembly being LLVM's own text, the assembler is checked
 * against LLVM's encoding too.) Per mnemonic: the same / different / not
 * assembled. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a64.h"

int main(void)
{
    char line[512], text[256], err[128];
    struct { char mn[16]; long same, diff, none; char ex[160]; } st[300];
    int ns = 0;
    long total = 0, same = 0, diff = 0, none = 0, equiv = 0;
    while (fgets(line, sizeof line, stdin)) {
        unsigned long long addr; unsigned w;
        if (!strchr(line, '\t') || sscanf(line, " %llx: %x", &addr, &w) != 2) continue;
        if (!a64_disasm(w, addr, text, sizeof text)) continue;
        char mn[16]; int k = 0;
        while (text[k] && text[k] != '\t' && k < 15) { mn[k] = text[k]; k++; }
        mn[k] = 0;
        int j;
        for (j = 0; j < ns && strcmp(st[j].mn, mn); j++) { }
        if (j == ns) { if (ns == 300) continue; strcpy(st[j].mn, mn); st[j].same = st[j].diff = st[j].none = 0; st[j].ex[0] = 0; ns++; }
        char flat[256]; int f = 0;
        for (int c = 0; text[c] && f < 255; c++) flat[f++] = text[c] == '\t' ? ' ' : text[c];
        flat[f] = 0;
        uint32_t back;
        total++;
        if (!a64_asm(flat, addr, &back, err, sizeof err)) { none++; st[j].none++; if (!st[j].ex[0]) snprintf(st[j].ex, sizeof st[j].ex, "%08x %s: %s", w, flat, err); }
        else if (back != w) {                     /* the same instruction, canonically encoded (don't-care bits)? */
            char again[256];
            a64_disasm(back, addr, again, sizeof again);
            if (!strcmp(again, text)) { equiv++; continue; }
            diff++; st[j].diff++; snprintf(st[j].ex, sizeof st[j].ex, "%08x %s -> %08x", w, flat, back);
        }
        else { same++; st[j].same++; }
    }
    printf("round trip: %ld of %ld the same, %ld the same instruction encoded canonically; %ld different, %ld not assembled\n", same, total, equiv, diff, none);
    for (int a = 0; a < ns; a++) for (int c = a + 1; c < ns; c++)
        if (st[c].diff * 1000 + st[c].none > st[a].diff * 1000 + st[a].none) { __typeof__(st[0]) t = st[a]; st[a] = st[c]; st[c] = t; }
    for (int a = 0; a < ns && a < (getenv("TOP") ? atoi(getenv("TOP")) : 40); a++)
        if (st[a].diff || st[a].none) printf("%-10s same %ld diff %ld none %ld   %s\n", st[a].mn, st[a].same, st[a].diff, st[a].none, st[a].ex);
    return diff != 0;
}
