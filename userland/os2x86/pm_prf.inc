/* userland/os2x86/pm_prf.inc - profiles (PMSHAPI's Prf calls: the user's
 * OS2.INI, the system's OS2SYS.INI, a program's own .INI files) and the
 * help manager's calls (HELPMGR). Included at the end of pm.inc.
 *
 * Profiles are kept in OS/2's own binary format, so a program's .INI
 * files from OS/2 read as they are; each change is written back at once.
 * The user's and the system's are USER_INI and SYSTEM_INI (OS/2 file
 * names, as on OS/2), else OS2.INI and OS2SYS.INI in $OS2X86_HOME, else
 * $HOME/.os2x86.
 *
 * The format: a header { 0xFFFFFFFF, the first application's offset, the
 * file's size, 0, 0 }; applications { next, first key, 0, name length
 * (with its NUL) twice, name offset }; keys { next, 0, name length twice,
 * name offset, data length twice, data offset }; all offsets from the
 * start, 0 ending a list. */

typedef struct { char *name; uint8_t *data; uint32_t len; } prfkey_t;
typedef struct { char *name; prfkey_t *k; int nk; } prfapp_t;
typedef struct { int used; char path[1024]; prfapp_t *a; int na; } prf_t;
static prf_t prfs[16];                          /* (0: the user's, 1: the system's; then PrfOpenProfile's) */

