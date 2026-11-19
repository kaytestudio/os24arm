/* ── IPF help (pm.inc): the help manager (HELPMGR: WinCreateHelpInstance,
 * help tables - HELPTABLE and HELPSUBTABLE resources, or the program's
 * own - the HM_ messages, F1 as WM_HELP) and its window, which shows the
 * panels of .HLP and .INF files (IPF's compiled form, as IBM's IPFC and
 * OpenWatcom's wipfc write it): the text in its styles (:hp1.-:hp9.),
 * colours and fonts, margins (lists, definitions), examples and lines kept
 * as they are, links (to panels, to footnotes; "inform" to the program as
 * HM_INFORM), Contents and Index made from the file's tables, Previous.
 * Not shown: pictures (a [picture] in their place), split windows (one
 * panel at a time), searching. "os2x86 --view FILE.INF [topic]": VIEW's
 * job, the file on its own. ── */

/* IPF's words are code page 850; the fonts here, Latin-1 */
static const uint8_t cp850_l1[256] = {
    0x00, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xb7, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x3e, 0x3c, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x5e, 0x76, 0x3e, 0x3c, 0x20, 0x20, 0x5e, 0x76,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
    0xc7, 0xfc, 0xe9, 0xe2, 0xe4, 0xe0, 0xe5, 0xe7, 0xea, 0xeb, 0xe8, 0xef, 0xee, 0xec, 0xc4, 0xc5,
    0xc9, 0xe6, 0xc6, 0xf4, 0xf6, 0xf2, 0xfb, 0xf9, 0xff, 0xd6, 0xdc, 0xf8, 0xa3, 0xd8, 0xd7, 0x66,
    0xe1, 0xed, 0xf3, 0xfa, 0xf1, 0xd1, 0xaa, 0xba, 0xbf, 0xae, 0xac, 0xbd, 0xbc, 0xa1, 0xab, 0xbb,
    0x23, 0x23, 0x23, 0x7c, 0x2b, 0xc1, 0xc2, 0xc0, 0xa9, 0x2b, 0x7c, 0x2b, 0x2b, 0xa2, 0xa5, 0x2b,
    0x2b, 0x2b, 0x2b, 0x2b, 0x2d, 0x2b, 0xe3, 0xc3, 0x2b, 0x2b, 0x2b, 0x2b, 0x2b, 0x3d, 0x2b, 0xa4,
    0xf0, 0xd0, 0xca, 0xcb, 0xc8, 0x69, 0xcd, 0xce, 0xcf, 0x2b, 0x2b, 0x23, 0x5f, 0xa6, 0xcc, 0x2d,
    0xd3, 0xdf, 0xd4, 0xd2, 0xf5, 0xd5, 0xb5, 0xfe, 0xde, 0xda, 0xdb, 0xd9, 0xfd, 0xdd, 0xaf, 0xb4,
    0xad, 0xb1, 0x5f, 0xbe, 0xb6, 0xa7, 0xf7, 0xb8, 0xb0, 0xa8, 0xb7, 0xb9, 0xb3, 0xb2, 0x23, 0xa0,
};
typedef struct {
    uint8_t *d;
    uint32_t n;
    uint32_t ntoc, tocstart, nres, resstart, nname, namestart, nindex, indexstart, indexlen;
    uint32_t nslots, slotsstart, ndict, dictstart, dictlen, nfont, fontstart;
    char **word;                        /* the dictionary, in Latin-1 */
    char title[64];
    char path[PATH_MAX];                /* (where it is: books it links to are looked for there first) */
} inf_t;
#define INF_FITS(f, o, len) ((uint64_t)(o) + (uint64_t)(len) <= (f)->n)
static uint32_t inf16(inf_t *f, uint64_t o) { return o + 2 <= f->n ? (uint32_t)f->d[o] | (uint32_t)f->d[o + 1] << 8 : 0; }
static uint32_t inf32(inf_t *f, uint64_t o) { return o + 4 <= f->n ? inf16(f, o) | inf16(f, o + 2) << 16 : 0; }
static void inf_l1(char *out, size_t on, const uint8_t *s, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < on; i++) { uint8_t c = cp850_l1[s[i]]; if (c) out[k++] = (char)c; }
    out[k] = 0;
}
static void inf_free(inf_t *f)
{
    if (!f) return;
    if (f->word) for (uint32_t i = 0; i < f->ndict; i++) free(f->word[i]);
    free(f->word);
    free(f->d);
    free(f);
}
static inf_t *inf_load(const char *host)
{
    FILE *fp = fopen(host, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    inf_t *f = n >= 155 && n < (64L << 20) ? calloc(1, sizeof *f) : 0;
    if (f) { f->d = malloc((size_t)n); f->n = (uint32_t)n; }
    if (!f || !f->d || fread(f->d, 1, (size_t)n, fp) != (size_t)n || memcmp(f->d, "HS", 2)) { fclose(fp); inf_free(f); return 0; }
    fclose(fp);
    if (!realpath(host, f->path)) snprintf(f->path, sizeof f->path, "%s", host);
    f->ntoc = inf16(f, 8); f->tocstart = inf32(f, 18);
    f->nres = inf16(f, 22); f->resstart = inf32(f, 24);
    f->nname = inf16(f, 28); f->namestart = inf32(f, 30);
    f->nindex = inf16(f, 34); f->indexstart = inf32(f, 36); f->indexlen = inf32(f, 40);
    f->nslots = inf16(f, 62); f->slotsstart = inf32(f, 64);
    f->dictlen = inf32(f, 68); f->ndict = inf16(f, 72); f->dictstart = inf32(f, 74);
    if (!INF_FITS(f, f->tocstart, 4ull * f->ntoc) || !INF_FITS(f, f->resstart, 4ull * f->nres) || !INF_FITS(f, f->namestart, 4ull * f->nname) ||
        !INF_FITS(f, f->indexstart, f->indexlen) || !INF_FITS(f, f->slotsstart, 4ull * f->nslots) || !INF_FITS(f, f->dictstart, f->dictlen)) { inf_free(f); return 0; }
    inf_l1(f->title, sizeof f->title, f->d + 107, strnlen((char *)f->d + 107, 48));
    f->word = calloc(f->ndict + 1u, sizeof *f->word);
    uint32_t p = f->dictstart, end = f->dictstart + f->dictlen;
    for (uint32_t i = 0; i < f->ndict; i++) {
        uint32_t l = p < end ? f->d[p] : 0;
        char b[300] = "";
        if (l >= 1 && p + l <= end) { inf_l1(b, sizeof b, f->d + p + 1, l - 1); p += l; }
        else p = end;
        f->word[i] = strdup(b);
    }
    uint32_t ext = inf32(f, 91);
    if (ext && INF_FITS(f, ext, 6)) {
        f->nfont = inf16(f, ext); f->fontstart = inf32(f, ext + 2);
        if (!INF_FITS(f, f->fontstart, 39ull * f->nfont)) f->nfont = 0;
    }
    return f;
}
/* a panel's entry in the table of contents: its level (0: a footnote's), flags, slots, title */
typedef struct { int level, flags, nslot; uint32_t slots; char title[256]; } inftoc_t;
static int inf_toc(inf_t *f, uint32_t i, inftoc_t *t)
{
    if (i >= f->ntoc) return 0;
    uint32_t o = inf32(f, f->tocstart + 4ull * i);
    if (!INF_FITS(f, o, 3)) return 0;
    uint32_t len = f->d[o], end = o + len, q = o + 3;
    if (len < 3 || end > f->n) return 0;
    t->flags = f->d[o + 1]; t->level = t->flags & 15; t->nslot = f->d[o + 2];
    if (t->flags & 0x20) {                                   /* (its window's: origin, size, style, group, control area) */
        if (q + 2 > end) return 0;
        int w1 = f->d[q], w2 = f->d[q + 1];
        q += 2;
        if (w1 & 1) q += 5;
        if (w1 & 2) q += 5;
        if (w1 & 4) q += 2;
        if (w1 & 8) q += 2;
        if (w2 & 4) q += 2;
    }
    t->slots = q;
    q += 2u * (uint32_t)t->nslot;
    if (q > end) return 0;
    inf_l1(t->title, sizeof t->title, f->d + q, end - q);
    return 1;
}
static int inf_res(inf_t *f, uint32_t res)                   /* res= : its panel */
{
    for (uint32_t i = 0; i < f->nres; i++)
        if (inf16(f, f->resstart + 2ull * i) == res) return (int)inf16(f, f->resstart + 2ull * f->nres + 2ull * i);
    return -1;
}
static int inf_name(inf_t *f, const char *name)              /* name= or id= : its panel */
{
    for (uint32_t i = 0; i < f->nname; i++) {
        uint32_t w = inf16(f, f->namestart + 2ull * i);
        if (w < f->ndict && !strcasecmp(f->word[w], name)) return (int)inf16(f, f->namestart + 2ull * f->nname + 2ull * i);
    }
    return -1;
}

/* a topic: a panel's number, its name, a word of its title, of the index's; -1: none */
static int inf_find(inf_t *f, const char *topic)
{
    char *e;
    long r = strtol(topic, &e, 10);
    int page = *topic && !*e ? inf_res(f, (uint32_t)r) : inf_name(f, topic);
    inftoc_t t;
    for (uint32_t i = 0; page < 0 && i < f->ntoc; i++) if (inf_toc(f, i, &t) && t.level && !strcasecmp(t.title, topic)) page = (int)i;
    for (uint32_t i = 0; page < 0 && i < f->ntoc; i++) if (inf_toc(f, i, &t) && t.level && strcasestr(t.title, topic)) page = (int)i;
    uint32_t p = f->indexstart, end = f->indexstart + f->indexlen;
    for (uint32_t k = 0; page < 0 && k < f->nindex && p + 5 <= end; k++) {
        uint32_t len = f->d[p];
        char s[300];
        if (p + 5 + len > end) break;
        inf_l1(s, sizeof s, f->d + p + 5, len);
        if (strcasestr(s, topic)) page = (int)inf16(f, p + 3);
        p += 5 + len + 4u * f->d[p + 2];
    }
    return page;
}
/* a book (VIEW's name for one: NAME, NAME.INF): as named; beside the one linking to it; in BOOKSHELF's, HELP's directories; here */
static inf_t *book_open(const char *name, const char *near)
{
    char dirs[8192], nm[300];
    const char *bs = getenv("BOOKSHELF"), *hp = getenv("HELP"), *sl = near ? strrchr(near, '/') : 0;
    snprintf(nm, sizeof nm, "%s", name);
    char *b = strrchr(nm, '\\');
    if (b) memmove(nm, b + 1, strlen(b));                    /* (C:\BOOKS\X.INF: X.INF, looked for) */
    snprintf(dirs, sizeof dirs, "%.*s;%s;%s;.", sl ? (int)(sl - near) : 1, sl ? near : ".", bs ? bs : "", hp ? hp : "");
    inf_t *f = inf_load(name);
    for (char *d = dirs, *e; d && !f; d = e) {
        e = strchr(d, ';');
        if (e) *e++ = 0;
        if (!*d) continue;
        char hd[PATH_MAX];
        if ((isalpha((unsigned char)d[0]) && d[1] == ':') || d[0] == '\\') { if (os2_to_host(d, hd, sizeof hd, 0)) continue; d = hd; }
        for (int k = 0; k < 6 && !f; k++) {                  /* as it is, lower case, upper; each with .inf after it */
            char n2[300], p[PATH_MAX * 2];
            snprintf(n2, sizeof n2, "%s%s", nm, k & 1 ? ".inf" : "");
            for (char *c = n2; *c; c++) *c = (char)(k / 2 == 1 ? tolower((unsigned char)*c) : k / 2 == 2 ? toupper((unsigned char)*c) : *c);
            snprintf(p, sizeof p, "%s/%s", d, n2);
            f = inf_load(p);
        }
    }
    return f;
}

/* ── the help window: a panel laid out as words (each where it goes), links ── */
typedef struct { int x, y, w, style, mono, link; uint32_t rgb; char *s; } hitem_t;   /* y: its line's top, from the panel's top */
typedef struct { int kind, target; char *s; } hlink_t;      /* kind 0: a panel (its TOC entry); 1: inform (a resource number, the program's); 2: launch (s) */
typedef struct helpi helpi_t;
typedef struct {
    inf_t *f;
    int page;                                                /* a TOC entry; -1 Contents, -2 Index */
    int hist[64], nhist;
    hitem_t *it; int nit, cap;
    hlink_t *lk; int nlk, lkcap;
    int laid_w, doc_h, top, sel, lineh;
    char title[128];
    uint32_t frame, client, sb, inform;                      /* (inform: the program's window, for HM_INFORM) */
    int owns, quit;                                          /* its book's its own (to free); the last of these gone: WM_QUIT */
    helpi_t *hi;
} hview_t;
typedef struct { hview_t *v; int x, y, lm, rm, width, cw, space, spacing, mono, fmono, style, link, align, line0, run, hide, any; uint32_t rgb; int save_spacing; } hlay_t;
enum { HV_BAR = 40 };
static pmfont_t *hv_font(int style, int mono)
{
    int px = pm_deffont->ok ? pm_deffont->px : 15;
    int bold = style == 2 || style == 3 || style == 6 || style == 7;
    return font_get((mono ? 2 : 0) | (bold ? 1 : 0), px);
}
static void hv_clear(hview_t *v)
{
    for (int i = 0; i < v->nit; i++) free(v->it[i].s);
    for (int i = 0; i < v->nlk; i++) free(v->lk[i].s);
    v->nit = 0; v->nlk = 0;
}
static int hv_link(hview_t *v, int kind, int target)
{
    if (v->nlk == v->lkcap) { v->lkcap = v->lkcap ? v->lkcap * 2 : 16; v->lk = realloc(v->lk, (size_t)v->lkcap * sizeof *v->lk); }
    v->lk[v->nlk].kind = kind; v->lk[v->nlk].target = target; v->lk[v->nlk].s = 0;
    return v->nlk++;
}
static void lay_nl(hlay_t *L)
{
    hview_t *v = L->v;
    if (L->align & 6 && v->nit > L->line0) {                 /* :lines align=right, center */
        int right = v->it[v->nit - 1].x + v->it[v->nit - 1].w, room = L->width - L->rm - right;
        int dx = L->align & 4 ? room / 2 : room;
        if (dx > 0) for (int i = L->line0; i < v->nit; i++) v->it[i].x += dx;
    }
    L->y += v->lineh;
    L->x = L->lm;
    L->line0 = v->nit;
    L->run = -1;
}
static void lay_word(hlay_t *L, const char *s)
{
    hview_t *v = L->v;
    if (L->hide || !*s) return;
    pmfont_t *save = pmf;
    pmf = hv_font(L->style, L->mono || L->fmono);
    int n = (int)strlen(s), w = text_w(s, n);
    pmf = save;
    if (!L->mono && L->x + w > L->width - L->rm && L->x > L->lm) {
        int from = L->run > L->line0 ? L->run : v->nit;      /* (words with no space between: to the next line together) */
        int dx = from < v->nit ? v->it[from].x - 0 : 0;
        lay_nl(L);
        dx -= L->lm;
        for (int i = from; i < v->nit; i++) { v->it[i].x -= dx; v->it[i].y = L->y; }
        if (from < v->nit) { L->x = v->it[v->nit - 1].x + v->it[v->nit - 1].w; L->line0 = from; L->run = from; }
    }
    if (L->run < 0) L->run = v->nit;
    if (v->nit == v->cap) { v->cap = v->cap ? v->cap * 2 : 256; v->it = realloc(v->it, (size_t)v->cap * sizeof *v->it); }
    hitem_t *it = &v->it[v->nit++];
    it->x = L->x; it->y = L->y; it->w = w; it->style = L->style; it->mono = L->mono || L->fmono; it->link = L->link; it->rgb = L->rgb;
    it->s = strdup(s);
    L->x += w;
    L->any = 1;
    if (L->spacing && !L->mono) { L->x += L->space; L->run = -1; }
}
static void lay_text(hlay_t *L, const char *s)              /* (words of ours: Contents, Index) */
{
    char b[300];
    snprintf(b, sizeof b, "%s", s);
    for (char *t = strtok(b, " "); t; t = strtok(0, " ")) lay_word(L, t);
}
static void lay_para(hlay_t *L)
{
    if (L->hide) return;
    if (L->any) { if (L->x > L->lm) lay_nl(L); L->y += L->v->lineh; L->line0 = L->v->nit; }
    L->x = L->lm;
    L->run = -1;
}
static void lay_margin(hlay_t *L, int chars) { L->lm = 8 + chars * L->cw; if (L->x < L->lm) L->x = L->lm; }
static const uint32_t ipf_rgb[17] = { 0x000000, 0x0000C0, 0xC00000, 0xC000C0, 0x008000, 0x008080, 0xA0A000, 0x606060, 0x804000,
                                      0x404040, 0x000080, 0x800000, 0x800080, 0x004000, 0x004040, 0x000000, 0xA0A0A0 };
static void lay_esc(hlay_t *L, int code, const uint8_t *a, uint32_t n)
{
    inf_t *f = L->v->f;
    uint32_t a16 = n >= 2 ? (uint32_t)a[0] | (uint32_t)a[1] << 8 : 0;
    switch (code) {
    case 0x02: if (n >= 1) lay_margin(L, a[0]); break;       /* the left margin, in characters */
    case 0x03: if (n >= 1) L->rm = 8 + a[0] * L->cw; break;  /* the right */
    case 0x1c: L->lm = L->x; break;                          /* the left margin: here */
    case 0x04: if (n >= 1) L->style = a[0] & 7; break;       /* :hp1. italic, 2 bold, 3 both, 5-7 underlined */
    case 0x0d: if (n >= 1) L->rgb = a[0] < 4 ? ipf_rgb[a[0]] : 0; break;   /* :hp4. blue, 8 red, 9 pink */
    case 0x13: if (n >= 1) L->rgb = a[0] < 17 ? ipf_rgb[a[0]] : 0; break;  /* :color fc= */
    case 0x05: case 0x06: case 0x07:                         /* a link: to a panel, a footnote */
        if (n >= 2) L->link = hv_link(L->v, 0, (int)a16);
        break;
    case 0x16: if (n >= 2) L->link = hv_link(L->v, 1, (int)a16); break;   /* reftype=inform: the program's */
    case 0x10:                                               /* reftype=launch: (a byte, then "object data") */
        if (n >= 2) {
            char s[256];
            inf_l1(s, sizeof s, a + 1, n - 1);
            L->link = hv_link(L->v, 2, 0);
            L->v->lk[L->link].s = strdup(s);
        }
        break;
    case 0x08: L->link = -1; break;
    case 0x0b: lay_para(L); L->save_spacing = L->spacing; L->spacing = 0; L->mono = 1; break;   /* :xmp., :cgraphic.: a block, as it is */
    case 0x0c: if (L->mono) { L->spacing = L->save_spacing; L->mono = 0; } if (L->x > L->lm) lay_nl(L); break;
    case 0x1a: if (n >= 1) L->align = a[0]; break;           /* :lines. (1 left, 2 right, 4 centred) */
    case 0x1b: L->align = 0; break;
    case 0x19:                                               /* :font.: the file's fonts; a fixed one's, mono */
        L->fmono = 0;
        if (n >= 1 && a[0] && a[0] < f->nfont) {
            char face[34];
            inf_l1(face, sizeof face, f->d + f->fontstart + 39u * a[0], strnlen((char *)f->d + f->fontstart + 39u * a[0], 33));
            L->fmono = strcasestr(face, "courier") || strcasestr(face, "mono") || strcasestr(face, "vio");
        }
        break;
    case 0x17: L->hide = 1; break;                           /* :hide. (shown with a key we don't have) */
    case 0x18: L->hide = 0; break;
    default:
        if (trace) fprintf(stderr, "[help] escape %02x (%u bytes)\n", code, n);
        break;
    case 0x0e: {                                             /* :artwork. */
        uint32_t c = L->rgb;
        L->rgb = 0x808080; lay_word(L, "[picture]"); L->rgb = c;
        break;
    }
    }
}
static void lay_slot(hlay_t *L, uint32_t slot)
{
    inf_t *f = L->v->f;
    if (slot >= f->nslots) return;
    uint32_t o = inf32(f, f->slotsstart + 4ull * slot);
    if (!INF_FITS(f, o, 8)) return;
    uint32_t lmo = inf32(f, o + 1), lmn = f->d[o + 5], nt = inf16(f, o + 6);
    if (!INF_FITS(f, lmo, 2ull * lmn) || !INF_FITS(f, o + 8ull, nt)) return;
    const uint8_t *t = f->d + o + 8;
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t b = t[i];
        if (b < lmn) { uint32_t w = inf16(f, lmo + 2ull * b); if (w < f->ndict) lay_word(L, f->word[w]); continue; }
        switch (b) {
        case 0xfa: lay_para(L); break;                       /* a paragraph */
        case 0xfc: L->spacing = !L->spacing; break;          /* words with spaces after them, or not */
        case 0xfd: if (!L->hide) lay_nl(L); break;           /* a new line */
        case 0xfe: if (!L->hide) { L->x += L->space; L->run = -1; } break;   /* a space */
        case 0xff: {                                         /* an escape: its length, its code, what it says */
            if (i + 2 >= nt) return;
            uint32_t len = t[i + 1];
            if (len < 2 || i + 1 + len > nt) return;
            lay_esc(L, t[i + 2], t + i + 3, len - 2);
            i += len;
            break;
        }
        }
    }
}
static void hv_layout(hview_t *v, int width)
{
    inf_t *f = v->f;
    hv_clear(v);
    pmfont_t *save = pmf;
    pmf = hv_font(0, 0);
    hlay_t L = { 0 };
    L.v = v; L.width = width; L.cw = text_w("0", 1); L.space = text_w(" ", 1);
    v->lineh = pm_lineh + 2;
    pmf = save;
    L.rm = 8; L.y = 8; L.spacing = 1; L.link = -1; L.run = -1;
    lay_margin(&L, 1);
    L.x = L.lm;
    inftoc_t t;
    if (v->page >= 0 && inf_toc(f, (uint32_t)v->page, &t))
        for (int k = 0; k < t.nslot; k++) lay_slot(&L, inf16(f, t.slots + 2ull * k));
    else if (v->page == -1) {                                /* Contents: the panels with headings, by level */
        for (uint32_t i = 0; i < f->ntoc; i++) {
            if (!inf_toc(f, i, &t) || !t.level || (t.flags & 0x40) || !t.title[0]) continue;
            lay_margin(&L, 1 + (t.level - 1) * 3); L.x = L.lm;
            L.link = hv_link(v, 0, (int)i);
            lay_text(&L, t.title);
            L.link = -1;
            lay_nl(&L);
        }
    } else if (v->page == -2) {                              /* Index: :i1. and :i2. entries, as the file has them (sorted) */
        uint32_t p = f->indexstart, end = f->indexstart + f->indexlen;
        for (uint32_t k = 0; k < f->nindex && p + 5 <= end; k++) {
            uint32_t len = f->d[p], fl = f->d[p + 1], nsyn = f->d[p + 2], toc = inf16(f, p + 3);
            if (p + 5 + len > end) break;
            char s[300];
            inf_l1(s, sizeof s, f->d + p + 5, len);
            lay_margin(&L, fl & 2 ? 4 : 1); L.x = L.lm;
            L.link = hv_link(v, 0, (int)toc);
            lay_text(&L, s);
            L.link = -1;
            lay_nl(&L);
            p += 5 + len + 4 * nsyn;
        }
        if (!f->nindex) lay_text(&L, "This file has no index.");
    }
    if (L.x > L.lm) lay_nl(&L);
    v->doc_h = L.y + 8;
    v->laid_w = width;
}
static hview_t *hv_of(pmwin_t *w) { return w && w->ctl ? w->ctl : 0; }
static void hv_scrollbar(hview_t *v, pmwin_t *c)
{
    pmwin_t *s = win(v->sb);
    if (!s) return;
    int vis = (c->cy - HV_BAR) / v->lineh, tot = (v->doc_h + v->lineh - 1) / v->lineh, hi = tot > vis ? tot - vis : 0;
    if (v->top > hi) v->top = hi;
    if (v->top < 0) v->top = 0;
    pm_send(s, 0x01a0, (uint32_t)v->top, (uint32_t)hi << 16);                  /* SBM_SETSCROLLBAR */
    pm_send(s, 0x01a6, (uint32_t)(vis & 0xFFFF) | (uint32_t)(tot & 0xFFFF) << 16, 0);   /* SBM_SETTHUMBSIZE */
}
static void hv_titles(hview_t *v)
{
    pmwin_t *fr = win(v->frame);
    if (!fr) return;
    inftoc_t t;
    const char *pt = v->page == -1 ? "Contents" : v->page == -2 ? "Index" : !inf_toc(v->f, (uint32_t)v->page, &t) ? "" : t.level ? t.title : "Footnote";
    if (v->title[0] && pt[0]) snprintf(fr->text, sizeof fr->text, "%s - %s", v->title, pt);
    else snprintf(fr->text, sizeof fr->text, "%s", pt[0] ? pt : v->title);
    if (pmb && pmb->title) pmb->title(fr);
}
static void hv_go(hview_t *v, int page, int remember)
{
    if (remember && v->page != page) {
        if (v->nhist == 64) { memmove(v->hist, v->hist + 1, sizeof v->hist - sizeof *v->hist); v->nhist--; }
        v->hist[v->nhist++] = v->page;
    }
    v->page = page; v->top = 0; v->sel = -1; v->laid_w = 0;
    hv_titles(v);
    pmwin_t *c = win(v->client);
    if (c) invalidate(c);
    if (trace) fprintf(stderr, "[help] panel %d\n", page);
}
static hview_t *hv_open(inf_t *f, const char *title, int page);
static void hv_follow(hview_t *v, int l)
{
    if (l < 0 || l >= v->nlk) return;
    hlink_t *k = &v->lk[l];
    if (k->kind == 0) hv_go(v, k->target, 1);
    else if (k->kind == 1) { if (win(v->inform)) post(win(v->inform), v->inform, 0x0234, (uint32_t)k->target, 0); }   /* HM_INFORM */
    else if (k->s) {                                         /* "view.exe BOOK topic": that book, in a window of its own */
        char s[256], *p = s;
        snprintf(s, sizeof s, "%s", k->s);
        char *prog = strtok_r(p, " ", &p), *book = prog ? strtok_r(0, " ", &p) : 0;
        const char *pb = prog ? (strrchr(prog, '\\') ? strrchr(prog, '\\') + 1 : prog) : "";
        while (p && *p == ' ') p++;
        if (!book || (strcasecmp(pb, "view.exe") && strcasecmp(pb, "view"))) { if (trace) fprintf(stderr, "[help] launch \"%s\": not done\n", k->s); return; }
        char *plus = strchr(book, '+');
        if (plus) *plus = 0;                                 /* (books joined: the first) */
        inf_t *b = book_open(book, v->f->path);
        if (!b) { char t[400]; snprintf(t, sizeof t, "The book %s can't be found.", book); pm_msgbox(t, "Help", 0x0040 | 0x4000); return; }
        int page = p && *p ? inf_find(b, p) : 0;
        hview_t *n = hv_open(b, 0, page >= 0 ? page : 0);
        if (!n) { inf_free(b); return; }
        n->owns = 1; n->quit = v->quit; n->inform = v->inform;
    }
}
static void hv_back(hview_t *v) { if (v->nhist) hv_go(v, v->hist[--v->nhist], 0); }
static uint32_t hv_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    hview_t *v = hv_of(w);
    if (!v) return 0;
    int vis = (w->cy - HV_BAR) / (v->lineh ? v->lineh : 16);
    switch (msg) {
    case WM_PAINT: {
        w->invalid = 0;
        if (v->laid_w != w->cx) { hv_layout(v, w->cx); hv_scrollbar(v, w); }
        fill(w, 0, HV_BAR, w->cx, w->cy, C_LIGHT);
        pmfont_t *save = pmf;
        int y0 = v->top * v->lineh;
        for (int i = 0; i < v->nit; i++) {
            hitem_t *it = &v->it[i];
            int top = it->y - y0;
            if (top + v->lineh <= 0 || top >= w->cy - HV_BAR) continue;
            pmf = hv_font(it->style, it->mono);
            int base = w->cy - top - pm_ascent - 1;
            uint32_t rgb = it->link >= 0 ? 0x0000C0 : it->rgb;
            if (it->link >= 0 && it->link == v->sel) {
                int r = i + 1 < v->nit && v->it[i + 1].link == it->link && v->it[i + 1].y == it->y ? v->it[i + 1].x : it->x + it->w;
                fill(w, it->x - 1, w->cy - top - v->lineh, r + 1, w->cy - top, C_HILITE);
                rgb = C_LIGHT;
            }
            text_at(w, it->x, base, it->s, (int)strlen(it->s), rgb);
            int ux = i + 1 < v->nit && v->it[i + 1].y == it->y && ((it->link >= 0 && v->it[i + 1].link == it->link) ||
                     (it->link < 0 && (it->style & 4) && (v->it[i + 1].style & 4))) ? v->it[i + 1].x : it->x + it->w;   /* (across the space, to the next word of it) */
            if (it->link >= 0 || (it->style & 4)) line(w, it->x, base - 2, ux, base - 2, rgb);
        }
        pmf = save;
        fill(w, 0, 0, w->cx, HV_BAR, C_FACE);
        line(w, 0, HV_BAR - 1, w->cx, HV_BAR - 1, C_DARK);
        present(w);
        return 0;
    }
    case WM_SIZE: v->laid_w = 0; invalidate(w); return 0;
    case 0x0031: {                                           /* WM_VSCROLL */
        int cmd = (int)(mp2 >> 16), pos = (int16_t)(mp2 & 0xFFFF), o = v->top;
        if (cmd == 1) v->top--;
        else if (cmd == 2) v->top++;
        else if (cmd == 3) v->top -= vis > 1 ? vis - 1 : 1;
        else if (cmd == 4) v->top += vis > 1 ? vis - 1 : 1;
        else if (cmd == 5 || cmd == 6) v->top = pos;
        hv_scrollbar(v, w);
        if (v->top != o) invalidate(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        set_focus(w->hwnd);
        int x = (int16_t)(mp1 & 0xFFFF), y = w->cy - (int16_t)(mp1 >> 16) + v->top * v->lineh;
        for (int i = 0; i < v->nit; i++) {
            hitem_t *it = &v->it[i];
            if (it->link >= 0 && x >= it->x && x < it->x + it->w + 4 && y >= it->y && y < it->y + v->lineh) { hv_follow(v, it->link); return 1; }
        }
        return 1;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int ch = (int)(mp2 & 0xFFFF), vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0, o = v->top;
        if (vk == 0x16) v->top--;
        else if (vk == 0x18) v->top++;
        else if (vk == 0x11) v->top -= vis > 1 ? vis - 1 : 1;
        else if (vk == 0x12) v->top += vis > 1 ? vis - 1 : 1;
        else if (vk == 0x14 && (mp1 & KC_CTRL)) v->top = 0;
        else if (vk == 0x13 && (mp1 & KC_CTRL)) v->top = 1 << 14;
        else if (vk == 0x06 || ch == 9 || vk == 0x07) {      /* Tab, Shift+Tab (backtab): the next link, the one before */
            if (!v->nlk) return 1;
            int back = vk == 0x07 || (mp1 & KC_SHIFT);
            v->sel = v->sel < 0 ? (back ? v->nlk - 1 : 0) : (v->sel + (back ? v->nlk - 1 : 1)) % v->nlk;
            for (int i = 0; i < v->nit; i++)
                if (v->it[i].link == v->sel) {
                    int line = v->it[i].y / v->lineh;
                    if (line < v->top || line >= v->top + vis) v->top = line > 1 ? line - 1 : 0;
                    break;
                }
            hv_scrollbar(v, w);
            invalidate(w);
            return 1;
        } else if (ch == '\r' || vk == 0x08 || vk == 0x1e) { hv_follow(v, v->sel); return 1; }
        else if (vk == 0x05 || ch == 8) { hv_back(v); return 1; }   /* Backspace: the panel before */
        else if (vk == 0x0f || ch == 27) {                   /* Esc: the panel before; on the first, away */
            if (v->nhist) hv_back(v);
            else post(w, w->hwnd, WM_CLOSE, 0, 0);
            return 1;
        } else return 1;
        hv_scrollbar(v, w);
        if (v->top != o) invalidate(w);
        return 1;
    }
    case WM_COMMAND:
        switch (mp1 & 0xFFFF) {
        case 1: hv_back(v); break;                           /* Previous */
        case 2: hv_go(v, -1, 1); break;                      /* Contents */
        case 3: hv_go(v, -2, 1); break;                      /* Index */
        }
        set_focus(w->hwnd);
        return 0;
    case WM_CLOSE: {
        pmwin_t *fr = win(v->frame);
        int quit = v->quit;
        if (fr) win_destroy(fr);
        for (int i = 0; quit && i < MAXWIN; i++)             /* (VIEW's last window: the end) */
            if (pmwins[i].used && pmwins[i].iproc == hv_proc && pmwins[i].ctl && ((hview_t *)pmwins[i].ctl)->quit) quit = 0;
        if (quit) post(0, 0, WM_QUIT, 0, 0);
        return 0;
    }
    case WM_DESTROY:
        hv_clear(v);
        free(v->it); free(v->lk);
        if (v->owns) inf_free(v->f);
        if (v->hi) *(hview_t **)v->hi = 0;                   /* (helpi_t's first: its window) */
        return 0;
    }
    return 0;
}
static hview_t *hv_open(inf_t *f, const char *title, int page)
{
    pmwin_t *fr = win_new(0, "#1", 0, 0);
    if (!fr) return 0;
    fr->top = 1;
    fr->iproc = frame_proc;
    frame_pixels(fr, 600, 440);
    pmwin_t *sb = win_new(fr, "#8", 1, 0x8006), *c = win_new(fr, "#helpview", 0, FID_CLIENT);
    if (!sb || !c) { win_destroy(fr); return 0; }
    sb->owner = fr; sb->visible = 1;
    c->iproc = hv_proc; c->owner = fr; c->visible = 1;
    fr->client = c;
    frame_layout(fr);
    hview_t *v = calloc(1, sizeof *v);
    v->f = f; v->page = page; v->sel = -1; v->lineh = 18;
    v->frame = fr->hwnd; v->client = c->hwnd; v->sb = sb->hwnd;
    snprintf(v->title, sizeof v->title, "%s", title && *title ? title : f->title);
    c->ctl = v;
    static const char *const names[] = { "Previous", "Contents", "Index" };
    for (int k = 0; k < 3; k++) {
        pmwin_t *b = win_new(c, "#3", WS_VISIBLE_, (uint32_t)k + 1);
        if (!b) continue;
        b->x = 8 + k * 104; b->y = 6; b->cx = 96; b->cy = 28;
        b->owner = c; b->visible = 1;
        snprintf(b->text, sizeof b->text, "%s", names[k]);
        pm_send(b, WM_CREATE, 0, 0);
    }
    hv_titles(v);
    win_show(fr, 1);
    set_focus(c->hwnd);
    return v;
}

