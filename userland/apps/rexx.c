/* userland/apps/rexx.c - REXX, as OS/2 has it: a classic REXX interpreter.
 *
 *   rexx file.cmd [arguments]     rexx -e 'say 1+1'
 *
 * Clauses (; or the line's end; a trailing comma continues the line),
 * comments (nested), strings ('..', "..", hex 'x', binary 'b'). The
 * instructions: assignments, SAY, IF/THEN/ELSE, DO (count, a control
 * variable TO BY FOR, FOREVER, WHILE, UNTIL), SELECT/WHEN/OTHERWISE,
 * ITERATE, LEAVE, CALL and RETURN (internal routines, functions, labels;
 * PROCEDURE EXPOSE), EXIT, SIGNAL (a label, in a routine too), PARSE [UPPER]
 * ARG/PULL/VAR/VALUE...WITH/SOURCE/VERSION/LINEIN with templates (words,
 * '.', literal patterns, columns, +/- offsets, (variable) patterns), PULL,
 * ARG, DROP, NOP, INTERPRET, NUMERIC DIGITS, ADDRESS, TRACE (ignored);
 * stems (A.I.J, tails substituted), expressions with REXX's arithmetic
 * (decimal strings, NUMERIC DIGITS significant digits), concatenation
 * (abuttal, blank, ||), comparisons (normal and strict), logic. About 60
 * built-in functions, LINEIN/LINEOUT/LINES/CHARIN/CHAROUT/STREAM for files,
 * RexxUtil's common functions (OS/2 paths through /dev/os2cmd). Conditions:
 * SIGNAL ON ERROR/FAILURE/HALT/NOVALUE/SYNTAX/NOTREADY [NAME label] and CALL
 * ON ERROR/FAILURE/HALT (DELAY while the handler runs); RC, SIGL,
 * CONDITION(), ERRORTEXT(), SOURCELINE(). Any other clause is a command: its value goes to the shell (OS/2's,
 * /h/OS2/os2cmd, on the system; REXX_SHELL to choose), RC its result. */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>

/* ── memory: an arena for each clause's temporaries ── */
typedef struct chunk { struct chunk *prev; size_t used, cap; char d[]; } chunk_t;
static chunk_t *arena;
typedef struct { chunk_t *c; size_t used; } mark_t;
static void *amalloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!arena || arena->used + n > arena->cap) {
        size_t cap = n > 65536 ? n : 65536;
        chunk_t *c = malloc(sizeof *c + cap);
        if (!c) { fputs("REX0005: out of memory\n", stderr); exit(5); }
        c->prev = arena; c->used = 0; c->cap = cap;
        arena = c;
    }
    void *p = arena->d + arena->used;
    arena->used += n;
    return p;
}
static mark_t amark(void) { return (mark_t){ arena, arena ? arena->used : 0 }; }
static void arelease(mark_t m)
{
    while (arena && arena != m.c) { chunk_t *p = arena->prev; free(arena); arena = p; }
    if (arena) arena->used = m.used;
}
static char *astr(const char *s, size_t n) { char *r = amalloc(n + 1); memcpy(r, s, n); r[n] = 0; return r; }
static char *adup(const char *s) { return astr(s, strlen(s)); }
static char *afmt(const char *f, ...)
{
    va_list a; char b[512];
    va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a);
    return adup(b);
}

/* ── errors ── */
static const char *src_name = "";
static int cur_line;
static void syntax_trap(int n, const char *msg);                 /* (SIGNAL ON SYNTAX: doesn't return when on) */
static void novalue(const char *name);                           /* (SIGNAL ON NOVALUE: likewise) */
static void rx_error(int n, const char *fmt, ...)
{
    va_list a; char b[512];
    va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    syntax_trap(n, b);
    fprintf(stderr, "REX%04d: Error %d running %s, line %d: %s\n", n, n, src_name, cur_line, b);
    fflush(stdout);
    exit(n > 255 ? 255 : n);
}

/* ── tokens ── */
enum { T_STR, T_SYM, T_OP, T_END };
typedef struct { int type; char *s; size_t n; int blank; int line; } tok_t;   /* (blank: whitespace before it) */
typedef struct { tok_t *t; int n; int line; } clause_t;
static clause_t *cls;
static int ncls, cap_cls;
static void add_clause(tok_t *t, int n, int line)
{
    if (!n) return;
    if (ncls == cap_cls) { cap_cls = cap_cls ? cap_cls * 2 : 256; cls = realloc(cls, (size_t)cap_cls * sizeof *cls); }
    cls[ncls].t = malloc((size_t)n * sizeof *t);
    memcpy(cls[ncls].t, t, (size_t)n * sizeof *t);
    cls[ncls].n = n; cls[ncls].line = line;
    ncls++;
}
static int symch(int c) { return isalnum(c) || c == '.' || c == '_' || c == '!' || c == '?' || c == '@' || c == '#' || c == '$'; }
static int tok_is(const tok_t *t, const char *kw)    /* a symbol, that keyword (any case) */
{
    return t->type == T_SYM && strlen(kw) == t->n && !strncasecmp(t->s, kw, t->n);
}
static int op_is(const tok_t *t, const char *op) { return t->type == T_OP && strlen(op) == t->n && !strncmp(t->s, op, t->n); }
static char *hexstr(const char *s, size_t n, int bin, size_t *outn)    /* 'ab'x, '1010'b */
{
    char *o = malloc(n + 1); size_t k = 0;
    int acc = 0, bits = 0, per = bin ? 1 : 4;
    for (size_t i = n; i-- > 0;) {                               /* (from the right: groups) */
        int c = s[i], v;
        if (c == ' ') continue;
        if (bin) v = c - '0'; else v = isdigit(c) ? c - '0' : toupper(c) - 'A' + 10;
        acc |= v << bits; bits += per;
        if (bits == 8) { o[k++] = (char)acc; acc = 0; bits = 0; }
    }
    if (bits) o[k++] = (char)acc;
    for (size_t i = 0; i < k / 2; i++) { char t = o[i]; o[i] = o[k - 1 - i]; o[k - 1 - i] = t; }
    *outn = k;
    return o;
}
/* the source into clauses: then THEN, ELSE, OTHERWISE and labels made clauses of their own */
static void tokenize(const char *p)
{
    tok_t buf[4096]; int n = 0, line = 1, cline = 1, blank = 0;
    while (*p) {
        cur_line = line;
        if (*p == '/' && p[1] == '*') {                          /* comments, nested */
            int depth = 1; p += 2;
            while (*p && depth) {
                if (*p == '/' && p[1] == '*') { depth++; p += 2; }
                else if (*p == '*' && p[1] == '/') { depth--; p += 2; }
                else { if (*p == '\n') line++; p++; }
            }
            blank = 1; continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; blank = 1; continue; }
        if (*p == '\n' || *p == ';') {
            if (*p == '\n') {
                line++;
                if (n && buf[n - 1].type == T_OP && op_is(&buf[n - 1], ",")) {   /* (a continuation) */
                    int depth = 0;
                    for (int i = 0; i < n - 1; i++) if (op_is(&buf[i], "(")) depth++; else if (op_is(&buf[i], ")")) depth--;
                    n--; p++; blank = 1;
                    (void)depth;
                    continue;
                }
            }
            add_clause(buf, n, cline); n = 0; p++; blank = 0; cline = line;
            continue;
        }
        if (n >= 4096) rx_error(13, "a clause too long");
        if (n == 0) cline = line;
        tok_t t = { 0, 0, 0, blank, line };
        blank = 0;
        if (*p == '\'' || *p == '"') {
            char q = *p++; size_t cap = 64, k = 0; char *s = malloc(cap);
            for (;;) {
                if (!*p || *p == '\n') rx_error(6, "an unmatched quote");
                if (*p == q) { if (p[1] == q) { p += 2; s[k++] = q; } else { p++; break; } }
                else s[k++] = *p++;
                if (k + 2 >= cap) s = realloc(s, cap *= 2);
            }
            s[k] = 0;
            if ((*p == 'x' || *p == 'X' || *p == 'b' || *p == 'B') && !symch((unsigned char)p[1])) {   /* a hex or binary string */
                int bin = *p == 'b' || *p == 'B';
                for (size_t i = 0; i < k; i++)
                    if (!(s[i] == ' ' || (bin ? s[i] == '0' || s[i] == '1' : isxdigit((unsigned char)s[i]))))
                        rx_error(15, "an invalid %s constant: '%s'%c", bin ? "binary" : "hexadecimal", s, *p);
                size_t m; char *h = hexstr(s, k, bin, &m); free(s); s = h; k = m; p++;
            }
            t.type = T_STR; t.s = s; t.n = k;
        } else if (symch((unsigned char)*p)) {
            const char *b = p;
            while (symch((unsigned char)*p)) {
                if ((*p == 'e' || *p == 'E') && (p[1] == '+' || p[1] == '-') && p > b && isdigit((unsigned char)b[0]) && isdigit((unsigned char)p[2])) p += 2;
                else p++;
            }
            t.type = T_SYM; t.s = strndup(b, (size_t)(p - b)); t.n = (size_t)(p - b);
        } else {
            static const char *const ops[] = { "\\==", "\\=", "<>", "><", ">=", "<=", "==", ">>=", "<<=", ">>", "<<", "**", "//", "||", "&&",
                                               "\\<", "\\>", "=", "<", ">", "+", "-", "*", "/", "%", "|", "&", "\\", "(", ")", ",", ":", 0 };
            int k;
            for (k = 0; ops[k]; k++) if (!strncmp(p, ops[k], strlen(ops[k]))) break;
            if (!ops[k]) { if ((unsigned char)*p == 0xAC) { t.s = strdup("\\"); t.n = 1; t.type = T_OP; p++; buf[n++] = t; continue; }
                           rx_error(13, "an invalid character '%c'", *p); }
            t.type = T_OP; t.s = strdup(ops[k]); t.n = strlen(ops[k]); p += t.n;
        }
        buf[n++] = t;
    }
    add_clause(buf, n, cline);
    /* splits: labels; THEN/ELSE/OTHERWISE their own clauses */
    for (int i = 0; i < ncls; i++) {
        clause_t *c = &cls[i];
        int split = -1, keep = 0;
        if (c->n >= 2 && c->t[0].type == T_SYM && op_is(&c->t[1], ":")) split = 2, keep = 2;
        else if (tok_is(&c->t[0], "IF") || tok_is(&c->t[0], "WHEN")) {
            int depth = 0;
            for (int k = 1; k < c->n; k++) {
                if (op_is(&c->t[k], "(")) depth++; else if (op_is(&c->t[k], ")")) depth--;
                else if (!depth && tok_is(&c->t[k], "THEN")) { split = k; keep = k; break; }
            }
        } else if ((tok_is(&c->t[0], "THEN") || tok_is(&c->t[0], "ELSE") || tok_is(&c->t[0], "OTHERWISE")) && c->n > 1) split = 1, keep = 1;
        if (split < 0 || split >= c->n) continue;
        if (ncls == cap_cls) { cap_cls *= 2; cls = realloc(cls, (size_t)cap_cls * sizeof *cls); c = &cls[i]; }
        memmove(&cls[i + 2], &cls[i + 1], (size_t)(ncls - i - 1) * sizeof *cls);
        ncls++;
        cls[i + 1].t = c->t + split; cls[i + 1].n = c->n - split; cls[i + 1].line = c->t[split].line;
        cls[i].n = keep;
    }
}