static void prf_clear(prf_t *p)
{
    for (int i = 0; i < p->na; i++) {
        for (int j = 0; j < p->a[i].nk; j++) { free(p->a[i].k[j].name); free(p->a[i].k[j].data); }
        free(p->a[i].k); free(p->a[i].name);
    }
    free(p->a); p->a = 0; p->na = 0;
}
static prfapp_t *prf_app(prf_t *p, const char *name, int make)
{
    for (int i = 0; i < p->na; i++) if (!strcasecmp(p->a[i].name, name)) return &p->a[i];
    if (!make) return 0;
    p->a = realloc(p->a, (size_t)(p->na + 1) * sizeof *p->a);
    prfapp_t *a = &p->a[p->na++];
    memset(a, 0, sizeof *a);
    a->name = strdup(name);
    return a;
}
static prfkey_t *prf_key(prfapp_t *a, const char *name, int make)
{
    for (int i = 0; i < a->nk; i++) if (!strcasecmp(a->k[i].name, name)) return &a->k[i];
    if (!make) return 0;
    a->k = realloc(a->k, (size_t)(a->nk + 1) * sizeof *a->k);
    prfkey_t *k = &a->k[a->nk++];
    memset(k, 0, sizeof *k);
    k->name = strdup(name);
    return k;
}
static uint32_t prd32(const uint8_t *d) { return d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24; }
static void prf_load(prf_t *p)
{
    FILE *f = fopen(p->path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 && n < (64 << 20) ? malloc((size_t)n) : 0;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n || n < 20 || prd32(d) != 0xFFFFFFFFu) { free(d); fclose(f); return; }
    fclose(f);
    #define IN(o, len) ((o) && (uint64_t)(o) + (len) <= (uint64_t)n)
    int guard = 0;
    for (uint32_t ao = prd32(d + 4); IN(ao, 20) && guard++ < 100000; ao = prd32(d + ao)) {
        uint32_t nl = (uint32_t)(d[ao + 12] | d[ao + 13] << 8), no = prd32(d + ao + 16);
        if (!IN(no, nl) || !nl) break;
        char *an = strndup((char *)d + no, nl);
        prfapp_t *a = prf_app(p, an, 1);
        free(an);
        for (uint32_t ko = prd32(d + ao + 4); IN(ko, 24) && guard++ < 100000; ko = prd32(d + ko)) {
            uint32_t kl = (uint32_t)(d[ko + 8] | d[ko + 9] << 8), kn = prd32(d + ko + 12);
            uint32_t vl = (uint32_t)(d[ko + 16] | d[ko + 17] << 8), vo = prd32(d + ko + 20);
            if (!IN(kn, kl) || !kl || (vl && !IN(vo, vl))) break;
            char *knm = strndup((char *)d + kn, kl);
            prfkey_t *k = prf_key(a, knm, 1);
            free(knm);
            free(k->data);
            k->data = malloc(vl ? vl : 1);
            if (vl) memcpy(k->data, d + vo, vl);
            k->len = vl;
        }
    }
    #undef IN
    free(d);
}
static void pput32(uint8_t *d, uint32_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24); }
static void pput16(uint8_t *d, uint32_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }
static void prf_save(prf_t *p)
{
    size_t n = 20;
    for (int i = 0; i < p->na; i++) {
        n += 20 + strlen(p->a[i].name) + 1;
        for (int j = 0; j < p->a[i].nk; j++) n += 24 + strlen(p->a[i].k[j].name) + 1 + p->a[i].k[j].len;
    }
    uint8_t *d = calloc(1, n);
    size_t o = 20;
    pput32(d, 0xFFFFFFFFu); pput32(d + 4, p->na ? 20 : 0); pput32(d + 8, (uint32_t)n);
    for (int i = 0; i < p->na; i++) {
        size_t ao = o, nl = strlen(p->a[i].name) + 1;
        o += 20;
        pput16(d + ao + 12, (uint32_t)nl); pput16(d + ao + 14, (uint32_t)nl); pput32(d + ao + 16, (uint32_t)o);
        memcpy(d + o, p->a[i].name, nl); o += nl;
        pput32(d + ao + 4, p->a[i].nk ? (uint32_t)o : 0);
        for (int j = 0; j < p->a[i].nk; j++) {
            prfkey_t *k = &p->a[i].k[j];
            size_t ko = o, kl = strlen(k->name) + 1;
            o += 24;
            pput16(d + ko + 8, (uint32_t)kl); pput16(d + ko + 10, (uint32_t)kl); pput32(d + ko + 12, (uint32_t)o);
            memcpy(d + o, k->name, kl); o += kl;
            pput16(d + ko + 16, k->len); pput16(d + ko + 18, k->len); pput32(d + ko + 20, (uint32_t)o);
            memcpy(d + o, k->data, k->len); o += k->len;
            pput32(d + ko, j + 1 < p->a[i].nk ? (uint32_t)o : 0);
        }
        pput32(d + ao, i + 1 < p->na ? (uint32_t)o : 0);
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", p->path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        int good = fwrite(d, 1, n, f) == n;
        if (fclose(f) == 0 && good) rename(tmp, p->path); else remove(tmp);
    }
    free(d);
}
/* the user's (0) or the system's (1): where they are, loaded the first time */
static prf_t *prf_std(int which)
{
    prf_t *p = &prfs[which];
    if (p->used) return p;
    p->used = 1;
    const char *env = getenv(which ? "SYSTEM_INI" : "USER_INI");
    int drv;
    if (!env || os2_to_host(env, p->path, sizeof p->path, &drv)) {
        const char *home = getenv("OS2X86_HOME"), *h = getenv("HOME");
        char dir[900];
        if (home && *home) snprintf(dir, sizeof dir, "%s", home);
        else snprintf(dir, sizeof dir, "%s/.os2x86", h && *h ? h : "/tmp");
        mkdir(dir, 0755);
        snprintf(p->path, sizeof p->path, "%s/%s", dir, which ? "OS2SYS.INI" : "OS2.INI");
    }
    prf_load(p);
    return p;
}
/* HINI: 0 (HINI_PROFILE: the user's, then the system's), -1 the user's, -2 the system's, ours from PrfOpenProfile */
static prf_t *prf_get(uint32_t h, int *both)
{
    if (both) *both = h == 0;
    if (h == 0 || h == 0xFFFFFFFFu) return prf_std(0);
    if (h == 0xFFFFFFFEu) return prf_std(1);
    h -= 0x5100;
    return h >= 2 && h < 16 && prfs[h].used ? &prfs[h] : 0;
}
static prfkey_t *prf_find(uint32_t h, const char *app, const char *key)
{
    int both;
    prf_t *p = prf_get(h, &both);
    prfapp_t *a;
    prfkey_t *k = p && (a = prf_app(p, app, 0)) ? prf_key(a, key, 0) : 0;
    if (!k && both && (a = prf_app(prf_std(1), app, 0))) k = prf_key(a, key, 0);
    return k;
}
/* the names, NUL after each and one more at the end: applications (app 0) or an application's keys */
static uint32_t prf_names(uint32_t h, const char *app, uint32_t buf, uint32_t max)
{
    prf_t *p = prf_get(h, 0);
    char *out = 0;
    size_t n = 0;
    FILE *m = open_memstream(&out, &n);
    if (p && !app) for (int i = 0; i < p->na; i++) fwrite(p->a[i].name, 1, strlen(p->a[i].name) + 1, m);
    else if (p) { prfapp_t *a = prf_app(p, app, 0); if (a) for (int i = 0; i < a->nk; i++) fwrite(a->k[i].name, 1, strlen(a->k[i].name) + 1, m); }
    fputc(0, m);
    fclose(m);
    uint32_t r = (uint32_t)n;
    if (buf && max) { if (r > max) r = max; if (ok(buf, r)) memcpy(C.mem + buf, out, r); }
    free(out);
    return r;
}
static uint32_t api_PrfOpenProfile(void)            /* (hab, pszFileName) */
{
    char *s = gs(A(1));
    if (!s) return 0;
    char path[1024];
    int drv;
    if (os2_to_host(s, path, sizeof path, &drv)) return 0;
    for (int i = 2; i < 16; i++) if (prfs[i].used && !strcmp(prfs[i].path, path)) return 0x5100 + (uint32_t)i;
    for (int i = 2; i < 16; i++)
        if (!prfs[i].used) {
            prfs[i].used = 1;
            snprintf(prfs[i].path, sizeof prfs[i].path, "%s", path);
            prf_load(&prfs[i]);
            if (!prfs[i].na) prf_save(&prfs[i]);              /* (a new one: made now, as OS/2 does) */
            return 0x5100 + (uint32_t)i;
        }
    return 0;
}
static uint32_t api_PrfCloseProfile(void)
{
    uint32_t h = A(0) - 0x5100;
    if (h < 2 || h >= 16 || !prfs[h].used) return 0;
    prf_clear(&prfs[h]);
    prfs[h].used = 0;
    return 1;
}
static uint32_t api_PrfQueryProfileString(void)     /* (hini, pszApp, pszKey, pszDefault, pBuffer, cchBufferMax): bytes, with the NUL */
{
    char *app = A(1) ? gs(A(1)) : 0, *key = A(2) ? gs(A(2)) : 0;
    uint32_t buf = A(4), max = A(5);
    if (!app || !key) return prf_names(A(0), app, buf, max);
    prfkey_t *k = prf_find(A(0), app, key);
    char *def = A(3) ? gs(A(3)) : 0;
    if (!k && !def) return 0;
    const char *v = k ? (const char *)k->data : def;
    uint32_t len = k ? k->len : (uint32_t)strlen(def) + 1;
    if (k && (!len || k->data[len - 1])) len++;               /* (data without its NUL: one added) */
    if (!max || !ok(buf, max)) return 0;
    uint32_t c = len > max ? max : len;
    memcpy(C.mem + buf, v, c - 1);
    C.mem[buf + c - 1] = 0;
    return c;
}
static uint32_t api_PrfWriteProfileString(void)     /* (hini, pszApp, pszKey, pszData): no key - the application goes; no data - the key */
{
    prf_t *p = prf_get(A(0), 0);
    char *app = A(1) ? gs(A(1)) : 0;
    if (!p || !app) return 0;
    if (!A(2)) {
        prfapp_t *a = prf_app(p, app, 0);
        if (!a) return 1;
        int i = (int)(a - p->a);
        for (int j = 0; j < a->nk; j++) { free(a->k[j].name); free(a->k[j].data); }
        free(a->k); free(a->name);
        memmove(p->a + i, p->a + i + 1, (size_t)(p->na - i - 1) * sizeof *p->a);
        p->na--;
        prf_save(p);
        return 1;
    }
    char *key = gs(A(2));
    prfapp_t *a = prf_app(p, app, A(3) != 0);
    if (!a || !key) return a ? 0 : 1;
    if (!A(3)) {
        prfkey_t *k = prf_key(a, key, 0);
        if (k) { int i = (int)(k - a->k); free(k->name); free(k->data); memmove(a->k + i, a->k + i + 1, (size_t)(a->nk - i - 1) * sizeof *a->k); a->nk--; }
        prf_save(p);
        return 1;
    }
    char *v = gs(A(3));
    if (!v) return 0;
    prfkey_t *k = prf_key(a, key, 1);
    free(k->data);
    k->len = (uint32_t)strlen(v) + 1;
    k->data = (uint8_t *)strdup(v);
    prf_save(p);
    return 1;
}
static uint32_t api_PrfQueryProfileInt(void)        /* (hini, pszApp, pszKey, sDefault) */
{
    char *app = A(1) ? gs(A(1)) : 0, *key = A(2) ? gs(A(2)) : 0;
    prfkey_t *k = app && key ? prf_find(A(0), app, key) : 0;
    if (!k || !k->len) return A(3);
    char b[32];
    snprintf(b, sizeof b, "%.*s", (int)(k->len < 31 ? k->len : 31), (char *)k->data);
    return (uint32_t)(int32_t)atol(b);
}
static uint32_t api_PrfQueryProfileSize(void)       /* (hini, pszApp, pszKey, pulReqLen) */
{
    char *app = A(1) ? gs(A(1)) : 0, *key = A(2) ? gs(A(2)) : 0;
    uint32_t n;
    if (!app || !key) n = prf_names(A(0), app, 0, 0);
    else { prfkey_t *k = prf_find(A(0), app, key); if (!k) return 0; n = k->len; }
    if (A(3)) p32(A(3), n);
    return 1;
}
static uint32_t api_PrfQueryProfileData(void)       /* (hini, pszApp, pszKey, pBuffer, pulBufferMax) */
{
    char *app = A(1) ? gs(A(1)) : 0, *key = A(2) ? gs(A(2)) : 0;
    uint32_t max = A(4) ? g32(A(4)) : 0;
    if (!app || !key) { uint32_t n = prf_names(A(0), app, A(3), max); if (A(4)) p32(A(4), n); return 1; }
    prfkey_t *k = prf_find(A(0), app, key);
    if (!k) return 0;
    uint32_t n = k->len < max ? k->len : max;
    if (n && ok(A(3), n)) memcpy(C.mem + A(3), k->data, n);
    if (A(4)) p32(A(4), n);
    return k->len <= max;                                    /* (too small: what fits, and FALSE) */
}
static uint32_t api_PrfWriteProfileData(void)       /* (hini, pszApp, pszKey, pData, cchDataLen) */
{
    prf_t *p = prf_get(A(0), 0);
    char *app = A(1) ? gs(A(1)) : 0, *key = A(2) ? gs(A(2)) : 0;
    if (!p || !app) return 0;
    if (!key || !A(3)) return api_PrfWriteProfileString();  /* (deletions: the same) */
    uint32_t n = A(4);
    if (n > 0xFFFF || !ok(A(3), n)) return 0;
    prfkey_t *k = prf_key(prf_app(p, app, 1), key, 1);
    free(k->data);
    k->data = malloc(n ? n : 1);
    memcpy(k->data, C.mem + A(3), n);
    k->len = n;
    prf_save(p);
    return 1;
}
static uint32_t api_PrfReset(void) { return 1; }
static uint32_t api_PrfQueryProfile(void) { return 0; }