/* ── the help manager: an instance (a window of its own, HM_ messages to
 * it), its library, its help table: for a frame or dialog (by its id), the
 * panel of each window in it (by the focus's id), the extended help panel ── */
typedef struct { uint16_t app, ext; int nsub; uint16_t sub[128][2]; } helptab_t;
struct helpi {
    hview_t *view;                                           /* (first: hv_proc clears it) */
    uint32_t hwnd;
    char lib[260], title[128];
    inf_t *f;
    int tried;
    helptab_t *tab; int ntab;
    uint32_t active, app;                                    /* HM_SET_ACTIVE_WINDOW's; the window help is for now */
};
static helpi_t helpis[8];
static helpi_t *helpi_of(uint32_t hwnd) { for (int i = 0; i < 8; i++) if (helpis[i].hwnd && helpis[i].hwnd == hwnd) return &helpis[i]; return 0; }
static uint32_t help_assoc[64][2];                           /* (window, help instance) */
static void help_subtab(helptab_t *t, uint32_t a, uint32_t end)   /* HELPSUBTABLE: the words an item, then items (window, panel, ...) up to a 0 */
{
    uint32_t size = g16(a);
    if (size < 2) size = 2;
    for (a += 2; t->nsub < 128 && a + 2 * size <= end && g16(a); a += 2 * size) {
        t->sub[t->nsub][0] = g16(a); t->sub[t->nsub][1] = g16(a + 2); t->nsub++;
    }
}
/* HELPTABLE: (window id, subtable, extended help panel) up to a 0; a subtable MAKELONG(id, 0xFFFF) is a resource's */
static void help_table(helpi_t *h, uint32_t a, uint32_t end, uint32_t hmod)
{
    free(h->tab); h->tab = 0; h->ntab = 0;
    for (; a + 8 <= end && g16(a) && h->ntab < 64; a += 8) {
        h->tab = realloc(h->tab, (size_t)(h->ntab + 1) * sizeof *h->tab);
        helptab_t *t = &h->tab[h->ntab++];
        memset(t, 0, sizeof *t);
        t->app = g16(a); t->ext = g16(a + 6);
        uint32_t s = g32(a + 2), sa, sn;
        if ((s >> 16) == 0xFFFF) { if (find_resource(hmod, 19, s & 0xFFFF, &sa, &sn)) help_subtab(t, sa, sa + sn); }   /* RT_HELPSUBTABLE */
        else if (s) help_subtab(t, s, s + 2 + 128 * 8);
    }
    if (trace) fprintf(stderr, "[help] table: %d windows\n", h->ntab);
}
static void help_table_res(helpi_t *h, uint32_t hmod, uint32_t id)
{
    uint32_t a, n;
    if (find_resource(hmod, 18, id, &a, &n)) help_table(h, a, a + n, hmod);   /* RT_HELPTABLE */
}
static void help_table_set(helpi_t *h, uint32_t p, uint32_t hmod)
{
    if ((p >> 16) == 0xFFFF) help_table_res(h, hmod, p & 0xFFFF);   /* PMHELP: a resource's */
    else if (p) help_table(h, p, p + 64 * 8 + 8, hmod);
}
/* its library: the first of its names (one or more, spaces between) there is - as named, in HELP's directories, the program's, here */
static inf_t *help_inf(helpi_t *h)
{
    if (h->f || h->tried) return h->f;
    h->tried = 1;
    char names[260];
    snprintf(names, sizeof names, "%s", h->lib);
    for (char *n = strtok(names, " "); n && !h->f; n = strtok(0, " ")) {
        char host[PATH_MAX];
        if ((strchr(n, '\\') || strchr(n, '/') || strchr(n, ':')) && !os2_to_host(n, host, sizeof host, 0)) { h->f = inf_load(host); continue; }
        char dirs[4096];
        const char *hp = getenv("HELP");
        snprintf(dirs, sizeof dirs, "%s;%s;.", hp ? hp : "", exe_dir);
        for (char *d = dirs, *e; d && !h->f; d = e) {
            e = strchr(d, ';');
            if (e) *e++ = 0;
            if (!*d) continue;
            char p[PATH_MAX * 2], hd[PATH_MAX];
            const char *dir = d;
            if ((isalpha((unsigned char)d[0]) && d[1] == ':') || d[0] == '\\') { if (os2_to_host(d, hd, sizeof hd, 0)) continue; dir = hd; }
            snprintf(p, sizeof p, "%s/%s", dir, n);
            if (!os2_to_host(p, host, sizeof host, 0)) h->f = inf_load(host);
            if (!h->f) h->f = inf_load(p);
        }
    }
    if (trace) fprintf(stderr, "[help] library \"%s\": %s\n", h->lib, h->f ? "loaded" : "not there");
    return h->f;
}
static uint32_t help_app_win(helpi_t *h)                     /* the program's window, for HM_ messages to it */
{
    uint32_t a = h->active ? h->active : h->app;
    if (!a) for (int i = 0; i < 64; i++) if (help_assoc[i][1] == h->hwnd) { a = help_assoc[i][0]; break; }
    pmwin_t *w = win(a);
    return w && w->client ? w->client->hwnd : a;
}
static uint32_t help_show(helpi_t *h, int page)              /* a panel (a TOC entry; -1 Contents, -2 Index); 0, else an HMERR_ */
{
    inf_t *f = help_inf(h);
    if (!f) {
        char t[400];
        snprintf(t, sizeof t, "The help file %s can't be found.", h->lib[0] ? h->lib : "(none named)");
        pm_msgbox(t, h->title[0] ? h->title : "Help", 0x0040 | 0x4000);   /* MB_INFORMATION, MB_MOVEABLE */
        return 0x2003;                                       /* HMERR_OPEN_LIB_FILE */
    }
    if (page >= (int)f->ntoc) return 0x2010;              /* HMERR_PANEL_NOT_FOUND */
    if (!h->view || !win(h->view->frame)) {
        h->view = hv_open(f, h->title, page);
        if (!h->view) return 0x2007;                     /* HMERR_NO_MEMORY */
        h->view->hi = h;
        hv_titles(h->view);
    } else {
        hv_go(h->view, page, 1);
        pmwin_t *fr = win(h->view->frame);
        if (fr && !fr->visible) win_show(fr, 1);
        set_focus(h->view->client);
    }
    h->view->inform = help_app_win(h);
    return 0;
}
static uint32_t help_res(helpi_t *h, uint32_t res)           /* a panel by its res= */
{
    inf_t *f = help_inf(h);
    if (!f) return help_show(h, 0);
    int t = inf_res(f, res);
    if (t < 0) {
        if (trace) fprintf(stderr, "[help] no panel %u\n", res);
        pmwin_t *a = win(help_app_win(h));
        if (a) pm_send(a, 0x022e, 0x2010, 0);                /* HM_ERROR: HMERR_PANEL_NOT_FOUND */
        return 0x2010;
    }
    return help_show(h, t);
}
static helptab_t *help_entry(helpi_t *h, uint32_t app)
{
    for (int i = 0; i < h->ntab; i++) if (h->tab[i].app == app) return &h->tab[i];
    return 0;
}
static void help_ext(helpi_t *h, pmwin_t *top)               /* the extended help of the window help is for */
{
    helptab_t *t = top ? help_entry(h, top->id & 0xFFFF) : 0;
    if (t && t->ext) { help_res(h, t->ext); return; }
    pmwin_t *a = win(help_app_win(h));
    if (a) pm_send(a, 0x0232, 0, 0);                         /* HM_EXT_HELP_UNDEFINED */
}
/* F1 (WM_HELP, at the frame or dialog): the focus's panel, else HM_HELPSUBITEM_NOT_FOUND, else the extended help */
static uint32_t api_WinQueryHelpInstance(void);
static helpi_t *help_for(pmwin_t *w)
{
    for (; w; w = w->parent ? w->parent : w->owner)
        for (int i = 0; i < 64; i++) if (help_assoc[i][0] == w->hwnd) return helpi_of(help_assoc[i][1]);
    return 0;
}
static void help_request(pmwin_t *top, uint32_t id)
{
    helpi_t *h = help_for(top);
    if (!h) return;
    pmwin_t *act = win(h->active);
    if (act) top = frame_of(act) ? frame_of(act) : act;
    h->app = top->hwnd;
    helptab_t *t = help_entry(h, top->id & 0xFFFF);
    if (trace) fprintf(stderr, "[help] F1: window %u, item %u: %s\n", top->id & 0xFFFF, id & 0xFFFF, t ? "in the table" : "not in it");
    if (t) for (int i = 0; i < t->nsub; i++) if (t->sub[i][0] == (id & 0xFFFF)) { help_res(h, t->sub[i][1]); return; }
    pmwin_t *a = win(help_app_win(h));
    if (a && pm_send(a, 0x022f, 1, (top->id & 0xFFFF) | (id & 0xFFFF) << 16)) return;   /* HM_HELPSUBITEM_NOT_FOUND (HLPM_WINDOW): the program's */
    help_ext(h, top);
}
static uint32_t help_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    helpi_t *h = helpi_of(w->hwnd);
    if (!h) return 0;
    switch (msg) {
    case 0x0222:                                             /* HM_DISPLAY_HELP: (a panel's res= | its name, HM_RESOURCEID | HM_PANELNAME) */
        if (mp2 == 1) {
            char *n = gs(mp1);
            inf_t *f = help_inf(h);
            int t = f && n ? inf_name(f, n) : -1;
            return t >= 0 ? help_show(h, t) : f ? 0x2010 : help_show(h, 0);
        }
        if (!(mp1 & 0xFFFF)) return help_show(h, -1);        /* (none: help for help - the contents, here) */
        return help_res(h, mp1 & 0xFFFF);
    case 0x0223: help_ext(h, frame_of(win(help_app_win(h)))); return 0;   /* HM_EXT_HELP */
    case 0x022a: return help_show(h, -2);                    /* HM_HELP_INDEX */
    case 0x022b: return help_show(h, -1);                    /* HM_HELP_CONTENTS */
    case 0x022c: {                                           /* HM_KEYS_HELP: the program says which (HM_QUERY_KEYS_HELP) */
        pmwin_t *a = win(help_app_win(h));
        uint32_t r = a ? pm_send(a, 0x0230, 0, 0) : 0;
        return r ? help_res(h, r & 0xFFFF) : 0;
    }
    case 0x0221: if (h->view && win(h->view->frame)) win_destroy(win(h->view->frame)); return 0;   /* HM_DISMISS_WINDOW */
    case 0x0224: h->active = mp1; return 0;                  /* HM_SET_ACTIVE_WINDOW */
    case 0x0225: help_table_res(h, mp2, mp1 & 0xFFFF); return 0;   /* HM_LOAD_HELP_TABLE (id, hmod) */
    case 0x0226: help_table_set(h, mp1, 0); return 0;        /* HM_CREATE_HELP_TABLE */
    case 0x0227: {                                           /* HM_SET_HELP_WINDOW_TITLE */
        char *t = gs(mp1);
        snprintf(h->title, sizeof h->title, "%s", t ? t : "");
        if (h->view) { snprintf(h->view->title, sizeof h->view->title, "%s", h->title); hv_titles(h->view); }
        return 0;
    }
    case 0x022d: {                                           /* HM_SET_HELP_LIBRARY_NAME */
        char *l = gs(mp1);
        snprintf(h->lib, sizeof h->lib, "%s", l ? l : "");
        if (h->view && win(h->view->frame)) win_destroy(win(h->view->frame));
        inf_free(h->f); h->f = 0; h->tried = 0;
        return help_inf(h) ? 0 : 0x2003;
    }
    }
    return 0;
}
static uint32_t api_WinCreateHelpInstance(void)     /* (hab, pHelpInit: cb, rc, tutorial, table, table's module, ..., title at 32, library at 40) */
{
    if (!pm_start()) return 0;
    helpi_t *h = 0;
    for (int i = 0; i < 8 && !h; i++) if (!helpis[i].hwnd) h = &helpis[i];
    pmwin_t *w = h ? win_new(0, "#help", 0, 0) : 0;
    if (!w) return 0;
    w->iproc = help_proc;
    memset(h, 0, sizeof *h);
    h->hwnd = w->hwnd;
    uint32_t hi = A(1);
    if (hi && ok(hi, 44)) {
        char *t = g32(hi + 32) ? gs(g32(hi + 32)) : 0, *l = g32(hi + 40) ? gs(g32(hi + 40)) : 0;
        snprintf(h->title, sizeof h->title, "%s", t ? t : "");
        snprintf(h->lib, sizeof h->lib, "%s", l ? l : "");
        help_table_set(h, g32(hi + 12), g32(hi + 16));
        p32(hi + 4, 0);                                      /* ulReturnCode: none */
    }
    return w->hwnd;
}
static uint32_t api_WinDestroyHelpInstance(void)
{
    pmwin_t *w = win(A(0));
    helpi_t *h = helpi_of(A(0));
    if (!w || !h) return 0;
    for (int i = 0; i < 64; i++) if (help_assoc[i][1] == A(0)) help_assoc[i][0] = help_assoc[i][1] = 0;
    if (h->view && win(h->view->frame)) win_destroy(win(h->view->frame));
    inf_free(h->f);
    free(h->tab);
    memset(h, 0, sizeof *h);
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
static uint32_t api_WinQueryHelpInstance(void) { helpi_t *h = help_for(win(A(0))); return h ? h->hwnd : 0; }
static uint32_t api_WinLoadHelpTable(void)          /* (hwndHelpInstance, idHelpTable, hmod) */
{
    helpi_t *h = helpi_of(A(0));
    if (!h) return 0;
    help_table_res(h, A(2), A(1) & 0xFFFF);
    return 1;
}
static uint32_t api_WinCreateHelpTable(void)        /* (hwndHelpInstance, phtHelpTable) */
{
    helpi_t *h = helpi_of(A(0));
    if (!h) return 0;
    help_table_set(h, A(1), 0);
    return 1;
}
/* F1, as the system's accelerator makes it: WM_HELP to the focus (a control of ours: to the window it's in) */
static int help_key(pmwin_t *f)
{
    pmwin_t *w = win(pm_focus);
    if (!w || frame_of(w) != f) w = f->client ? f->client : f;
    uint32_t id = w->id & 0xFFFF;
    while (w && !w->top && w->iproc && !w->proc) w = w->owner ? w->owner : w->parent;
    if (!w) return 0;
    post(w, w->hwnd, 0x0022, id, 3);                         /* WM_HELP (usCmd, CMDSRC_ACCELERATOR) */
    return 1;
}
/* VIEW: a file on its own; the topic a panel's number, its name, a word of its title, of the index */
static int help_view_main(const char *file, const char *topic)
{
    inf_t *f = book_open(file, 0);
    if (!f) { fprintf(stderr, "os2x86: %s isn't a help file (.INF, .HLP) here\n", file); return 2; }
    int page = 0;
    if (topic && *topic && (page = inf_find(f, topic)) < 0) { fprintf(stderr, "os2x86: no \"%s\" in %s\n", topic, file); page = 0; }
    if (!pm_start()) return 3;
    pmq_t *q = &pmqs[me->tid - 1];
    q->used = 1; q->tid = me->tid;
    hview_t *v = hv_open(f, 0, page);
    if (!v) return 3;
    v->owns = v->quit = 1;
    for (;;) {
        pmmsg_t m;
        pm_wait_msg(q, 0, 0, 0, &m);
        if (m.msg == WM_QUIT) break;
        pmwin_t *t = win(m.hwnd);
        if (t) pm_send(t, m.msg, m.mp1, m.mp2);
    }
    return 0;
}