/* ── variables: scopes; stems; exposure ── */
typedef struct var { char *name, *val; struct var *next; } var_t;
typedef struct scope { var_t *tab[256]; struct scope *parent; char **expose; int nexpose; } scope_t;
static scope_t *scope;
static unsigned hashs(const char *s) { unsigned h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h & 255; }
static scope_t *scope_for(const char *name)                     /* (an exposed name, or its stem: the caller's) */
{
    scope_t *s = scope;
    for (;;) {
        if (!s->parent) return s;
        int ex = 0;
        for (int i = 0; i < s->nexpose && !ex; i++) {
            const char *e = s->expose[i];
            size_t l = strlen(e);
            if (!strcmp(e, name) || (e[l - 1] == '.' && !strncmp(e, name, l))) ex = 1;
        }
        if (!ex) return s;
        s = s->parent;
    }
}
static var_t *var_find(scope_t *s, const char *name)
{
    for (var_t *v = s->tab[hashs(name)]; v; v = v->next) if (!strcmp(v->name, name)) return v;
    return 0;
}
static const char *var_get(const char *name)                    /* (0: not set) */
{
    scope_t *s = scope_for(name);
    var_t *v = var_find(s, name);
    if (v) return v->val;
    const char *dot = strchr(name, '.');
    if (dot && dot[1]) {                                         /* (a stem's default: STEM.) */
        char stem[256];
        snprintf(stem, sizeof stem, "%.*s", (int)(dot - name + 1), name);
        v = var_find(scope_for(stem), stem);
        if (v) return v->val;
    }
    return 0;
}
static void var_set(const char *name, const char *val)
{
    scope_t *s = scope_for(name);
    size_t l = strlen(name);
    if (name[l - 1] == '.') {                                    /* (the stem itself: every compound of it goes) */
        for (int h = 0; h < 256; h++)
            for (var_t **pp = &s->tab[h]; *pp;) {
                if (!strncmp((*pp)->name, name, l) && (*pp)->name[l]) { var_t *d = *pp; *pp = d->next; free(d->name); free(d->val); free(d); }
                else pp = &(*pp)->next;
            }
    }
    var_t *v = var_find(s, name);
    if (!v) { v = calloc(1, sizeof *v); v->name = strdup(name); unsigned h = hashs(name); v->next = s->tab[h]; s->tab[h] = v; }
    free(v->val);
    v->val = strdup(val);
}
static void var_drop(const char *name)
{
    scope_t *s = scope_for(name);
    size_t l = strlen(name);
    for (int h = 0; h < 256; h++)
        for (var_t **pp = &s->tab[h]; *pp;) {
            if (!strcmp((*pp)->name, name) || (name[l - 1] == '.' && !strncmp((*pp)->name, name, l))) { var_t *d = *pp; *pp = d->next; free(d->name); free(d->val); free(d); }
            else pp = &(*pp)->next;
        }
}
static void scope_free(scope_t *s)
{
    for (int h = 0; h < 256; h++) for (var_t *v = s->tab[h]; v;) { var_t *n = v->next; free(v->name); free(v->val); free(v); v = n; }
    for (int i = 0; i < s->nexpose; i++) free(s->expose[i]);
    free(s->expose); free(s);
}
/* a symbol's name: upper case; a compound's tail parts replaced by their variables' values */
static char *var_name(const tok_t *t)
{
    char *u = astr(t->s, t->n);
    for (char *c = u; *c; c++) *c = (char)toupper((unsigned char)*c);
    char *dot = strchr(u, '.');
    if (!dot || !dot[1]) return u;
    size_t cap = 256, k = (size_t)(dot - u + 1);
    char *o = amalloc(cap);
    memcpy(o, u, k);
    for (char *p = dot + 1; ; ) {
        char *e = strchr(p, '.');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char part[256];
        snprintf(part, sizeof part, "%.*s", (int)l, p);
        const char *v = part[0] && !isdigit((unsigned char)part[0]) ? var_get(part) : 0;
        const char *r = v ? v : part;
        size_t rl = strlen(r);
        if (k + rl + 2 > cap) { char *n2 = amalloc(cap = (k + rl + 2) * 2); memcpy(n2, o, k); o = n2; }
        memcpy(o + k, r, rl); k += rl;
        if (!e) break;
        o[k++] = '.';
        p = e + 1;
    }
    o[k] = 0;
    return o;
}

/* ── numbers ── */
static int digits = 9;
static int is_num(const char *s, double *out)
{
    while (*s == ' ') s++;
    if (!*s) return 0;
    char *e;
    const char *b = s;
    if (*s == '+' || *s == '-') s++;
    if (!isdigit((unsigned char)*s) && !(*s == '.' && isdigit((unsigned char)s[1]))) return 0;
    double v = strtod(b, &e);
    while (*e == ' ') e++;
    if (*e) return 0;
    if (out) *out = v;
    return 1;
}
static double num(const char *s)
{
    double v;
    if (!is_num(s, &v)) rx_error(41, "bad arithmetic conversion: \"%s\"", s);
    return v;
}
static char *fmtnum(double v)
{
    if (v == 0) return adup("0");
    if (fabs(v) < 1e15 && v == trunc(v)) return afmt("%.0f", v);
    char b[64];
    snprintf(b, sizeof b, "%.*g", digits, v);
    if (strchr(b, 'e')) {                                        /* (REXX: E+n, no leading zeros) */
        char m[64]; int ex = 0;
        sscanf(b, "%[^e]e%d", m, &ex);
        return afmt("%sE%+d", m, ex);
    }
    return adup(b);
}
static int truthy(const char *s)
{
    if (!strcmp(s, "1")) return 1;
    if (!strcmp(s, "0")) return 0;
    rx_error(34, "a logical value isn't 0 or 1: \"%s\"", s);
    return 0;
}

/* ── expressions ── */
typedef struct { const tok_t *t; int i, n; } ex_t;
static char *call_function(const char *name, char **args, int nargs, int internal_first, int line);
static char *expr_or(ex_t *e);
static const tok_t *ex_peek(ex_t *e) { return e->i < e->n ? &e->t[e->i] : 0; }
static int at_term_start(const tok_t *t) { return t && (t->type != T_OP || op_is(t, "(") || op_is(t, "\\") || op_is(t, "-") || op_is(t, "+")); }
static char *expr_primary(ex_t *e)
{
    const tok_t *t = ex_peek(e);
    if (!t) rx_error(35, "an expression ends too soon");
    if (op_is(t, "(")) {
        e->i++;
        char *v = expr_or(e);
        if (!ex_peek(e) || !op_is(ex_peek(e), ")")) rx_error(36, "an unmatched (");
        e->i++;
        return v;
    }
    if (t->type == T_STR || t->type == T_SYM) {
        e->i++;
        const tok_t *nx = ex_peek(e);
        if (nx && op_is(nx, "(") && !nx->blank) {               /* a function call */
            e->i++;
            char *args[32]; int na = 0;
            if (ex_peek(e) && op_is(ex_peek(e), ")")) e->i++;
            else for (;;) {
                if (ex_peek(e) && (op_is(ex_peek(e), ",") || op_is(ex_peek(e), ")"))) args[na++] = 0;   /* (omitted) */
                else args[na++] = expr_or(e);
                if (!ex_peek(e)) rx_error(36, "an unmatched (");
                if (op_is(ex_peek(e), ")")) { e->i++; break; }
                if (!op_is(ex_peek(e), ",")) rx_error(37, "an unexpected \"%.*s\"", (int)ex_peek(e)->n, ex_peek(e)->s);
                e->i++;
                if (na == 32) rx_error(40, "too many arguments");
            }
            char *nm = t->type == T_STR ? astr(t->s, t->n) : astr(t->s, t->n);
            if (t->type == T_SYM) for (char *c = nm; *c; c++) *c = (char)toupper((unsigned char)*c);
            return call_function(nm, args, na, t->type == T_SYM, t->line);
        }
        if (t->type == T_STR) return astr(t->s, t->n);
        if (isdigit((unsigned char)t->s[0]) || (t->s[0] == '.' && t->n > 1 && isdigit((unsigned char)t->s[1]))) return astr(t->s, t->n);   /* a constant */
        char *nm = var_name(t);
        const char *v = var_get(nm);
        if (!v) novalue(nm);
        return v ? adup(v) : nm;                                 /* (unset: its own name) */
    }
    rx_error(35, "an unexpected \"%.*s\"", (int)t->n, t->s);
    return 0;
}
static char *expr_prefix(ex_t *e)
{
    const tok_t *t = ex_peek(e);
    if (t && (op_is(t, "\\") || op_is(t, "-") || op_is(t, "+"))) {
        e->i++;
        char *v = expr_prefix(e);
        if (op_is(t, "\\")) return adup(truthy(v) ? "0" : "1");
        double d = num(v);
        return fmtnum(op_is(t, "-") ? -d : d);
    }
    return expr_primary(e);
}
static char *expr_power(ex_t *e)
{
    char *a = expr_prefix(e);
    while (ex_peek(e) && op_is(ex_peek(e), "**")) {
        e->i++;
        char *b = expr_prefix(e);
        a = fmtnum(pow(num(a), num(b)));
    }
    return a;
}
static char *expr_mul(ex_t *e)
{
    char *a = expr_power(e);
    for (const tok_t *t; (t = ex_peek(e)) && (op_is(t, "*") || op_is(t, "/") || op_is(t, "%") || op_is(t, "//"));) {
        e->i++;
        char *b = expr_power(e);
        double x = num(a), y = num(b);
        if (!op_is(t, "*") && y == 0) rx_error(42, "division by zero");
        a = fmtnum(op_is(t, "*") ? x * y : op_is(t, "/") ? x / y : op_is(t, "%") ? trunc(x / y) : x - y * trunc(x / y));
    }
    return a;
}
static char *expr_add(ex_t *e)
{
    char *a = expr_mul(e);
    for (const tok_t *t; (t = ex_peek(e)) && (op_is(t, "+") || op_is(t, "-"));) {
        e->i++;
        char *b = expr_mul(e);
        a = fmtnum(op_is(t, "+") ? num(a) + num(b) : num(a) - num(b));
    }
    return a;
}
static char *concat(const char *a, const char *b, int blank)
{
    size_t la = strlen(a), lb = strlen(b);
    char *r = amalloc(la + lb + 2);
    memcpy(r, a, la);
    if (blank) r[la++] = ' ';
    memcpy(r + la, b, lb + 1);
    return r;
}
static char *expr_cat(ex_t *e)
{
    char *a = expr_add(e);
    for (;;) {
        const tok_t *t = ex_peek(e);
        if (t && op_is(t, "||")) { e->i++; a = concat(a, expr_add(e), 0); continue; }
        if (at_term_start(t) && !(t->type == T_OP && (op_is(t, "-") || op_is(t, "+") ) && t->blank)) {   /* abuttal or blank concatenation */
            if (t->type == T_OP && (op_is(t, "-") || op_is(t, "+"))) break;
            int blank = t->blank;
            a = concat(a, expr_add(e), blank);
            continue;
        }
        break;
    }
    return a;
}
static int compare(const char *a, const char *b)               /* (normal: numbers as numbers, else blanks ignored at the ends) */
{
    double x, y;
    if (is_num(a, &x) && is_num(b, &y)) return x < y ? -1 : x > y;
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == ' ') la--;
    while (lb && b[lb - 1] == ' ') lb--;
    size_t m = la > lb ? la : lb;
    for (size_t i = 0; i < m; i++) {
        int ca = i < la ? (unsigned char)a[i] : ' ', cb = i < lb ? (unsigned char)b[i] : ' ';
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return 0;
}
static char *expr_cmp(ex_t *e)
{
    char *a = expr_cat(e);
    for (const tok_t *t; (t = ex_peek(e)) && t->type == T_OP;) {
        static const char *const ops[] = { "=", "\\=", "<>", "><", ">", "<", ">=", "<=", "\\<", "\\>", "==", "\\==", ">>", "<<", ">>=", "<<=", 0 };
        int k;
        for (k = 0; ops[k]; k++) if (op_is(t, ops[k])) break;
        if (!ops[k]) break;
        e->i++;
        char *b = expr_cat(e);
        int r, strict = k >= 10;
        if (strict) { int c = strcmp(a, b); r = c < 0 ? -1 : c > 0; } else r = compare(a, b);
        int v = 0;
        switch (k) {
        case 0: case 10: v = r == 0; break;
        case 1: case 2: case 3: case 11: v = r != 0; break;
        case 4: case 12: v = r > 0; break;
        case 5: case 13: v = r < 0; break;
        case 6: case 14: case 8: v = r >= 0; break;
        case 7: case 15: case 9: v = r <= 0; break;
        }
        a = adup(v ? "1" : "0");
    }
    return a;
}
static char *expr_and(ex_t *e)
{
    char *a = expr_cmp(e);
    while (ex_peek(e) && op_is(ex_peek(e), "&")) { e->i++; char *b = expr_cmp(e); a = adup(truthy(a) && truthy(b) ? "1" : "0"); }
    return a;
}
static char *expr_or(ex_t *e)
{
    char *a = expr_and(e);
    for (const tok_t *t; (t = ex_peek(e)) && (op_is(t, "|") || op_is(t, "&&"));) {
        e->i++;
        char *b = expr_and(e);
        a = adup(op_is(t, "|") ? (truthy(a) || truthy(b) ? "1" : "0") : (truthy(a) != truthy(b) ? "1" : "0"));
    }
    return a;
}
/* an expression of tokens [i, n) */
static char *eval_toks(const tok_t *t, int i, int n)
{
    if (i >= n) return adup("");
    ex_t e = { t, i, n };
    char *v = expr_or(&e);
    if (e.i < n) rx_error(35, "an unexpected \"%.*s\"", (int)t[e.i].n, t[e.i].s);
    return v;
}