/* ── the help manager: an instance the program can talk to; help itself
 * (the .HLP files' panels) isn't shown yet: a message says so ── */
static char help_lib[260], help_title[128];
static uint32_t help_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    (void)w; (void)mp1; (void)mp2;
    switch (msg) {
    case 0x0222: case 0x0223: case 0x022a: case 0x022b: case 0x022c: {   /* HM_DISPLAY_HELP, HM_EXT_HELP, HM_HELP_INDEX, CONTENTS, KEYS_HELP */
        char t[400];
        snprintf(t, sizeof t, "The help for this program%s%s%s can't be shown here yet.", help_lib[0] ? " (" : "", help_lib, help_lib[0] ? ")" : "");
        pm_msgbox(t, help_title[0] ? help_title : "Help", 0x0040 | 0x4000);   /* MB_INFORMATION, MB_MOVEABLE */
        return 0;
    }
    case 0x0224: return 0;                                   /* HM_SET_ACTIVE_WINDOW */
    }
    return 0;
}
static uint32_t help_assoc[64][2];                           /* (window, help instance) */
static uint32_t api_WinCreateHelpInstance(void)     /* (hab, pHelpInit: cb, rc, tutorial, table, ..., title at 32, library at 40) */
{
    pmwin_t *w = win_new(0, "#help", 0, 0);
    if (!w) return 0;
    w->iproc = help_proc;
    uint32_t hi = A(1);
    if (hi && ok(hi, 44)) {
        char *t = g32(hi + 32) ? gs(g32(hi + 32)) : 0, *l = g32(hi + 40) ? gs(g32(hi + 40)) : 0;
        snprintf(help_title, sizeof help_title, "%s", t ? t : "");
        snprintf(help_lib, sizeof help_lib, "%s", l ? l : "");
        p32(hi + 4, 0);                                      /* ulReturnCode: none */
    }
    return w->hwnd;
}
static uint32_t api_WinDestroyHelpInstance(void)
{
    pmwin_t *w = win(A(0));
    if (!w || strcmp(w->cls, "#help")) return 0;
    for (int i = 0; i < 64; i++) if (help_assoc[i][1] == A(0)) help_assoc[i][0] = help_assoc[i][1] = 0;
    win_destroy(w);
    return 1;
}
static uint32_t api_WinAssociateHelpInstance(void)  /* (hwndHelpInstance, hwndApp): 0 - the association goes */
{
    for (int i = 0; i < 64; i++) if (help_assoc[i][0] == A(1)) { help_assoc[i][1] = A(0); if (!A(0)) help_assoc[i][0] = 0; return 1; }
    if (!A(0)) return 1;
    for (int i = 0; i < 64; i++) if (!help_assoc[i][0]) { help_assoc[i][0] = A(1); help_assoc[i][1] = A(0); return 1; }
    return 0;
}
static uint32_t api_WinQueryHelpInstance(void)
{
    for (pmwin_t *w = win(A(0)); w; w = w->parent ? w->parent : w->owner)
        for (int i = 0; i < 64; i++) if (help_assoc[i][0] == w->hwnd) return help_assoc[i][1];
    return 0;
}
static uint32_t api_WinLoadHelpTable(void) { return win(A(0)) ? 1 : 0; }
static uint32_t api_WinCreateHelpTable(void) { return win(A(0)) ? 1 : 0; }