/* ── execution ── */
enum { R_NEXT, R_LEAVE, R_ITERATE, R_RETURN, R_EXIT, R_SIGNAL };
typedef struct frame { char **args; int nargs; char *result; int has_result; int internal; struct scope *own; } frame_t;
static frame_t *frame;
static char *ctl_name;                                           /* (ITERATE/LEAVE name; SIGNAL's label) */
static char *exit_val;
static char *source_line(void) { return afmt("OS/2 COMMAND %s", src_name); }

/* ── conditions: SIGNAL ON / CALL ON ── */
enum { C_ERROR, C_FAILURE, C_HALT, C_NOVALUE, C_SYNTAX, C_NOTREADY, C_N };
static const char *const cond_names[C_N] = { "ERROR", "FAILURE", "HALT", "NOVALUE", "SYNTAX", "NOTREADY" };
typedef struct { int on, call, delayed; char label[64]; } trap_t;
static trap_t traps[C_N];
static struct { int cond, call; char *desc; } cinfo = { -1, 0, 0 };   /* (CONDITION()'s: the last one trapped) */
/* an activation (the program, a routine called): where SIGNAL goes on from, and where a trap from deep inside an
 * expression (SYNTAX, NOVALUE) comes back to */
typedef struct act { jmp_buf jb; struct act *prev; frame_t *frame; struct scope *scope; int ncls; } act_t;
static act_t *acts;
static char *pend_label, *pend_rc;
static int pend_line, sig_line;
static volatile sig_atomic_t halted;
static void on_sigint(int sig) { (void)sig; halted = 1; }
static void cond_info(int c, int call, const char *desc)
{
    cinfo.cond = c; cinfo.call = call;
    free(cinfo.desc); cinfo.desc = strdup(desc ? desc : "");
}
static void cond_jump(int c, const char *desc, const char *rc)   /* (SIGNAL to its label, from anywhere) */
{
    traps[c].on = 0;
    cond_info(c, 0, desc);
    free(pend_label); pend_label = strdup(traps[c].label);
    free(pend_rc); pend_rc = rc ? strdup(rc) : 0;
    pend_line = cur_line;
    longjmp(acts->jb, 1);
}
static void syntax_trap(int n, const char *msg)
{
    if (!acts || !traps[C_SYNTAX].on) return;
    char rc[16]; snprintf(rc, sizeof rc, "%d", n);
    cond_jump(C_SYNTAX, msg, rc);
}
static void novalue(const char *name) { if (acts && traps[C_NOVALUE].on) cond_jump(C_NOVALUE, name, 0); }
static int find_label(const char *name);
static char *call_routine(int label, char **args, int nargs, int as_function);
static int raise_cond(int c, const char *desc)                   /* (between clauses: ERROR, FAILURE, HALT, NOTREADY) */
{
    if (c == C_FAILURE && !traps[c].on) c = C_ERROR;              /* (FAILURE not trapped: an ERROR) */
    trap_t *tr = &traps[c];
    if (!tr->on || tr->delayed) return R_NEXT;
    cond_info(c, tr->call, desc);
    if (!tr->call) {
        tr->on = 0;
        free(ctl_name); ctl_name = strdup(tr->label);
        sig_line = cur_line;
        return R_SIGNAL;
    }
    int lab = find_label(tr->label), line = cur_line;
    if (lab < 0) rx_error(16, "label %s not found", tr->label);
    var_set("SIGL", fmtnum(line));
    tr->delayed = 1;
    call_routine(lab, 0, 0, 0);
    tr->delayed = 0;
    cur_line = line;
    return R_NEXT;
}
static void trap_set(const tok_t *t, int n, int call)           /* SIGNAL|CALL ON|OFF condition [NAME label] */
{
    int c = -1;
    if (n >= 3) for (int k = 0; k < C_N; k++) if (tok_is(&t[2], cond_names[k])) c = k;
    if (c < 0) rx_error(25, "%s %s: ERROR, FAILURE, HALT, NOVALUE, SYNTAX or NOTREADY", call ? "CALL" : "SIGNAL", tok_is(&t[1], "ON") ? "ON" : "OFF");
    if (call && (c == C_NOVALUE || c == C_SYNTAX)) rx_error(25, "CALL ON %s isn't allowed", cond_names[c]);
    trap_t *tr = &traps[c];
    if (tok_is(&t[1], "OFF")) { tr->on = 0; return; }
    tr->on = 1; tr->call = call; tr->delayed = 0;
    if (n >= 5 && tok_is(&t[3], "NAME")) snprintf(tr->label, sizeof tr->label, "%.*s", (int)t[4].n, t[4].s);
    else snprintf(tr->label, sizeof tr->label, "%s", cond_names[c]);
}
static int find_label(const char *name)
{
    for (int i = 0; i < ncls; i++)
        if (cls[i].n == 2 && op_is(&cls[i].t[1], ":") && cls[i].t[0].type == T_SYM && strlen(name) == cls[i].t[0].n && !strncasecmp(cls[i].t[0].s, name, cls[i].t[0].n)) return i;
    return -1;
}
static int instr_end(int i);
static int match_end(int i)                                      /* (a DO's or SELECT's: the clause after its END) */
{
    int k = i + 1;
    while (k < ncls) {
        if (tok_is(&cls[k].t[0], "END")) return k + 1;
        if (tok_is(&cls[k].t[0], "DO") || tok_is(&cls[k].t[0], "SELECT")) k = match_end(k);
        else k++;
    }
    cur_line = cls[i].line;
    rx_error(14, "a DO or SELECT without its END");
    return k;
}
static int instr_end(int i)                                      /* (the clause after an instruction: DO/SELECT groups, IF with THEN and ELSE) */
{
    if (i >= ncls) return i;
    if (tok_is(&cls[i].t[0], "DO") || tok_is(&cls[i].t[0], "SELECT")) return match_end(i);
    if (tok_is(&cls[i].t[0], "IF")) {
        int k = i + 1;
        if (k < ncls && tok_is(&cls[k].t[0], "THEN") && cls[k].n == 1) k++;
        k = instr_end(k);
        if (k < ncls && tok_is(&cls[k].t[0], "ELSE") && cls[k].n == 1) k = instr_end(k + 1);
        return k;
    }
    return i + 1;
}
static int exec_range(int from, int to);
static int exec_clause(int *ip);
static int run_command(const char *cmd)
{
    fflush(stdout);
    const char *sh = getenv("REXX_SHELL");
    if (!sh) sh = access("/h/OS2/os2cmd", X_OK) == 0 ? "/h/OS2/os2cmd" : "/bin/sh";
    pid_t pid = fork();
    if (!pid) {
        if (strstr(sh, "os2cmd")) execl(sh, sh, "/C", cmd, (char *)0);   /* (the line as it is: os2cmd /C) */
        else execl(sh, sh, "-c", cmd, (char *)0);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) { }
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    var_set("RC", fmtnum(rc));
    return rc;
}
/* PARSE's template against a string */
static void parse_template(const char *src, const tok_t *t, int i, int n, int upper)
{
    char *s = adup(src ? src : "");
    if (upper) for (char *c = s; *c; c++) *c = (char)toupper((unsigned char)*c);
    size_t len = strlen(s), pos = 0;
    while (i < n) {
        /* the targets up to the next pattern; then the pattern gives where this part ends */
        int ts = i;
        while (i < n && t[i].type == T_SYM && !(isdigit((unsigned char)t[i].s[0]))) i++;
        int te = i;
        size_t end = len, next = len;
        if (i < n) {
            const tok_t *p = &t[i];
            if (p->type == T_STR || op_is(p, "(")) {             /* a literal (or variable) pattern */
                char *lit;
                if (p->type == T_STR) { lit = astr(p->s, p->n); i++; }
                else { const char *v = var_get(var_name(&t[i + 1])); lit = adup(v ? v : ""); i += 3; }
                char *f = strstr(s + pos, lit);
                if (f) { end = (size_t)(f - s); next = end + strlen(lit); } else { end = len; next = len; }
            } else if (op_is(p, "+") || op_is(p, "-") || (p->type == T_SYM && isdigit((unsigned char)p->s[0]))) {   /* columns */
                int sign = op_is(p, "+") ? 1 : op_is(p, "-") ? -1 : 0;
                if (sign) i++;
                long c = i < n ? atol(t[i].s) : 0;
                i++;
                long np = sign ? (long)pos + sign * c : c - 1;
                if (np < 0) np = 0;
                if ((size_t)np > len) np = (long)len;
                if ((size_t)np > pos) end = (size_t)np; else end = len;
                next = (size_t)np;
            } else i++;
        }
        /* this part [pos, end) to the targets: words, the last one the rest */
        char *part = astr(s + pos, end > pos ? end - pos : 0);
        char *p = part;
        for (int k = ts; k < te; k++) {
            int last = k == te - 1;
            char *v;
            if (last) {
                if (k > ts) while (*p == ' ') p++;
                v = p;
                if (k > ts || te - ts > 1) { }
            } else {
                while (*p == ' ') p++;
                char *b = p;
                while (*p && *p != ' ') p++;
                v = astr(b, (size_t)(p - b));
                if (*p) p++;
            }
            if (last && te - ts > 1) { }                         /* (the last of several: the rest, its leading blank already gone) */
            if (!(t[k].n == 1 && t[k].s[0] == '.')) var_set(var_name(&t[k]), v);
        }
        if (te == ts && i >= n) break;
        pos = next;
        if (i >= n && te > ts) break;
    }
}
static char *get_line_stdin(void)
{
    static char *b; static size_t cap;
    fflush(stdout);
    ssize_t r = getline(&b, &cap, stdin);
    if (r <= 0) { clearerr(stdin); return adup(""); }
    while (r && (b[r - 1] == '\n' || b[r - 1] == '\r')) b[--r] = 0;
    return adup(b);
}
static int exec_instr(int i, int *next)                          /* one instruction (with its group): its result */
{
    int ip = i;
    int r = exec_clause(&ip);
    *next = ip;
    return r;
}
static int exec_range(int from, int to)
{
    int ip = from;
    while (ip < to) {
        int r = exec_clause(&ip);
        if (r != R_NEXT) return r;
    }
    return R_NEXT;
}
/* the DO header: [name = expr [TO e] [BY e] [FOR e] | FOREVER | count] [WHILE e | UNTIL e] */
static int find_kw(const tok_t *t, int i, int n, const char *const *kws)
{
    int depth = 0;
    for (int k = i; k < n; k++) {
        if (op_is(&t[k], "(")) depth++; else if (op_is(&t[k], ")")) depth--;
        else if (!depth) for (int j = 0; kws[j]; j++) if (tok_is(&t[k], kws[j])) return k;
    }
    return n;
}
static int exec_do(int i, int *next)
{
    clause_t *c = &cls[i];
    int end = match_end(i), body = i + 1, last = end - 1;    /* (last: the END) */
    *next = end;
    const tok_t *t = c->t;
    int n = c->n;
    static const char *const kw_all[] = { "TO", "BY", "FOR", "WHILE", "UNTIL", 0 }, *const kw_cond[] = { "WHILE", "UNTIL", 0 };
    char *cvar = 0;
    double from = 0, to = 0, by = 1, cnt = -1;
    int has_to = 0, forever = 0, kind = 0;                       /* (kind: 0 once, 1 controlled, 2 count, 3 forever) */
    int k = 1, ce = find_kw(t, 1, n, kw_cond);
    if (n == 1) kind = 0;
    else if (n > 2 && t[1].type == T_SYM && op_is(&t[2], "=")) {
        kind = 1;
        cvar = var_name(&t[1]);
        int e1 = find_kw(t, 3, n, kw_all);
        from = num(eval_toks(t, 3, e1));
        k = e1;
        while (k < ce) {
            int e2 = find_kw(t, k + 1, n, kw_all);
            double v = num(eval_toks(t, k + 1, e2));
            if (tok_is(&t[k], "TO")) { to = v; has_to = 1; }
            else if (tok_is(&t[k], "BY")) by = v;
            else if (tok_is(&t[k], "FOR")) cnt = v;
            k = e2;
        }
    } else if (tok_is(&t[1], "FOREVER")) { kind = 3; forever = 1; }
    else if (ce > 1) { kind = 2; cnt = num(eval_toks(t, 1, ce)); }
    int cond = 0;                                                 /* (1 WHILE, 2 UNTIL) */
    int cs = ce;
    if (ce < n) cond = tok_is(&t[ce], "WHILE") ? 1 : 2;
    (void)forever;
    char *ctl_label = cvar;
    if (kind == 0 && !cond) { int r = exec_range(body, last); return r; }
    if (kind == 1) var_set(cvar, fmtnum(from));
    for (double iter = 0;; iter++) {
        mark_t m = amark();
        if (kind == 1) {
            double v = num(var_get(cvar) ? var_get(cvar) : "0");
            if (has_to && (by >= 0 ? v > to : v < to)) { arelease(m); break; }
        }
        if ((kind == 1 || kind == 2) && cnt >= 0 && iter >= cnt) { arelease(m); break; }
        if (cond == 1 && !truthy(eval_toks(t, cs + 1, n))) { arelease(m); break; }
        arelease(m);
        int r = exec_range(body, last);
        if (r == R_LEAVE && (!ctl_name || (ctl_label && !strcmp(ctl_name, ctl_label)))) { ctl_name = 0; break; }
        if (r == R_ITERATE && (!ctl_name || (ctl_label && !strcmp(ctl_name, ctl_label)))) ctl_name = 0;
        else if (r != R_NEXT) return r;
        m = amark();
        if (cond == 2 && truthy(eval_toks(t, cs + 1, n))) { arelease(m); break; }
        arelease(m);
        if (kind == 1) var_set(cvar, fmtnum(num(var_get(cvar) ? var_get(cvar) : "0") + by));
    }
    return R_NEXT;
}
static int run_act(int start)
{
    act_t a;
    a.prev = acts; a.frame = frame; a.scope = scope; a.ncls = ncls;
    acts = &a;
    volatile int ip = start;
    int r;
    if (setjmp(a.jb)) {                                           /* (a trap from inside: this activation's state again) */
        acts = &a; frame = a.frame; ncls = a.ncls;
        scope_t *target = a.frame->own ? a.frame->own : a.scope;
        while (scope != target && scope->parent) { scope_t *sc = scope; scope = sc->parent; scope_free(sc); }
        var_set("SIGL", fmtnum(pend_line));
        if (pend_rc) var_set("RC", pend_rc);
        int lab = find_label(pend_label);
        if (lab < 0) rx_error(16, "label %s not found", pend_label);
        ip = lab + 1;
    }
    for (;;) {
        r = exec_range(ip, ncls);
        if (r != R_SIGNAL) break;
        int lab = find_label(ctl_name);
        if (lab < 0) rx_error(16, "label %s not found", ctl_name);
        var_set("SIGL", fmtnum(sig_line));
        ip = lab + 1;
    }
    acts = a.prev;
    return r;
}
static char *call_routine(int label, char **args, int nargs, int as_function)
{
    frame_t f = { args, nargs, 0, 0, 1, 0 }, *saved = frame;
    scope_t *sc = scope;
    frame = &f;
    int r = run_act(label + 1);
    frame = saved;
    if (scope != sc) { scope_t *mine = scope; scope = sc; scope_free(mine); }   /* (a PROCEDURE's scope gone) */
    if (r == R_EXIT) { fflush(stdout); exit(exit_val && is_num(exit_val, 0) ? (int)num(exit_val) : 0); }
    if (as_function && !f.has_result) rx_error(44, "a function returned nothing");
    if (f.has_result) { char *v = adup(f.result); free(f.result); if (!as_function) var_set("RESULT", v); return v; }
    if (!as_function) var_drop("RESULT");
    return 0;
}
static int exec_clause(int *ip)
{
    int i = *ip;
    clause_t *c = &cls[i];
    const tok_t *t = c->t;
    int n = c->n;
    cur_line = c->line;
    if (halted) {
        halted = 0;
        if (!traps[C_HALT].on) rx_error(4, "program interrupted");
        int r = raise_cond(C_HALT, "");
        if (r != R_NEXT) return r;
    }
    *ip = i + 1;
    mark_t m = amark();
    int res = R_NEXT;
    if (n >= 2 && t[0].type == T_SYM && op_is(&t[1], ":")) goto done;                     /* a label */
    if (n >= 2 && t[0].type == T_SYM && op_is(&t[1], "=") && !(t[0].n == 0)) {           /* an assignment */
        char *nm = var_name(&t[0]);
        var_set(nm, eval_toks(t, 2, n));
        goto done;
    }
    if (t[0].type == T_SYM) {
        char kw[16];
        snprintf(kw, sizeof kw, "%.*s", (int)(t[0].n < 15 ? t[0].n : 15), t[0].s);
        for (char *p = kw; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (!strcmp(kw, "SAY")) { char *v = eval_toks(t, 1, n); fputs(v, stdout); fputc('\n', stdout); goto done; }
        if (!strcmp(kw, "NOP") || !strcmp(kw, "TRACE") || !strcmp(kw, "ADDRESS") || !strcmp(kw, "OPTIONS")) goto done;
        if (!strcmp(kw, "IF")) {
            int cond = truthy(eval_toks(t, 1, n));
            int k = i + 1;
            if (k >= ncls || !tok_is(&cls[k].t[0], "THEN") || cls[k].n != 1) rx_error(18, "IF without THEN");
            k++;
            int after_then = instr_end(k), nx;
            if (cond) { arelease(m); res = exec_instr(k, &nx); if (res != R_NEXT) return res; }
            k = after_then;
            if (k < ncls && tok_is(&cls[k].t[0], "ELSE") && cls[k].n == 1) {
                int after_else = instr_end(k + 1);
                if (!cond) { arelease(m); res = exec_instr(k + 1, &nx); if (res != R_NEXT) return res; }
                k = after_else;
            }
            *ip = k;
            goto done;
        }
        if (!strcmp(kw, "THEN") || !strcmp(kw, "ELSE")) rx_error(8, "an unexpected %s", kw);
        if (!strcmp(kw, "DO")) { arelease(m); return exec_do(i, ip); }
        if (!strcmp(kw, "END")) rx_error(10, "an unexpected END");
        if (!strcmp(kw, "SELECT")) {
            int end = match_end(i), k = i + 1, done_sel = 0;
            *ip = end;
            while (k < end - 1) {
                clause_t *w = &cls[k];
                if (tok_is(&w->t[0], "WHEN")) {
                    int cnd = done_sel ? 0 : truthy(eval_toks(w->t, 1, w->n));
                    int th = k + 1;
                    if (th >= ncls || !tok_is(&cls[th].t[0], "THEN") || cls[th].n != 1) rx_error(18, "WHEN without THEN");
                    int ie = instr_end(th + 1), nx;
                    if (cnd) { done_sel = 1; res = exec_instr(th + 1, &nx); if (res != R_NEXT) { arelease(m); return res; } }
                    k = ie;
                } else if (tok_is(&w->t[0], "OTHERWISE")) {
                    if (!done_sel) { done_sel = 1; res = exec_range(k + 1, end - 1); if (res != R_NEXT) { arelease(m); return res; } }
                    break;
                } else rx_error(7, "WHEN or OTHERWISE expected");
            }
            if (!done_sel) rx_error(7, "no WHEN was true, and no OTHERWISE");
            goto done;
        }
        if (!strcmp(kw, "ITERATE") || !strcmp(kw, "LEAVE")) {
            ctl_name = n > 1 ? strdup(var_name(&t[1])) : 0;
            res = !strcmp(kw, "LEAVE") ? R_LEAVE : R_ITERATE;
            goto done;
        }
        if (!strcmp(kw, "EXIT")) { char *v = eval_toks(t, 1, n); free(exit_val); exit_val = strdup(v); res = R_EXIT; goto done; }
        if (!strcmp(kw, "RETURN")) {
            if (frame && frame->internal) { if (n > 1) { frame->result = strdup(eval_toks(t, 1, n)); frame->has_result = 1; } res = R_RETURN; }
            else { char *v = eval_toks(t, 1, n); free(exit_val); exit_val = strdup(v); res = R_EXIT; }
            goto done;
        }
        if (!strcmp(kw, "CALL")) {
            if (n < 2) rx_error(19, "CALL without a name");
            if (tok_is(&t[1], "ON") || tok_is(&t[1], "OFF")) { trap_set(t, n, 1); goto done; }
            char nm[128];
            snprintf(nm, sizeof nm, "%.*s", (int)t[1].n, t[1].s);
            if (t[1].type == T_SYM) for (char *p = nm; *p; p++) *p = (char)toupper((unsigned char)*p);
            char *args[32]; int na = 0, k = 2;
            while (k < n && na < 32) {
                int depth = 0, e = k;
                while (e < n && !(depth == 0 && op_is(&t[e], ","))) { if (op_is(&t[e], "(")) depth++; else if (op_is(&t[e], ")")) depth--; e++; }
                args[na++] = e > k ? eval_toks(t, k, e) : 0;
                k = e + 1;
            }
            char *v = call_function(nm, args, na, t[1].type == T_SYM, t[1].line);
            (void)v;
            goto done;
        }
        if (!strcmp(kw, "PROCEDURE")) {
            if (!frame || !frame->internal) rx_error(17, "PROCEDURE isn't the first instruction of a routine");
            scope_t *s = calloc(1, sizeof *s);
            s->parent = scope;
            if (n > 1 && tok_is(&t[1], "EXPOSE"))
                for (int k = 2; k < n; k++) {
                    s->expose = realloc(s->expose, (size_t)(s->nexpose + 1) * sizeof *s->expose);
                    char *nm = astr(t[k].s, t[k].n);
                    for (char *p = nm; *p; p++) *p = (char)toupper((unsigned char)*p);
                    s->expose[s->nexpose++] = strdup(nm);
                }
            scope = s;
            frame->own = s;
            goto done;
        }
        if (!strcmp(kw, "DROP")) { for (int k = 1; k < n; k++) var_drop(var_name(&t[k])); goto done; }
        if (!strcmp(kw, "SIGNAL")) {
            if (n > 1 && (tok_is(&t[1], "ON") || tok_is(&t[1], "OFF"))) { trap_set(t, n, 0); goto done; }
            sig_line = cur_line;
            free(ctl_name);
            ctl_name = n > 1 && tok_is(&t[1], "VALUE") ? strdup(eval_toks(t, 2, n)) : n > 1 ? strndup(t[1].s, t[1].n) : strdup("");
            res = R_SIGNAL;
            goto done;
        }
        if (!strcmp(kw, "NUMERIC")) {
            if (n > 1 && tok_is(&t[1], "DIGITS")) { digits = n > 2 ? (int)num(eval_toks(t, 2, n)) : 9; if (digits < 1) digits = 1; if (digits > 17) digits = 17; }
            goto done;
        }
        if (!strcmp(kw, "INTERPRET")) {
            char *code = eval_toks(t, 1, n);
            int base = ncls;
            tokenize(code);
            int r = exec_range(base, ncls);
            ncls = base;                                         /* (its clauses gone again) */
            arelease(m);
            return r;
        }
        if (!strcmp(kw, "PULL") || !strcmp(kw, "ARG") || !strcmp(kw, "PARSE")) {
            int k = 1, upper = 0;
            const char *how = kw;
            if (!strcmp(kw, "PARSE")) {
                if (k < n && tok_is(&t[k], "UPPER")) { upper = 1; k++; }
                if (k >= n) rx_error(25, "PARSE what?");
                how = astr(t[k].s, t[k].n);
                k++;
            } else upper = 1;
            if (!strcasecmp(how, "ARG")) {
                int a = 0, s = k;
                for (int e = k; ; e++) {                             /* (a template for each argument, commas between) */
                    if (e == n || op_is(&t[e], ",")) {
                        const char *v = frame && a < frame->nargs && frame->args[a] ? frame->args[a] : "";
                        parse_template(v, t, s, e, upper);
                        a++; s = e + 1;
                        if (e == n) break;
                    }
                }
            } else if (!strcasecmp(how, "PULL")) parse_template(get_line_stdin(), t, k, n, upper);
            else if (!strcasecmp(how, "LINEIN")) parse_template(get_line_stdin(), t, k, n, upper);
            else if (!strcasecmp(how, "VAR")) { const char *v = var_get(var_name(&t[k])); parse_template(v ? v : "", t, k + 1, n, upper); }
            else if (!strcasecmp(how, "VALUE")) {
                int w = find_kw(t, k, n, (const char *const[]){ "WITH", 0 });
                parse_template(eval_toks(t, k, w), t, w + 1, n, upper);
            } else if (!strcasecmp(how, "SOURCE")) parse_template(source_line(), t, k, n, upper);
            else if (!strcasecmp(how, "VERSION")) parse_template("REXX-os2warp 4.00 15 Nov 2026", t, k, n, upper);
            else rx_error(25, "PARSE %s isn't known", how);
            goto done;
        }
    }
    /* anything else: an expression, a command for the shell */
    {
        char *cmd = eval_toks(t, 0, n);
        int rc = run_command(cmd);
        if (rc) res = raise_cond(rc < 0 || rc == 127 ? C_FAILURE : C_ERROR, cmd);   /* (127: not run at all) */
    }
done:
    arelease(m);
    return res;
}

/* ── built-in functions ── */
static char *argv1;                                              /* (the program's argument string) */
static char **src_lines;                                         /* (the program's lines, for SOURCELINE) */
static int nsrc;
static long arg_num(char **a, int na, int i, long def) { return i < na && a[i] ? (long)num(a[i]) : def; }
static const char *arg_str(char **a, int na, int i, const char *def) { return i < na && a[i] ? a[i] : def; }
static int word_bounds(const char *s, long w, size_t *b, size_t *e)   /* the w-th word's [b, e) */
{
    size_t i = 0, l = strlen(s);
    for (long k = 1;; k++) {
        while (i < l && s[i] == ' ') i++;
        if (i >= l) return 0;
        size_t st = i;
        while (i < l && s[i] != ' ') i++;
        if (k == w) { *b = st; *e = i; return 1; }
    }
}
static long count_words(const char *s) { long k = 0; size_t b, e; while (word_bounds(s, k + 1, &b, &e)) k++; return k; }
/* OS/2 paths ("D:\\X", "\\X") to this system's, through the kernel (/dev/os2cmd's resolve ioctl, as os2cmd does); and back,
 * by the drives' roots. Without the device (another system): backslashes become slashes, names stay as they are */
#define OS2CMD_RESOLVE 0x4F53
static int os2dev = -2;
static char droot[26][256];
static int os2_resolve(const char *in, char *out, size_t n)
{
    if (os2dev == -2) os2dev = open("/dev/os2cmd", O_RDWR);
    if (os2dev < 0) return -1;
    char b[256];
    snprintf(b, sizeof b, "%s", in);
    if (ioctl(os2dev, OS2CMD_RESOLVE, b) != 0) return -1;
    snprintf(out, n, "%s", b);
    return 0;
}
static char *to_host(const char *p)
{
    char r[1024];
    if ((p[0] && p[1] == ':') || p[0] == '\\') { if (os2_resolve(p, r, sizeof r) == 0) return adup(r); }
    snprintf(r, sizeof r, "%s", p);
    if (p[0] && p[1] == ':' && os2dev < 0) memmove(r, r + 2, strlen(r + 2) + 1);   /* (no drives here: D:\X is \X) */
    for (char *c = r; *c; c++) if (*c == '\\') *c = '/';
    return adup(r);
}
static void know_roots(void)
{
    static int known;
    if (known) return;
    known = 1;
    for (int d = 2; d < 26; d++) {                               /* (C: onwards: those the kernel knows) */
        char in[4] = { (char)('A' + d), ':', '\\', 0 }, out[256];
        if (os2_resolve(in, out, sizeof out) == 0 && out[0] == '/' && !strchr(out, ':')) {
            struct stat st;
            if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(droot[d], sizeof droot[d], "%s", out);
        }
    }
    for (int d = 3; d < 26; d++) if (droot[d][0] && !strcmp(droot[d], droot[2])) droot[d][0] = 0;   /* (a drive that isn't: as C:) */
}
static char *to_os2(const char *h)
{
    know_roots();
    int best = -1; size_t bl = 0;
    for (int d = 2; d < 26; d++) {
        size_t l = strlen(droot[d]);
        if (!l) continue;
        int root = l == 1;
        if ((root || (!strncmp(h, droot[d], l) && (h[l] == '/' || !h[l]))) && (best < 0 || l > bl)) { best = d; bl = l; }
    }
    if (best < 0) return adup(h);
    const char *rest = h + (bl == 1 ? 1 : bl);
    if (*rest == '/') rest++;
    char *r = afmt("%c:\\%s", 'A' + best, rest);
    for (char *c = r; *c; c++) if (*c == '/') *c = '\\';
    return r;
}
typedef struct { char name[256]; FILE *f; int mode; } stream_t;  /* (mode: 0 read, 1 write) */
static stream_t streams[16];
static FILE *stream_get(const char *name0, int write)
{
    const char *name = to_host(name0);
    for (int i = 0; i < 16; i++) if (streams[i].f && !strcmp(streams[i].name, name)) {
        if (streams[i].mode == write) return streams[i].f;
        fclose(streams[i].f); streams[i].f = 0;
    }
    for (int i = 0; i < 16; i++) if (!streams[i].f) {
        FILE *f = fopen(name, write ? "a" : "r");
        if (!f) return 0;
        snprintf(streams[i].name, sizeof streams[i].name, "%s", name);
        streams[i].f = f; streams[i].mode = write;
        return f;
    }
    return 0;
}
/* SysFileTree (spec, stem, options): F files, D directories, B both, S into subdirectories, O names only, T and L time formats */
static void tree_walk(const char *dir, const char *pat, const char *opt, const char *stem, long *k)
{
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) return;
    int files = !strchr(opt, 'D') || strchr(opt, 'B'), dirs = !strchr(opt, 'F') || strchr(opt, 'B');
    if (strchr(opt, 'F') && !strchr(opt, 'B')) dirs = 0;
    if (strchr(opt, 'D') && !strchr(opt, 'B')) files = 0;
    char **subs = 0; int ns = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "", e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (isdir && strchr(opt, 'S')) { subs = realloc(subs, (size_t)(ns + 1) * sizeof *subs); subs[ns++] = strdup(path); }
        if (fnmatch(pat, e->d_name, FNM_CASEFOLD) && strcmp(pat, "*.*")) continue;
        if (isdir ? !dirs : !files) continue;
        char *full = path[0] == '/' ? to_os2(path) : adup(path);
        if (path[0] != '/') for (char *c = full; *c; c++) if (*c == '/') *c = '\\';
        char *line;
        if (strchr(opt, 'O')) line = full;
        else {
            struct tm tm; localtime_r(&st.st_mtime, &tm);
            char at[6] = { isdir ? '-' : 'A', isdir ? 'D' : '-', e->d_name[0] == '.' ? 'H' : '-', access(path, W_OK) ? 'R' : '-', '-', 0 };
            if (strchr(opt, 'L')) line = afmt("%04d-%02d-%02d %02d:%02d:%02d %10lld  %s  %s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)st.st_size, at, full);
            else if (strchr(opt, 'T')) line = afmt("%02d/%02d/%02d/%02d/%02d %10lld  %s  %s", tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, (long long)st.st_size, at, full);
            else line = afmt("%2d/%02d/%02d %2d:%02d%c %10lld  %s  %s", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100, tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min,
                             tm.tm_hour < 12 ? 'a' : 'p', (long long)(isdir ? 0 : st.st_size), at, full);
        }
        (*k)++;
        var_set(afmt("%s%ld", stem, *k), line);
    }
    closedir(d);
    for (int i = 0; i < ns; i++) { tree_walk(subs[i], pat, opt, stem, k); free(subs[i]); }
    free(subs);
}
static char *sys_file_tree(const char *spec, const char *stem0, const char *opt0)
{
    char *opt = adup(opt0); for (char *c = opt; *c; c++) *c = (char)toupper((unsigned char)*c);
    char *stem = adup(stem0); for (char *c = stem; *c; c++) *c = (char)toupper((unsigned char)*c);
    if (!*stem) return adup("2");
    if (stem[strlen(stem) - 1] != '.') stem = concat(stem, ".", 0);
    char *sp = adup(spec);
    char *bs = strrchr(sp, '\\'), *fs = strrchr(sp, '/'), *cut = bs > fs ? bs : fs;
    if (!cut && sp[0] && sp[1] == ':') cut = sp + 1;
    char *dir = cut ? astr(sp, (size_t)(cut - sp + 1)) : adup(""), *pat = adup(cut ? cut + 1 : sp);
    if (!*pat) pat = adup("*");
    char *hd = *dir ? to_host(dir) : adup("");
    size_t l = strlen(hd);
    if (l > 1 && hd[l - 1] == '/') hd[l - 1] = 0;
    long k = 0;
    tree_walk(hd, pat, opt, stem, &k);
    var_set(afmt("%s0", stem), fmtnum((double)k));
    return adup("0");
}
static char *builtin(const char *nm, char **a, int na, int *found)
{
    *found = 1;
    #define S(i) arg_str(a, na, i, "")
    #define N(i, d) arg_num(a, na, i, d)
    if (!strcmp(nm, "LENGTH")) return fmtnum((double)strlen(S(0)));
    if (!strcmp(nm, "SUBSTR")) {
        const char *s = S(0); long l = (long)strlen(s), st = N(1, 1), len = N(2, l - st + 1 > 0 ? l - st + 1 : 0);
        const char *pad = arg_str(a, na, 3, " ");
        char *r = amalloc((size_t)(len > 0 ? len : 0) + 1);
        for (long i = 0; i < len; i++) r[i] = st - 1 + i < l && st - 1 + i >= 0 ? s[st - 1 + i] : pad[0];
        r[len > 0 ? len : 0] = 0;
        return r;
    }
    if (!strcmp(nm, "LEFT") || !strcmp(nm, "RIGHT") || !strcmp(nm, "CENTER") || !strcmp(nm, "CENTRE")) {
        const char *s = S(0), *pad = arg_str(a, na, 2, " ");
        long l = (long)strlen(s), len = N(1, 0);
        char *r = amalloc((size_t)len + 1);
        for (long i = 0; i < len; i++) {
            long j = nm[0] == 'L' ? i : nm[0] == 'R' ? l - len + i : (l - len) / 2 + i;
            r[i] = j >= 0 && j < l ? s[j] : pad[0];
        }
        r[len] = 0;
        return r;
    }
    if (!strcmp(nm, "POS") || !strcmp(nm, "INDEX")) {
        const char *nd = S(0), *h = S(1); long st = N(2, 1);
        if ((size_t)st > strlen(h) + 1) return adup("0");
        const char *f = *nd ? strstr(h + st - 1, nd) : 0;
        return fmtnum(f ? (double)(f - h + 1) : 0);
    }
    if (!strcmp(nm, "LASTPOS")) {
        const char *nd = S(0), *h = S(1); size_t ln = strlen(nd), lh = strlen(h);
        long st = N(2, (long)lh);
        for (long i = (st < (long)lh ? st : (long)lh) - (long)ln; i >= 0; i--) if (ln && !strncmp(h + i, nd, ln)) return fmtnum((double)i + 1);
        return adup("0");
    }
    if (!strcmp(nm, "WORD")) { size_t b, e; return word_bounds(S(0), N(1, 1), &b, &e) ? astr(S(0) + b, e - b) : adup(""); }
    if (!strcmp(nm, "WORDS")) return fmtnum((double)count_words(S(0)));
    if (!strcmp(nm, "WORDINDEX")) { size_t b, e; return fmtnum(word_bounds(S(0), N(1, 1), &b, &e) ? (double)b + 1 : 0); }
    if (!strcmp(nm, "WORDLENGTH")) { size_t b, e; return fmtnum(word_bounds(S(0), N(1, 1), &b, &e) ? (double)(e - b) : 0); }
    if (!strcmp(nm, "SUBWORD")) {
        const char *s = S(0); long w = N(1, 1), k = N(2, -1);
        size_t b, e, b2, e2;
        if (!word_bounds(s, w, &b, &e)) return adup("");
        long last = k < 0 ? count_words(s) : w + k - 1;
        if (k == 0) return adup("");
        if (!word_bounds(s, last, &b2, &e2)) word_bounds(s, count_words(s), &b2, &e2);
        return astr(s + b, e2 - b);
    }
    if (!strcmp(nm, "WORDPOS")) {
        const char *ph = S(0), *s = S(1);
        long np = count_words(ph), ns = count_words(s);
        for (long w = N(2, 1); w + np - 1 <= ns; w++) {
            int ok = 1;
            for (long k = 0; k < np && ok; k++) {
                size_t b1, e1, b2, e2;
                word_bounds(ph, k + 1, &b1, &e1); word_bounds(s, w + k, &b2, &e2);
                ok = e1 - b1 == e2 - b2 && !strncmp(ph + b1, s + b2, e1 - b1);
            }
            if (ok && np) return fmtnum((double)w);
        }
        return adup("0");
    }
    if (!strcmp(nm, "DELWORD")) {
        const char *s = S(0); long w = N(1, 1), k = N(2, -1);
        size_t b, e, b2, e2;
        if (!word_bounds(s, w, &b, &e)) return adup(s);
        long last = k < 0 ? count_words(s) : w + k - 1;
        if (!word_bounds(s, last + 1, &b2, &e2)) b2 = strlen(s);
        return concat(astr(s, b), s + b2, 0);
    }
    if (!strcmp(nm, "STRIP")) {
        const char *s = S(0), *o = arg_str(a, na, 1, "B"), *ch = arg_str(a, na, 2, " ");
        size_t b = 0, e = strlen(s);
        if (toupper(o[0]) != 'T') while (b < e && s[b] == ch[0]) b++;
        if (toupper(o[0]) != 'L') while (e > b && s[e - 1] == ch[0]) e--;
        return astr(s + b, e - b);
    }
    if (!strcmp(nm, "SPACE")) {
        const char *s = S(0), *pad = arg_str(a, na, 2, " "); long k = N(1, 1), nw = count_words(s);
        char *r = adup("");
        for (long w = 1; w <= nw; w++) {
            size_t b, e;
            word_bounds(s, w, &b, &e);
            r = concat(r, astr(s + b, e - b), 0);
            if (w < nw) for (long j = 0; j < k; j++) r = concat(r, astr(pad, 1), 0);
        }
        return r;
    }
    if (!strcmp(nm, "COPIES")) {
        const char *s = S(0); long k = N(1, 0); size_t l = strlen(s);
        char *r = amalloc(l * (size_t)(k > 0 ? k : 0) + 1);
        for (long i = 0; i < k; i++) memcpy(r + l * (size_t)i, s, l);
        r[l * (size_t)(k > 0 ? k : 0)] = 0;
        return r;
    }
    if (!strcmp(nm, "REVERSE")) { char *r = adup(S(0)); size_t l = strlen(r); for (size_t i = 0; i < l / 2; i++) { char c = r[i]; r[i] = r[l - 1 - i]; r[l - 1 - i] = c; } return r; }
    if (!strcmp(nm, "TRANSLATE")) {
        char *r = adup(S(0));
        if (na <= 1) { for (char *c = r; *c; c++) *c = (char)toupper((unsigned char)*c); return r; }
        const char *out = arg_str(a, na, 1, ""), *in = arg_str(a, na, 2, ""), *pad = arg_str(a, na, 3, " ");
        for (char *c = r; *c; c++) {
            const char *f = strchr(in, *c);
            if (f && *c) { size_t k = (size_t)(f - in); *c = k < strlen(out) ? out[k] : pad[0]; }
        }
        return r;
    }
    if (!strcmp(nm, "UPPER")) { char *r = adup(S(0)); for (char *c = r; *c; c++) *c = (char)toupper((unsigned char)*c); return r; }
    if (!strcmp(nm, "LOWER")) { char *r = adup(S(0)); for (char *c = r; *c; c++) *c = (char)tolower((unsigned char)*c); return r; }
    if (!strcmp(nm, "INSERT") || !strcmp(nm, "OVERLAY")) {
        const char *nw = S(0), *tg = S(1), *pad = arg_str(a, na, 4, " ");
        long st = N(2, nm[0] == 'I' ? 0 : 1), len = N(3, (long)strlen(nw));
        char *ns = amalloc((size_t)len + 1);
        for (long i = 0; i < len; i++) ns[i] = (size_t)i < strlen(nw) ? nw[i] : pad[0];
        ns[len] = 0;
        long tl = (long)strlen(tg), at = nm[0] == 'I' ? st : st - 1;
        char *pre = amalloc((size_t)(at > tl ? at : tl) + 1);
        for (long i = 0; i < at; i++) pre[i] = i < tl ? tg[i] : pad[0];
        pre[at] = 0;
        const char *rest = nm[0] == 'I' ? (at < tl ? tg + at : "") : (at + len < tl ? tg + at + len : "");
        return concat(concat(pre, ns, 0), rest, 0);
    }
    if (!strcmp(nm, "DELSTR")) {
        const char *s = S(0); long st = N(1, 1), l = (long)strlen(s), len = N(2, l - st + 1);
        if (st > l) return adup(s);
        return concat(astr(s, (size_t)(st - 1)), st - 1 + len < l ? s + st - 1 + len : "", 0);
    }
    if (!strcmp(nm, "VERIFY")) {
        const char *s = S(0), *ref = S(1), *o = arg_str(a, na, 2, "N");
        for (long i = N(3, 1) - 1; s[i]; i++) {
            int in = strchr(ref, s[i]) != 0;
            if (toupper(o[0]) == 'M' ? in : !in) return fmtnum((double)i + 1);
        }
        return adup("0");
    }
    if (!strcmp(nm, "COMPARE")) {
        const char *x = S(0), *y = S(1), *pad = arg_str(a, na, 2, " ");
        size_t lx = strlen(x), ly = strlen(y), m = lx > ly ? lx : ly;
        for (size_t i = 0; i < m; i++) if ((i < lx ? x[i] : pad[0]) != (i < ly ? y[i] : pad[0])) return fmtnum((double)i + 1);
        return adup("0");
    }
    if (!strcmp(nm, "ABS")) return fmtnum(fabs(num(S(0))));
    if (!strcmp(nm, "SIGN")) { double v = num(S(0)); return adup(v > 0 ? "1" : v < 0 ? "-1" : "0"); }
    if (!strcmp(nm, "MAX") || !strcmp(nm, "MIN")) {
        double r = num(S(0));
        for (int i = 1; i < na; i++) { double v = num(S(i)); if (nm[1] == 'A' ? v > r : v < r) r = v; }
        return fmtnum(r);
    }
    if (!strcmp(nm, "TRUNC")) {
        double v = num(S(0)); long d = N(1, 0);
        double p = pow(10, (double)d), t2 = trunc(v * p) / p;
        return d ? afmt("%.*f", (int)d, t2) : fmtnum(trunc(v));
    }
    if (!strcmp(nm, "FORMAT")) {
        double v = num(S(0)); long before = N(1, -1), after = N(2, -1);
        char b[128];
        if (after >= 0) snprintf(b, sizeof b, "%.*f", (int)after, v); else snprintf(b, sizeof b, "%s", fmtnum(v));
        if (before > 0) { char *dot = strchr(b, '.'); long ip = dot ? (long)(dot - b) : (long)strlen(b); if (ip < before) return afmt("%*s%s", (int)(before - ip), "", b); }
        return adup(b);
    }
    if (!strcmp(nm, "RANDOM")) {
        static int seeded; if (!seeded) { srand((unsigned)time(0)); seeded = 1; }
        long lo = na >= 2 ? N(0, 0) : 0, hi = na >= 2 ? N(1, 999) : na == 1 ? N(0, 999) : 999;
        return fmtnum((double)(lo + rand() % (hi - lo + 1)));
    }
    if (!strcmp(nm, "DATATYPE")) {
        const char *s = S(0), *ty = arg_str(a, na, 1, "");
        int isn = is_num(s, 0);
        if (!*ty) return adup(isn ? "NUM" : "CHAR");
        int ok = 1;
        switch (toupper(ty[0])) {
        case 'N': ok = isn; break;
        case 'W': { double v; ok = is_num(s, &v) && v == trunc(v); break; }
        case 'A': for (const char *c = s; *c; c++) ok &= isalnum((unsigned char)*c) != 0; ok &= *s != 0; break;
        case 'U': for (const char *c = s; *c; c++) ok &= isupper((unsigned char)*c) != 0; ok &= *s != 0; break;
        case 'L': for (const char *c = s; *c; c++) ok &= islower((unsigned char)*c) != 0; ok &= *s != 0; break;
        case 'M': for (const char *c = s; *c; c++) ok &= isalpha((unsigned char)*c) != 0; ok &= *s != 0; break;
        case 'X': for (const char *c = s; *c; c++) ok &= isxdigit((unsigned char)*c) || *c == ' '; break;
        case 'B': for (const char *c = s; *c; c++) ok &= *c == '0' || *c == '1' || *c == ' '; break;
        case 'S': ok = *s && (symch((unsigned char)*s)); for (const char *c = s; *c; c++) ok &= symch((unsigned char)*c); break;
        }
        return adup(ok ? "1" : "0");
    }
    if (!strcmp(nm, "C2D")) { const char *s = S(0); double v = 0; for (const unsigned char *c = (const unsigned char *)s; *c; c++) v = v * 256 + *c; return fmtnum(v); }
    if (!strcmp(nm, "D2C")) {
        unsigned long long v = (unsigned long long)num(S(0)); char b[16]; int k = 0;
        do { b[k++] = (char)(v & 255); v >>= 8; } while (v && k < 8);
        char *r = amalloc((size_t)k + 1); for (int i = 0; i < k; i++) r[i] = b[k - 1 - i]; r[k] = 0;
        return r;
    }
    if (!strcmp(nm, "C2X")) { const char *s = S(0); char *r = amalloc(strlen(s) * 2 + 1); for (size_t i = 0; s[i]; i++) sprintf(r + 2 * i, "%02X", (unsigned char)s[i]); r[strlen(s) * 2] = 0; return r; }
    if (!strcmp(nm, "X2C")) { size_t m; char *h = hexstr(S(0), strlen(S(0)), 0, &m); char *r = astr(h, m); free(h); return r; }
    if (!strcmp(nm, "D2X")) { long len = N(1, -1); char *r = afmt("%llX", (unsigned long long)num(S(0))); if (len > 0) { size_t l = strlen(r); if ((long)l < len) r = concat(afmt("%0*d", (int)(len - (long)l), 0), r, 0); else r += l - (size_t)len; } return r; }
    if (!strcmp(nm, "X2D")) return fmtnum((double)strtoull(S(0), 0, 16));
    if (!strcmp(nm, "B2X")) { size_t m; char *h = hexstr(S(0), strlen(S(0)), 1, &m); char *r = amalloc(m * 2 + 1); for (size_t i = 0; i < m; i++) sprintf(r + 2 * i, "%02X", (unsigned char)h[i]); r[m * 2] = 0; free(h); char *p = r; while (*p == '0' && p[1]) p++; return p; }
    if (!strcmp(nm, "X2B")) { const char *s = S(0); char *r = amalloc(strlen(s) * 4 + 1); size_t k = 0; for (const char *c = s; *c; c++) { if (*c == ' ') continue; int v = isdigit((unsigned char)*c) ? *c - '0' : toupper(*c) - 'A' + 10; for (int b = 3; b >= 0; b--) r[k++] = (char)('0' + ((v >> b) & 1)); } r[k] = 0; return r; }
    if (!strcmp(nm, "ARG")) {
        int cnt = frame ? frame->nargs : 0;
        if (!na) return fmtnum(cnt);
        long i = N(0, 1);
        const char *o = arg_str(a, na, 1, "");
        int there = frame && i >= 1 && i <= cnt && frame->args[i - 1];
        if (*o) return adup(toupper(o[0]) == 'E' ? (there ? "1" : "0") : (there ? "0" : "1"));
        return adup(there ? frame->args[i - 1] : "");
    }
    if (!strcmp(nm, "DATE")) {
        time_t now = time(0); struct tm tm; localtime_r(&now, &tm); char b[64];
        const char *o = arg_str(a, na, 0, "N");
        static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        switch (toupper(o[0])) {
        case 'S': strftime(b, sizeof b, "%Y%m%d", &tm); break;
        case 'U': strftime(b, sizeof b, "%m/%d/%y", &tm); break;
        case 'E': strftime(b, sizeof b, "%d/%m/%y", &tm); break;
        case 'O': strftime(b, sizeof b, "%y/%m/%d", &tm); break;
        case 'W': strftime(b, sizeof b, "%A", &tm); break;
        case 'M': strftime(b, sizeof b, "%B", &tm); break;
        case 'D': snprintf(b, sizeof b, "%d", tm.tm_yday + 1); break;
        default: snprintf(b, sizeof b, "%d %s %d", tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900);
        }
        return adup(b);
    }
    if (!strcmp(nm, "TIME")) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        time_t now = ts.tv_sec; struct tm tm; localtime_r(&now, &tm); char b[64];
        const char *o = arg_str(a, na, 0, "N");
        switch (toupper(o[0])) {
        case 'S': snprintf(b, sizeof b, "%d", tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec); break;
        case 'M': snprintf(b, sizeof b, "%d", tm.tm_hour * 60 + tm.tm_min); break;
        case 'H': snprintf(b, sizeof b, "%d", tm.tm_hour); break;
        case 'L': snprintf(b, sizeof b, "%02d:%02d:%02d.%06ld", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000); break;
        default: strftime(b, sizeof b, "%H:%M:%S", &tm);
        }
        return adup(b);
    }
    if (!strcmp(nm, "SYMBOL")) {
        const char *s = S(0);
        int ok = *s != 0; for (const char *c = s; *c; c++) ok &= symch((unsigned char)*c);
        if (!ok) return adup("BAD");
        tok_t t = { T_SYM, (char *)s, strlen(s), 0, 0 };
        return adup(var_get(var_name(&t)) ? "VAR" : "LIT");
    }
    if (!strcmp(nm, "VALUE")) {
        tok_t t = { T_SYM, (char *)S(0), strlen(S(0)), 0, 0 };
        char *vn = var_name(&t);
        const char *v = var_get(vn);
        char *old = adup(v ? v : vn);
        if (na > 1 && a[1]) var_set(vn, a[1]);
        return old;
    }
    if (!strcmp(nm, "QUEUED")) return adup("0");
    if (!strcmp(nm, "SOURCELINE")) {
        if (!na || !a[0]) return fmtnum(nsrc);
        int k = (int)num(S(0));
        if (k < 1 || k > nsrc) rx_error(40, "SOURCELINE: no line %d", k);
        return adup(src_lines[k - 1]);
    }
    if (!strcmp(nm, "CONDITION")) {
        int o = toupper(arg_str(a, na, 0, "I")[0]);
        if (cinfo.cond < 0) return adup("");
        switch (o) {
        case 'C': return adup(cond_names[cinfo.cond]);
        case 'I': return adup(cinfo.call ? "CALL" : "SIGNAL");
        case 'D': return adup(cinfo.desc ? cinfo.desc : "");
        case 'S': { trap_t *tr = &traps[cinfo.cond]; return adup(tr->delayed ? "DELAY" : tr->on ? "ON" : "OFF"); }
        }
        rx_error(40, "CONDITION: an option C, D, I or S");
    }
    if (!strcmp(nm, "ERRORTEXT")) {
        static const char *const et[50] = {
            [3] = "Program is unreadable", [4] = "Program interrupted", [5] = "Machine resources exhausted",
            [6] = "Unmatched \"/*\" or quote", [7] = "WHEN or OTHERWISE expected", [8] = "Unexpected THEN or ELSE",
            [9] = "Unexpected WHEN or OTHERWISE", [10] = "Unexpected or unmatched END", [11] = "Control stack full",
            [13] = "Invalid character in program", [14] = "Incomplete DO/SELECT/IF", [15] = "Invalid hexadecimal or binary string",
            [16] = "Label not found", [17] = "Unexpected PROCEDURE", [18] = "THEN expected", [19] = "String or symbol expected",
            [20] = "Symbol expected", [21] = "Invalid data on end of clause", [24] = "Invalid TRACE request",
            [25] = "Invalid sub-keyword found", [26] = "Invalid whole number", [27] = "Invalid DO syntax",
            [28] = "Invalid LEAVE or ITERATE", [29] = "Environment name too long", [30] = "Name or string too long",
            [31] = "Name starts with number or \".\"", [33] = "Invalid expression result", [34] = "Logical value not 0 or 1",
            [35] = "Invalid expression", [36] = "Unmatched \"(\" in expression", [37] = "Unexpected \",\" or \")\"",
            [38] = "Invalid template or pattern", [39] = "Evaluation stack overflow", [40] = "Incorrect call to routine",
            [41] = "Bad arithmetic conversion", [42] = "Arithmetic overflow/underflow", [43] = "Routine not found",
            [44] = "Function did not return data", [45] = "No data specified on function RETURN",
            [46] = "Invalid variable reference", [48] = "Failure in system service", [49] = "Interpretation error" };
        int k = (int)num(S(0));
        if (k < 0 || k > 99) rx_error(40, "ERRORTEXT: 0 to 99");
        return adup(k < 50 && et[k] ? et[k] : "");
    }
    if (!strcmp(nm, "LINEIN")) {
        const char *f = S(0);
        if (!*f) return get_line_stdin();
        FILE *fp = stream_get(f, 0);
        if (!fp) return adup("");
        char *b = 0; size_t cap = 0;
        ssize_t r = getline(&b, &cap, fp);
        char *out = adup("");
        if (r > 0) { while (r && (b[r - 1] == '\n' || b[r - 1] == '\r')) b[--r] = 0; out = adup(b); }
        free(b);
        return out;
    }
    if (!strcmp(nm, "LINEOUT")) {
        const char *f = S(0);
        if (!*f) { if (na > 1) { fputs(S(1), stdout); fputc('\n', stdout); } return adup("0"); }
        if (na < 2 || !a[1]) {                                    /* (no line: the stream closed) */
            for (int i = 0; i < 16; i++) if (streams[i].f && !strcmp(streams[i].name, f)) { fclose(streams[i].f); streams[i].f = 0; }
            return adup("0");
        }
        FILE *fp = stream_get(f, 1);
        if (!fp) return adup("1");
        fputs(S(1), fp); fputc('\n', fp); fflush(fp);
        return adup("0");
    }
    if (!strcmp(nm, "CHAROUT")) {
        const char *f = S(0);
        if (!*f) { fputs(S(1), stdout); return adup("0"); }
        FILE *fp = stream_get(f, 1);
        if (!fp) return fmtnum((double)strlen(S(1)));
        fputs(S(1), fp); fflush(fp);
        return adup("0");
    }
    if (!strcmp(nm, "LINES") || !strcmp(nm, "CHARS")) {
        FILE *fp = stream_get(S(0), 0);
        if (!fp) return adup("0");
        int c = fgetc(fp);
        if (c == EOF) return adup("0");
        ungetc(c, fp);
        return adup("1");
    }
    if (!strcmp(nm, "STREAM")) {
        const char *f = S(0), *o = arg_str(a, na, 1, "S"), *cmd = arg_str(a, na, 2, "");
        if (toupper(o[0]) == 'C') {
            char u[32]; snprintf(u, sizeof u, "%s", cmd); for (char *c = u; *c; c++) *c = (char)toupper((unsigned char)*c);
            if (!strncmp(u, "CLOSE", 5)) { for (int i = 0; i < 16; i++) if (streams[i].f && !strcmp(streams[i].name, f)) { fclose(streams[i].f); streams[i].f = 0; } return adup("READY:"); }
            if (!strncmp(u, "QUERY EXISTS", 12)) return access(to_host(f), F_OK) == 0 ? adup(f) : adup("");
            if (!strncmp(u, "OPEN", 4)) return stream_get(f, strstr(u, "WRITE") != 0) ? adup("READY:") : adup("ERROR:");
            return adup("");
        }
        return adup(access(to_host(f), F_OK) == 0 ? "READY" : "NOTREADY");
    }
    if (!strcmp(nm, "FILESPEC")) {                              /* (option D drive, P path, N name; of a file's spec) */
        const char *o = S(0), *f = S(1);
        const char *colon = f[0] && f[1] == ':' ? f + 2 : f, *bs = strrchr(f, '\\'), *fs = strrchr(f, '/'), *last = bs > fs ? bs : fs;
        const char *name = last ? last + 1 : colon;
        switch (toupper(o[0])) {
        case 'D': return colon != f ? astr(f, 2) : adup("");
        case 'P': return astr(colon, (size_t)(name - colon));
        case 'N': return adup(name);
        }
        rx_error(40, "FILESPEC: an option D, P or N");
    }
    /* registering functions (all of these are here already: nothing to load) */
    if (!strcmp(nm, "RXFUNCADD") || !strcmp(nm, "RXFUNCDROP")) return adup("0");
    if (!strcmp(nm, "RXFUNCQUERY")) return adup("0");            /* (0: registered) */
    if (!strcmp(nm, "SYSLOADFUNCS") || !strcmp(nm, "SYSDROPFUNCS")) return adup("");
    /* RexxUtil */
    if (!strcmp(nm, "SYSFILETREE")) return sys_file_tree(S(0), S(1), arg_str(a, na, 2, "B"));
    if (!strcmp(nm, "SYSFILEDELETE")) { const char *h = to_host(S(0)); if (access(h, F_OK)) return adup("2"); return adup(unlink(h) ? "5" : "0"); }
    if (!strcmp(nm, "SYSMKDIR")) return adup(mkdir(to_host(S(0)), 0755) ? (access(to_host(S(0)), F_OK) == 0 ? "5" : "3") : "0");
    if (!strcmp(nm, "SYSRMDIR")) return adup(rmdir(to_host(S(0))) ? "16" : "0");
    if (!strcmp(nm, "SYSSLEEP")) { double v = num(S(0)); fflush(stdout); struct timespec ts = { (time_t)v, (long)((v - trunc(v)) * 1e9) }; nanosleep(&ts, 0); return adup(""); }
    if (!strcmp(nm, "SYSCLS")) { fputs("\033[2J\033[H", stdout); return adup(""); }
    if (!strcmp(nm, "SYSOS2VER")) return adup("2.45");          /* (Warp 4.52's) */
    if (!strcmp(nm, "SYSBOOTDRIVE")) return adup("C:");
    if (!strcmp(nm, "SYSTEXTSCREENSIZE")) { struct winsize ws; if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row) return afmt("%d %d", ws.ws_row, ws.ws_col); return adup("25 80"); }
    if (!strcmp(nm, "SYSDRIVEMAP")) {
        know_roots();
        char *r = adup(""); int st = toupper(arg_str(a, na, 0, "C")[0]) - 'A';
        if (os2dev < 0) return adup("C:");
        for (int d = st < 2 ? 2 : st; d < 26; d++) if (droot[d][0]) r = concat(r, afmt("%c:", 'A' + d), *r != 0);
        return r;
    }
    if (!strcmp(nm, "SYSGETKEY")) {
        struct termios t0, t1; int raw = tcgetattr(0, &t0) == 0;
        fflush(stdout);
        if (raw) { t1 = t0; t1.c_lflag &= (tcflag_t)~(ICANON | (toupper(arg_str(a, na, 0, "E")[0]) == 'N' ? ECHO : 0)); t1.c_cc[VMIN] = 1; t1.c_cc[VTIME] = 0; tcsetattr(0, TCSANOW, &t1); }
        int c = getchar();
        if (raw) tcsetattr(0, TCSANOW, &t0);
        char b[2] = { (char)(c == EOF ? 0 : c), 0 };
        return adup(b);
    }
    if (!strcmp(nm, "SYSTEMPFILENAME")) {
        const char *tp = S(0), *filler = arg_str(a, na, 1, "?");
        static int seeded; if (!seeded) { srand((unsigned)time(0) ^ (unsigned)getpid()); seeded = 1; }
        for (int tries = 0; tries < 1000; tries++) {
            char *r = adup(tp);
            for (char *c = r; *c; c++) if (*c == filler[0]) *c = (char)('0' + rand() % 10);
            if (access(to_host(r), F_OK) != 0) return r;
        }
        return adup("");
    }
    if (!strcmp(nm, "SYSSEARCHPATH")) {
        const char *ev = getenv(S(0)), *f = S(1);
        if (!ev) return adup("");
        char *list = adup(ev);
        for (char *d = strtok(list, ";:"); d; d = strtok(0, ";:")) {
            char *cand = afmt("%s%s%s", d, d[strlen(d) - 1] == '\\' || d[strlen(d) - 1] == '/' ? "" : "/", f);
            if (access(to_host(cand), F_OK) == 0) { char *h = to_host(cand); return h[0] == '/' ? to_os2(h) : cand; }
        }
        return adup("");
    }
    if (!strcmp(nm, "SYSFILESEARCH")) {                          /* (target, file, stem, options: C case, N line numbers) */
        const char *tg = S(0), *o = arg_str(a, na, 3, "");
        FILE *fp = fopen(to_host(S(1)), "r");
        if (!fp) return adup("3");
        int cs = strchr(o, 'C') || strchr(o, 'c'), ln = strchr(o, 'N') || strchr(o, 'n');
        char *stem = adup(S(2)); for (char *c = stem; *c; c++) *c = (char)toupper((unsigned char)*c);
        if (stem[strlen(stem) - 1] != '.') stem = concat(stem, ".", 0);
        char *b = 0; size_t cap = 0; long k = 0, lno = 0;
        while (getline(&b, &cap, fp) > 0) {
            lno++;
            size_t l = strlen(b); while (l && (b[l - 1] == '\n' || b[l - 1] == '\r')) b[--l] = 0;
            if (cs ? strstr(b, tg) != 0 : strcasestr(b, tg) != 0) { k++; var_set(afmt("%s%ld", stem, k), ln ? afmt("%ld %s", lno, b) : b); }
        }
        free(b); fclose(fp);
        var_set(afmt("%s0", stem), fmtnum((double)k));
        return adup("0");
    }
    #undef S
    #undef N
    *found = 0;
    return 0;
}
static char *call_function(const char *name, char **args, int nargs, int internal_first, int line)
{
    cur_line = line;
    if (internal_first) {                                         /* (an internal routine first: a label of that name) */
        int lab = find_label(name);
        if (lab >= 0) {
            char **copy = malloc((size_t)(nargs ? nargs : 1) * sizeof *copy);
            for (int i = 0; i < nargs; i++) copy[i] = args[i] ? strdup(args[i]) : 0;
            mark_t m = amark();
            char *r = call_routine(lab, copy, nargs, 0);
            char *keep = r ? strdup(r) : 0;
            arelease(m);
            for (int i = 0; i < nargs; i++) free(copy[i]);
            free(copy);
            char *out = keep ? adup(keep) : 0;
            free(keep);
            if (out) var_set("RESULT", out);
            return out ? out : adup("");
        }
    }
    int found;
    char *r = builtin(name, args, nargs, &found);
    if (found) { if (r) var_set("RESULT", r); return r ? r : adup(""); }
    rx_error(43, "routine %s not found", name);
    return 0;
}

int main(int argc, char **argv)
{
    char *src = 0;
    if (argc >= 3 && !strcmp(argv[1], "-e")) { src = strdup(argv[2]); src_name = "(-e)"; argv += 2; argc -= 2; }
    else if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "REX0003: %s: can't be read\n", argv[1]); return 3; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        src = malloc((size_t)n + 1);
        if (fread(src, 1, (size_t)n, f) != (size_t)n) n = 0;
        src[n] = 0; fclose(f);
        src_name = argv[1];
    } else { fprintf(stderr, "usage: rexx file.cmd [arguments] | rexx -e 'instructions'\n"); return 1; }
    /* the argument: one string, as OS/2 gives a REXX program */
    size_t al = 1;
    for (int i = 2; i < argc; i++) al += strlen(argv[i]) + 1;
    argv1 = calloc(1, al);
    for (int i = 2; i < argc; i++) { if (i > 2) strcat(argv1, " "); strcat(argv1, argv[i]); }
    scope = calloc(1, sizeof *scope);
    char *margs[1] = { argv1 };
    frame_t top = { margs, argc > 2 ? 1 : 0, 0, 0, 0, 0 };
    frame = &top;
    for (char *p = src;;) {
        char *nl = strchr(p, '\n');
        src_lines = realloc(src_lines, (size_t)(nsrc + 1) * sizeof *src_lines);
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (l && p[l - 1] == '\r') l--;
        src_lines[nsrc++] = strndup(p, l);
        if (!nl) break;
        p = nl + 1;
    }
    if (nsrc && !*src_lines[nsrc - 1]) nsrc--;
    tokenize(src);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;                                    /* (Ctrl+C: HALT, between clauses) */
    sigaction(SIGINT, &sa, 0);
    int r = run_act(0);
    if (r == R_LEAVE || r == R_ITERATE) rx_error(28, "LEAVE or ITERATE outside a loop");
    fflush(stdout);
    for (int i = 0; i < 16; i++) if (streams[i].f) fclose(streams[i].f);
    double v;
    return exit_val && is_num(exit_val, &v) ? (int)v : 0;
}
