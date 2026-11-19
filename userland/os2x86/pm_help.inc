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

enum { HS_ITALIC = 1, HS_BOLD = 2, HS_UNDER = 4 };   /* :hp1.-:hp7.: these together */
/* the help window's look */
enum { HV_W = 600, HV_H = 440, HV_BAR = 40, HV_MARGIN = 8, HV_BTN_W = 96, HV_BTN_H = 28, HV_BTN_GAP = 8, HV_HIST = 64, HV_INDENT = 3 };   /* (indent: characters a level) */
#define HV_LINK    0x0000C0             /* (a link's colour) */
#define HV_PICTURE C_DARK               /* (a [picture]'s) */
enum { HV_PREVIOUS = 1, HV_CONTENTS, HV_INDEX };   /* its buttons */
enum { HV_PG_CONTENTS = -1, HV_PG_INDEX = -2 };       /* pages of ours, not the file's */
enum { LINES_LEFT = 1, LINES_RIGHT = 2, LINES_CENTER = 4 };      /* :lines align= */
enum { HL_PANEL, HL_INFORM, HL_LAUNCH };              /* a link's kind: to a panel, to the program (HM_INFORM), to VIEW */

/* the structures, by their fields' offsets: an .INF's header (and its extended header's fonts), */
enum {
    INF_NTOC = 8, INF_TOCSTART = 18, INF_NRES = 22, INF_RESSTART = 24, INF_NNAME = 28, INF_NAMESTART = 30,
    INF_NINDEX = 34, INF_INDEXSTART = 36, INF_INDEXLEN = 40, INF_NSLOTS = 62, INF_SLOTSSTART = 64,
    INF_DICTLEN = 68, INF_NDICT = 72, INF_DICTSTART = 74, INF_EXTSTART = 91, INF_TITLE = 107, INF_TITLELEN = 48,
    INF_HEADER = 155, INF_FONT = 39, INF_FACELEN = 33,
};
/* a TOC entry (its length, flags, slots; then its window's, its slots' numbers, its title), */
enum { TOC_LEN = 0, TOC_FLAGS = 1, TOC_NSLOT = 2, TOC_HEADER = 3, TOC_LEVEL = 0x0f, TOC_EXTENDED = 0x20, TOC_HIDDEN = 0x40 };
/* an index entry (its text's length, flags, synonyms, panel; the text, 4 bytes a synonym), */
enum { IX_LEN = 0, IX_FLAGS = 1, IX_NSYN = 2, IX_TOC = 3, IX_TEXT = 5, IX_LEVEL2 = 2 };
/* a slot (its local dictionary, its size; its text's size, the text), its text's bytes past the words */
enum { SL_LOCALDICT = 1, SL_NLOCAL = 5, SL_NTEXT = 6, SL_TEXT = 8 };
enum { TX_PARA = 0xfa, TX_SPACING = 0xfc, TX_NEWLINE = 0xfd, TX_SPACE = 0xfe, TX_ESCAPE = 0xff };
/* the escapes (TX_ESCAPE, its length, one of these, what it says) */
enum {
    ESC_LMARGIN = 0x02, ESC_RMARGIN = 0x03, ESC_STYLE = 0x04, ESC_LINK = 0x05, ESC_LINK2 = 0x06, ESC_FOOTNOTE = 0x07,
    ESC_ENDLINK = 0x08, ESC_MONO = 0x0b, ESC_ENDMONO = 0x0c, ESC_HPCOLOR = 0x0d, ESC_ARTWORK = 0x0e, ESC_LAUNCH = 0x10,
    ESC_COLOR = 0x13, ESC_INFORM = 0x16, ESC_HIDE = 0x17, ESC_ENDHIDE = 0x18, ESC_FONT = 0x19, ESC_LINES = 0x1a,
    ESC_ENDLINES = 0x1b, ESC_MARGINHERE = 0x1c,
};
/* the program's HELPINIT, a HELPTABLE's entries (up to one with no window) */
enum { HI_RETURNCODE = 4, HI_HELPTABLE = 12, HI_HELPTABLEMOD = 16, HI_TITLE = 32, HI_LIBRARY = 40, HI_SIZE = 44 };
enum { HT_APPWINDOW = 0, HT_SUBTABLE = 2, HT_EXTPANEL = 6, HT_SIZE = 8 };
enum { HT_SCAN = 64, HST_SCAN = 128 };                 /* (a table in the program's memory: its end not known, this far looked) */

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
    char dir[PATH_MAX];                 /* (where it is: books it links to are looked for there first) */
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
    inf_t *f = n >= INF_HEADER && n < (64L << 20) ? calloc(1, sizeof *f) : 0;
    if (f) { f->d = malloc((size_t)n); f->n = (uint32_t)n; }
    if (!f || !f->d || fread(f->d, 1, (size_t)n, fp) != (size_t)n || memcmp(f->d, "HS", 2)) { fclose(fp); inf_free(f); return 0; }
    fclose(fp);
    if (!realpath(host, f->dir)) snprintf(f->dir, sizeof f->dir, "%s", host);
    char *sl = strrchr(f->dir, '/');
    if (sl) *sl = 0; else snprintf(f->dir, sizeof f->dir, ".");
    f->ntoc = inf16(f, INF_NTOC); f->tocstart = inf32(f, INF_TOCSTART);
    f->nres = inf16(f, INF_NRES); f->resstart = inf32(f, INF_RESSTART);
    f->nname = inf16(f, INF_NNAME); f->namestart = inf32(f, INF_NAMESTART);
    f->nindex = inf16(f, INF_NINDEX); f->indexstart = inf32(f, INF_INDEXSTART); f->indexlen = inf32(f, INF_INDEXLEN);
    f->nslots = inf16(f, INF_NSLOTS); f->slotsstart = inf32(f, INF_SLOTSSTART);
    f->dictlen = inf32(f, INF_DICTLEN); f->ndict = inf16(f, INF_NDICT); f->dictstart = inf32(f, INF_DICTSTART);
    if (!INF_FITS(f, f->tocstart, 4ull * f->ntoc) || !INF_FITS(f, f->resstart, 4ull * f->nres) || !INF_FITS(f, f->namestart, 4ull * f->nname) ||
        !INF_FITS(f, f->indexstart, f->indexlen) || !INF_FITS(f, f->slotsstart, 4ull * f->nslots) || !INF_FITS(f, f->dictstart, f->dictlen)) { inf_free(f); return 0; }
    inf_l1(f->title, sizeof f->title, f->d + INF_TITLE, strnlen((char *)f->d + INF_TITLE, INF_TITLELEN));
    f->word = calloc(f->ndict + 1u, sizeof *f->word);
    uint32_t p = f->dictstart, end = f->dictstart + f->dictlen;
    for (uint32_t i = 0; i < f->ndict; i++) {
        uint32_t l = p < end ? f->d[p] : 0;
        char b[300] = "";
        if (l >= 1 && p + l <= end) { inf_l1(b, sizeof b, f->d + p + 1, l - 1); p += l; }
        else p = end;
        f->word[i] = strdup(b);
    }
    uint32_t ext = inf32(f, INF_EXTSTART);
    if (ext && INF_FITS(f, ext, 6)) {
        f->nfont = inf16(f, ext); f->fontstart = inf32(f, ext + 2);
        if (!INF_FITS(f, f->fontstart, (uint64_t)INF_FONT * f->nfont)) f->nfont = 0;
    }
    return f;
}
/* a panel's entry in the table of contents: its level (0: a footnote's), flags, slots, title */
typedef struct { int level, flags, nslot; uint32_t slots; char title[256]; } inftoc_t;
static int inf_toc(inf_t *f, uint32_t i, inftoc_t *t)
{
    if (i >= f->ntoc) return 0;
    uint32_t o = inf32(f, f->tocstart + 4ull * i);
    if (!INF_FITS(f, o, TOC_HEADER)) return 0;
    uint32_t len = f->d[o + TOC_LEN], end = o + len, q = o + TOC_HEADER;
    if (len < TOC_HEADER || end > f->n) return 0;
    t->flags = f->d[o + TOC_FLAGS]; t->level = t->flags & TOC_LEVEL; t->nslot = f->d[o + TOC_NSLOT];
    if (t->flags & TOC_EXTENDED) {                                   /* (its window's: origin, size, style, group, control area) */
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
/* the index's entries, one by one from *p (f->indexstart first): its text, panel, flags; 0 at the end */
static int inf_index(inf_t *f, uint32_t *p, char *s, size_t sn, int *toc, int *flags)
{
    uint32_t a = *p, end = f->indexstart + f->indexlen;
    if (a + IX_TEXT > end) return 0;
    uint32_t len = f->d[a + IX_LEN];
    if (a + IX_TEXT + len > end) return 0;
    inf_l1(s, sn, f->d + a + IX_TEXT, len);
    *toc = (int)inf16(f, a + IX_TOC); *flags = f->d[a + IX_FLAGS];
    *p = a + IX_TEXT + len + 4u * f->d[a + IX_NSYN];
    return 1;
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
    char s[300];
    uint32_t p = f->indexstart;
    int toc, fl;
    for (uint32_t k = 0; page < 0 && k < f->nindex && inf_index(f, &p, s, sizeof s, &toc, &fl); k++)
        if (strcasestr(s, topic)) page = toc;
    return page;
}
/* a book, a help library: as named (this system's name, OS/2's); else its file name looked for in dir
 * (the book linking to it, the program's), BOOKSHELF's and HELP's directories, here - as it is, in lower
 * case, in upper, each with .INF after it */
static inf_t *book_open(const char *name, const char *dir)
{
    char host[PATH_MAX], dirs[8192], nm[300];
    inf_t *f = inf_load(name);
    if (!f && strpbrk(name, "\\:") && !os2_to_host(name, host, sizeof host, 0)) f = inf_load(host);
    const char *base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;   /* (C:\BOOKS\X.INF: X.INF) */
    snprintf(nm, sizeof nm, "%s", base);
    const char *bs = getenv("BOOKSHELF"), *hp = getenv("HELP");
    snprintf(dirs, sizeof dirs, "%s;%s;%s;.", dir ? dir : "", bs ? bs : "", hp ? hp : "");
    for (char *d = dirs, *e; d && !f; d = e) {
        e = strchr(d, ';');
        if (e) *e++ = 0;
        if (!*d) continue;
        if (((isalpha((unsigned char)d[0]) && d[1] == ':') || d[0] == '\\')) {
            if (os2_to_host(d, host, sizeof host, 0)) continue;
            d = host;
        }
        for (int kase = 0; kase < 3 && !f; kase++)            /* as it is, in lower case, in upper */
            for (int ext = 0; ext < 2 && !f; ext++) {
                char p[PATH_MAX * 2];
                int k = snprintf(p, sizeof p, "%s/", d);
                snprintf(p + k, sizeof p - (size_t)k, "%s%s", nm, ext ? ".inf" : "");
                for (char *c = p + k; *c; c++) *c = (char)(kase == 1 ? tolower((unsigned char)*c) : kase == 2 ? toupper((unsigned char)*c) : *c);
                f = inf_load(p);
            }
    }
    return f;
}

/* ── the help window: a panel laid out as words (each where it goes), links ── */
typedef struct { int x, y, w, style, mono, link; uint32_t rgb; char *s; } hitem_t;   /* y: its line's top, from the panel's top */
typedef struct { int kind, target; char *s; } hlink_t;      /* HL_PANEL: target its TOC entry; HL_INFORM: a resource number; HL_LAUNCH: s */
typedef struct helpi helpi_t;
typedef struct {
    inf_t *f;
    int page;                                                /* a TOC entry; HV_PG_CONTENTS, HV_PG_INDEX */
    int hist[HV_HIST], nhist;
    hitem_t *it; int nit, cap;
    hlink_t *lk; int nlk, lkcap;
    int laid_w, doc_h, top, sel, lineh;
    char title[128];
    uint32_t frame, client, sb, inform;                      /* (inform: the program's window, for HM_INFORM) */
    int owns, quit;                                          /* its book's its own (to free); the last of these gone: WM_QUIT */
    helpi_t *hi;
} hview_t;
typedef struct { uint16_t app, ext; int nsub; uint16_t (*sub)[2]; } helptab_t;   /* sub: (window, panel) */
struct helpi {
    uint32_t hwnd;
    hview_t *view;
    char lib[260], title[128];
    inf_t *f;
    int tried;
    helptab_t *tab; int ntab;
    uint32_t active, app;                                    /* HM_SET_ACTIVE_WINDOW's; the window help is for now */
};
typedef struct {                                             /* a panel being laid out */
    hview_t *v;
    int x, y;                                                /* where the next word goes */
    int lm, rm, width, cw, space;                            /* margins, the width, a character's, a space's */
    int style, mono, fmono, link, hide;                      /* the words': HS_ style, :xmp., the font's, a link, hidden */
    uint32_t rgb;
    int spacing, save_spacing, align;                        /* spaces after words; :lines. */
    int line0, run, any;                                     /* the line's first word, words that stay together; any laid */
} hlay_t;
static pmfont_t *hv_font(int style, int mono)
{
    int px = pm_deffont->ok ? pm_deffont->px : 15;
    return font_get((mono ? 2 : 0) | (style & HS_BOLD ? 1 : 0), px);   /* (italic: as it is) */
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
    if (L->align & (LINES_RIGHT | LINES_CENTER) && v->nit > L->line0) {
        int right = v->it[v->nit - 1].x + v->it[v->nit - 1].w, room = L->width - L->rm - right;
        int dx = L->align & LINES_CENTER ? room / 2 : room;
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
        int dx = (from < v->nit ? v->it[from].x : 0) - L->lm;
        lay_nl(L);
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
static void lay_margin(hlay_t *L, int chars) { L->lm = HV_MARGIN + chars * L->cw; if (L->x < L->lm) L->x = L->lm; }
static const uint32_t ipf_rgb[17] = { 0x000000, 0x0000C0, 0xC00000, 0xC000C0, 0x008000, 0x008080, 0xA0A000, 0x606060, 0x804000,
                                      0x404040, 0x000080, 0x800000, 0x800080, 0x004000, 0x004040, 0x000000, 0xA0A0A0 };
static void lay_esc(hlay_t *L, int code, const uint8_t *a, uint32_t n)
{
    inf_t *f = L->v->f;
    uint32_t a16 = n >= 2 ? (uint32_t)a[0] | (uint32_t)a[1] << 8 : 0;
    switch (code) {
    case ESC_LMARGIN: if (n >= 1) lay_margin(L, a[0]); break;   /* (in characters) */
    case ESC_RMARGIN: if (n >= 1) L->rm = HV_MARGIN + a[0] * L->cw; break;
    case ESC_MARGINHERE: L->lm = L->x; break;
    case ESC_STYLE: if (n >= 1) L->style = a[0] & (HS_ITALIC | HS_BOLD | HS_UNDER); break;   /* :hp1.-:hp3., :hp5.-:hp7. */
    case ESC_HPCOLOR: if (n >= 1) L->rgb = a[0] < 4 ? ipf_rgb[a[0]] : 0; break;   /* :hp4. blue, 8 red, 9 pink */
    case ESC_COLOR: if (n >= 1) L->rgb = a[0] < sizeof ipf_rgb / sizeof *ipf_rgb ? ipf_rgb[a[0]] : 0; break;  /* :color fc= */
    case ESC_LINK: case ESC_LINK2: case ESC_FOOTNOTE:
        if (n >= 2) L->link = hv_link(L->v, HL_PANEL, (int)a16);
        break;
    case ESC_INFORM: if (n >= 2) L->link = hv_link(L->v, HL_INFORM, (int)a16); break;   /* reftype=inform */
    case ESC_LAUNCH:                                         /* (a byte, then "object data") */
        if (n >= 2) {
            char s[256];
            inf_l1(s, sizeof s, a + 1, n - 1);
            L->link = hv_link(L->v, HL_LAUNCH, 0);
            L->v->lk[L->link].s = strdup(s);
        }
        break;
    case ESC_ENDLINK: L->link = -1; break;
    case ESC_MONO: lay_para(L); L->save_spacing = L->spacing; L->spacing = 0; L->mono = 1; break;   /* :xmp., :cgraphic.: a block, as it is */
    case ESC_ENDMONO: if (L->mono) { L->spacing = L->save_spacing; L->mono = 0; } if (L->x > L->lm) lay_nl(L); break;
    case ESC_LINES: if (n >= 1) L->align = a[0]; break;           /* (LINES_) */
    case ESC_ENDLINES: L->align = 0; break;
    case ESC_FONT:                                           /* the file's fonts; a fixed one's, mono */
        L->fmono = 0;
        if (n >= 1 && a[0] && a[0] < f->nfont) {
            char face[INF_FACELEN + 1];
            const uint8_t *fe = f->d + f->fontstart + INF_FONT * a[0];
            inf_l1(face, sizeof face, fe, strnlen((const char *)fe, INF_FACELEN));
            L->fmono = strcasestr(face, "courier") || strcasestr(face, "mono") || strcasestr(face, "vio");
        }
        break;
    case ESC_HIDE: L->hide = 1; break;                           /* :hide. (shown with a key we don't have) */
    case ESC_ENDHIDE: L->hide = 0; break;
    case ESC_ARTWORK: {
        uint32_t c = L->rgb;
        L->rgb = HV_PICTURE; lay_word(L, "[picture]"); L->rgb = c;
        break;
    }
    default:
        if (trace) fprintf(stderr, "[help] escape %02x (%u bytes)\n", code, n);
        break;
    }
}
static void lay_slot(hlay_t *L, uint32_t slot)
{
    inf_t *f = L->v->f;
    if (slot >= f->nslots) return;
    uint32_t o = inf32(f, f->slotsstart + 4ull * slot);
    if (!INF_FITS(f, o, SL_TEXT)) return;
    uint32_t lmo = inf32(f, o + SL_LOCALDICT), lmn = f->d[o + SL_NLOCAL], nt = inf16(f, o + SL_NTEXT);
    if (!INF_FITS(f, lmo, 2ull * lmn) || !INF_FITS(f, (uint64_t)o + SL_TEXT, nt)) return;
    const uint8_t *t = f->d + o + SL_TEXT;
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t b = t[i];
        if (b < lmn) { uint32_t w = inf16(f, lmo + 2ull * b); if (w < f->ndict) lay_word(L, f->word[w]); continue; }
        switch (b) {
        case TX_PARA: lay_para(L); break;
        case TX_SPACING: L->spacing = !L->spacing; break;    /* words with spaces after them, or not */
        case TX_NEWLINE: if (!L->hide) lay_nl(L); break;
        case TX_SPACE: if (!L->hide) { L->x += L->space; L->run = -1; } break;
        case TX_ESCAPE: {
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
    L.rm = HV_MARGIN; L.y = HV_MARGIN; L.spacing = 1; L.link = -1; L.run = -1;
    lay_margin(&L, 1);
    L.x = L.lm;
    inftoc_t t;
    if (v->page >= 0 && inf_toc(f, (uint32_t)v->page, &t))
        for (int k = 0; k < t.nslot; k++) lay_slot(&L, inf16(f, t.slots + 2ull * k));
    else if (v->page == HV_PG_CONTENTS) {                                /* Contents: the panels with headings, by level */
        for (uint32_t i = 0; i < f->ntoc; i++) {
            if (!inf_toc(f, i, &t) || !t.level || (t.flags & TOC_HIDDEN) || !t.title[0]) continue;
            lay_margin(&L, 1 + (t.level - 1) * HV_INDENT); L.x = L.lm;
            L.link = hv_link(v, HL_PANEL, (int)i);
            lay_text(&L, t.title);
            L.link = -1;
            lay_nl(&L);
        }
    } else if (v->page == HV_PG_INDEX) {                              /* Index: :i1. and :i2. entries, as the file has them (sorted) */
        char s[300];
        uint32_t p = f->indexstart;
        int toc, fl;
        for (uint32_t k = 0; k < f->nindex && inf_index(f, &p, s, sizeof s, &toc, &fl); k++) {
            lay_margin(&L, 1 + (fl & IX_LEVEL2 ? HV_INDENT : 0)); L.x = L.lm;
            L.link = hv_link(v, HL_PANEL, toc);
            lay_text(&L, s);
            L.link = -1;
            lay_nl(&L);
        }
        if (!f->nindex) lay_text(&L, "This file has no index.");
    }
    if (L.x > L.lm) lay_nl(&L);
    v->doc_h = L.y + HV_MARGIN;
    v->laid_w = width;
}
static void hv_scrollbar(hview_t *v, pmwin_t *c)
{
    pmwin_t *s = win(v->sb);
    if (!s) return;
    int vis = (c->cy - HV_BAR) / v->lineh, tot = (v->doc_h + v->lineh - 1) / v->lineh, hi = tot > vis ? tot - vis : 0;
    if (v->top > hi) v->top = hi;
    if (v->top < 0) v->top = 0;
    pm_send(s, SBM_SETSCROLLBAR, (uint32_t)v->top, (uint32_t)hi << 16);
    pm_send(s, SBM_SETTHUMBSIZE, (uint32_t)(vis & 0xFFFF) | (uint32_t)(tot & 0xFFFF) << 16, 0);
}
static void hv_scroll(hview_t *v, pmwin_t *c, int top)       /* to a line (past the end: the last page) */
{
    int o = v->top;
    v->top = top;
    hv_scrollbar(v, c);
    if (v->top != o) invalidate(c);
}
static void hv_titles(hview_t *v)
{
    pmwin_t *fr = win(v->frame);
    if (!fr) return;
    inftoc_t t;
    const char *pt = "";                                     /* the page's */
    if (v->page == HV_PG_CONTENTS) pt = "Contents";
    else if (v->page == HV_PG_INDEX) pt = "Index";
    else if (inf_toc(v->f, (uint32_t)v->page, &t)) pt = t.level ? t.title : "Footnote";
    if (v->title[0] && pt[0]) snprintf(fr->text, sizeof fr->text, "%s - %s", v->title, pt);
    else snprintf(fr->text, sizeof fr->text, "%s", pt[0] ? pt : v->title);
    if (pmb && pmb->title) pmb->title(fr);
}
static void hv_go(hview_t *v, int page, int remember)
{
    if (remember && v->page != page) {
        if (v->nhist == HV_HIST) { memmove(v->hist, v->hist + 1, sizeof v->hist - sizeof *v->hist); v->nhist--; }
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
    if (k->kind == HL_PANEL) hv_go(v, k->target, 1);
    else if (k->kind == HL_INFORM) { if (win(v->inform)) post(win(v->inform), v->inform, HM_INFORM, (uint32_t)k->target, 0); }
    else if (k->s) {                                         /* "view.exe BOOK topic": that book, in a window of its own */
        char s[256], *p = s;
        snprintf(s, sizeof s, "%s", k->s);
        char *prog = strtok_r(p, " ", &p), *book = prog ? strtok_r(0, " ", &p) : 0;
        const char *pb = prog ? (strrchr(prog, '\\') ? strrchr(prog, '\\') + 1 : prog) : "";
        while (p && *p == ' ') p++;
        if (!book || (strcasecmp(pb, "view.exe") && strcasecmp(pb, "view"))) { if (trace) fprintf(stderr, "[help] launch \"%s\": not done\n", k->s); return; }
        char *plus = strchr(book, '+');
        if (plus) *plus = 0;                                 /* (books joined: the first) */
        inf_t *b = book_open(book, v->f->dir);
        if (!b) { char t[400]; snprintf(t, sizeof t, "The book %s can't be found.", book); pm_msgbox(t, "Help", MB_INFORMATION | MB_MOVEABLE); return; }
        int page = p && *p ? inf_find(b, p) : 0;
        hview_t *n = hv_open(b, 0, page >= 0 ? page : 0);
        if (!n) { inf_free(b); return; }
        n->owns = 1; n->quit = v->quit; n->inform = v->inform;
    }
}
static void hv_back(hview_t *v) { if (v->nhist) hv_go(v, v->hist[--v->nhist], 0); }
static uint32_t hv_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    hview_t *v = w->ctl;
    if (!v) return 0;
    int vis = (w->cy - HV_BAR) / v->lineh, page = vis > 1 ? vis - 1 : 1;   /* (a page: but its last line) */
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
            uint32_t rgb = it->link >= 0 ? HV_LINK : it->rgb;
            if (it->link >= 0 && it->link == v->sel) {
                int r = i + 1 < v->nit && v->it[i + 1].link == it->link && v->it[i + 1].y == it->y ? v->it[i + 1].x : it->x + it->w;
                fill(w, it->x - 1, w->cy - top - v->lineh, r + 1, w->cy - top, C_HILITE);
                rgb = C_LIGHT;
            }
            text_at(w, it->x, base, it->s, (int)strlen(it->s), rgb);
            int ux = i + 1 < v->nit && v->it[i + 1].y == it->y && ((it->link >= 0 && v->it[i + 1].link == it->link) ||
                     (it->link < 0 && (it->style & HS_UNDER) && (v->it[i + 1].style & HS_UNDER))) ? v->it[i + 1].x : it->x + it->w;   /* (across the space, to the next word of it) */
            if (it->link >= 0 || (it->style & HS_UNDER)) line(w, it->x, base - 2, ux, base - 2, rgb);
        }
        pmf = save;
        fill(w, 0, 0, w->cx, HV_BAR, C_FACE);
        line(w, 0, HV_BAR - 1, w->cx, HV_BAR - 1, C_DARK);
        present(w);
        return 0;
    }
    case WM_SIZE: v->laid_w = 0; invalidate(w); return 0;
    case WM_VSCROLL: {
        int cmd = (int)(mp2 >> 16), pos = (int16_t)(mp2 & 0xFFFF);
        if (cmd == SB_LINEUP) hv_scroll(v, w, v->top - 1);
        else if (cmd == SB_LINEDOWN) hv_scroll(v, w, v->top + 1);
        else if (cmd == SB_PAGEUP) hv_scroll(v, w, v->top - page);
        else if (cmd == SB_PAGEDOWN) hv_scroll(v, w, v->top + page);
        else if (cmd == SB_SLIDERTRACK || cmd == SB_SLIDERPOSITION) hv_scroll(v, w, pos);
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
        int ch = (int)(mp2 & 0xFFFF), vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0;
        if (vk == VK_UP) hv_scroll(v, w, v->top - 1);
        else if (vk == VK_DOWN) hv_scroll(v, w, v->top + 1);
        else if (vk == VK_PAGEUP) hv_scroll(v, w, v->top - page);
        else if (vk == VK_PAGEDOWN) hv_scroll(v, w, v->top + page);
        else if (vk == VK_HOME && (mp1 & KC_CTRL)) hv_scroll(v, w, 0);
        else if (vk == VK_END && (mp1 & KC_CTRL)) hv_scroll(v, w, INT_MAX);
        else if (vk == VK_TAB || ch == '\t' || vk == VK_BACKTAB) {   /* Tab, Shift+Tab: the next link, the one before */
            if (!v->nlk) return 1;
            int back = vk == VK_BACKTAB || (mp1 & KC_SHIFT);
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
        } else if (ch == '\r' || vk == VK_NEWLINE || vk == VK_ENTER) { hv_follow(v, v->sel); return 1; }
        else if (vk == VK_BACKSPACE || ch == '\b') { hv_back(v); return 1; }   /* the panel before */
        else if (vk == VK_ESC || ch == '\033') {                   /* Esc: the panel before; on the first, away */
            if (v->nhist) hv_back(v);
            else post(w, w->hwnd, WM_CLOSE, 0, 0);
            return 1;
        }
        return 1;
    }
    case WM_COMMAND:
        switch (mp1 & 0xFFFF) {
        case HV_PREVIOUS: hv_back(v); break;
        case HV_CONTENTS: hv_go(v, HV_PG_CONTENTS, 1); break;
        case HV_INDEX: hv_go(v, HV_PG_INDEX, 1); break;
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
        if (v->hi) v->hi->view = 0;
        return 0;
    }
    return 0;
}
static hview_t *hv_open(inf_t *f, const char *title, int page)
{
    pmwin_t *fr = frame_new(0, 0, 0, HV_W, HV_H, FCF_VERTSCROLL, 0), *c = fr ? frame_client(fr, "#helpview", 0) : 0;
    pmwin_t *sb = win_child(fr, FID_VERTSCROLL);
    if (!sb || !c) { if (fr) win_destroy(fr); return 0; }
    c->iproc = hv_proc; c->visible = 1;
    hview_t *v = calloc(1, sizeof *v);
    v->f = f; v->page = page; v->sel = -1; v->lineh = 18;
    v->frame = fr->hwnd; v->client = c->hwnd; v->sb = sb->hwnd;
    snprintf(v->title, sizeof v->title, "%s", title && *title ? title : f->title);
    c->ctl = v;
    static const char *const names[] = { [HV_PREVIOUS] = "Previous", [HV_CONTENTS] = "Contents", [HV_INDEX] = "Index" };
    for (int k = HV_PREVIOUS; k <= HV_INDEX; k++) {
        pmwin_t *b = win_new(c, "#3", WS_VISIBLE_, (uint32_t)k);
        if (!b) continue;
        b->x = HV_MARGIN + (k - HV_PREVIOUS) * (HV_BTN_W + HV_BTN_GAP); b->y = (HV_BAR - HV_BTN_H) / 2; b->cx = HV_BTN_W; b->cy = HV_BTN_H;
        b->owner = c; b->visible = 1;
        snprintf(b->text, sizeof b->text, "%s", names[k]);
        pm_send(b, WM_CREATE, 0, 0);
    }
    hv_titles(v);
    win_show(fr, 1);
    set_focus(c->hwnd);
    return v;
}

/* ── the help manager: an instance (a window of its own, "#help", HM_
 * messages to it), its library, its help table: for a frame or dialog (by
 * its id), the panel of each window in it (by the focus's id), the extended
 * help panel. A window's instance: its helpi (WinAssociateHelpInstance) ── */
static helpi_t *helpi_of(uint32_t hwnd) { pmwin_t *w = win(hwnd); return w && !strcmp(w->cls, "#help") ? w->ctl : 0; }
static uint32_t desktop_helpi;                               /* (HWND_DESKTOP's: every window's, last) */
static void help_subtab(helptab_t *t, uint32_t a, uint32_t end)   /* HELPSUBTABLE: the words an item, then items (window, panel, ...) up to a 0 */
{
    uint32_t size = g16(a);
    if (size < 2) size = 2;
    for (a += 2; a + 2 * size <= end && g16(a); a += 2 * size) {
        t->sub = realloc(t->sub, (size_t)(t->nsub + 1) * sizeof *t->sub);
        t->sub[t->nsub][0] = g16(a); t->sub[t->nsub][1] = g16(a + 2); t->nsub++;
    }
}
/* the help table: p a HELPTABLE in the program's memory, or MAKELONG(id, 0xFFFF) a resource of hmod's (its
 * subtables too); 0, none. Entries (window id, subtable, extended help panel) up to one with no window */
static void help_table(helpi_t *h, uint32_t p, uint32_t hmod)
{
    for (int i = 0; i < h->ntab; i++) free(h->tab[i].sub);
    free(h->tab); h->tab = 0; h->ntab = 0;
    uint32_t a = p, end = p + HT_SCAN * HT_SIZE + HT_SIZE, n;
    if ((p >> 16) == 0xFFFF) { if (!find_resource(hmod, RT_HELPTABLE, p & 0xFFFF, &a, &n)) return; end = a + n; }
    if (!a) return;
    for (; a + HT_SIZE <= end && g16(a + HT_APPWINDOW); a += HT_SIZE) {
        h->tab = realloc(h->tab, (size_t)(h->ntab + 1) * sizeof *h->tab);
        helptab_t *t = &h->tab[h->ntab++];
        memset(t, 0, sizeof *t);
        t->app = g16(a + HT_APPWINDOW); t->ext = g16(a + HT_EXTPANEL);
        uint32_t s = g32(a + HT_SUBTABLE), sa, sn;
        if ((s >> 16) == 0xFFFF) { if (find_resource(hmod, RT_HELPSUBTABLE, s & 0xFFFF, &sa, &sn)) help_subtab(t, sa, sa + sn); }
        else if (s) help_subtab(t, s, s + 2 + HST_SCAN * 8);
    }
    if (trace) fprintf(stderr, "[help] table: %d windows\n", h->ntab);
}
/* its library: the first of its names (one or more, spaces between) there is - looked for as books are, the program's directory first */
static inf_t *help_inf(helpi_t *h)
{
    if (h->f || h->tried) return h->f;
    h->tried = 1;
    char names[260];
    snprintf(names, sizeof names, "%s", h->lib);
    for (char *n = strtok(names, " "); n && !h->f; n = strtok(0, " ")) h->f = book_open(n, exe_dir);
    if (trace) fprintf(stderr, "[help] library \"%s\": %s\n", h->lib, h->f ? "loaded" : "not there");
    return h->f;
}
static uint32_t help_app_win(helpi_t *h)                     /* the program's window, for HM_ messages to it */
{
    uint32_t a = h->active ? h->active : h->app;
    for (int i = 0; !a && i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].helpi == h->hwnd) a = pmwins[i].hwnd;
    pmwin_t *w = win(a);
    return w && w->client ? w->client->hwnd : a;
}
static uint32_t help_show(helpi_t *h, int page)              /* a panel (a TOC entry; HV_PG_CONTENTS, HV_PG_INDEX); 0, else an HMERR_ */
{
    inf_t *f = help_inf(h);
    if (!f) {
        char t[400];
        snprintf(t, sizeof t, "The help file %s can't be found.", h->lib[0] ? h->lib : "(none named)");
        pm_msgbox(t, h->title[0] ? h->title : "Help", MB_INFORMATION | MB_MOVEABLE);
        return HMERR_OPEN_LIB_FILE;
    }
    if (page >= (int)f->ntoc) return HMERR_PANEL_NOT_FOUND;
    if (!h->view || !win(h->view->frame)) {
        h->view = hv_open(f, h->title, page);
        if (!h->view) return HMERR_NO_MEMORY;
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
        if (a) pm_send(a, HM_ERROR, HMERR_PANEL_NOT_FOUND, 0);
        return HMERR_PANEL_NOT_FOUND;
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
    if (a) pm_send(a, HM_EXT_HELP_UNDEFINED, 0, 0);
}
/* F1 (WM_HELP, at the frame or dialog): the focus's panel, else HM_HELPSUBITEM_NOT_FOUND, else the extended help */
static helpi_t *help_for(pmwin_t *w)
{
    for (; w; w = w->parent ? w->parent : w->owner) if (w->helpi) return helpi_of(w->helpi);
    return helpi_of(desktop_helpi);
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
    if (a && pm_send(a, HM_HELPSUBITEM_NOT_FOUND, HLPM_WINDOW, (top->id & 0xFFFF) | (id & 0xFFFF) << 16)) return;   /* (the program's) */
    help_ext(h, top);
}
static uint32_t help_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    helpi_t *h = helpi_of(w->hwnd);
    if (!h) return 0;
    switch (msg) {
    case HM_DISPLAY_HELP:                                    /* (a panel's res= | its name, HM_RESOURCEID | HM_PANELNAME) */
        if (mp2 == HM_PANELNAME) {
            char *n = gs(mp1);
            inf_t *f = help_inf(h);
            int t = f && n ? inf_name(f, n) : -1;
            return t >= 0 ? help_show(h, t) : f ? HMERR_PANEL_NOT_FOUND : help_show(h, 0);
        }
        if (!(mp1 & 0xFFFF)) return help_show(h, HV_PG_CONTENTS);   /* (none: help for help - the contents, here) */
        return help_res(h, mp1 & 0xFFFF);
    case HM_EXT_HELP: help_ext(h, frame_of(win(help_app_win(h)))); return 0;
    case HM_HELP_INDEX: return help_show(h, HV_PG_INDEX);
    case HM_HELP_CONTENTS: return help_show(h, HV_PG_CONTENTS);
    case HM_KEYS_HELP: {                                     /* (the program says which) */
        pmwin_t *a = win(help_app_win(h));
        uint32_t r = a ? pm_send(a, HM_QUERY_KEYS_HELP, 0, 0) : 0;
        return r ? help_res(h, r & 0xFFFF) : 0;
    }
    case HM_DISMISS_WINDOW: if (h->view && win(h->view->frame)) win_destroy(win(h->view->frame)); return 0;
    case HM_SET_ACTIVE_WINDOW: h->active = mp1; return 0;
    case HM_LOAD_HELP_TABLE: help_table(h, 0xFFFF0000u | (mp1 & 0xFFFF), mp2); return 0;   /* (id, hmod) */
    case HM_CREATE_HELP_TABLE: help_table(h, mp1, 0); return 0;
    case HM_SET_HELP_WINDOW_TITLE: {
        char *t = gs(mp1);
        snprintf(h->title, sizeof h->title, "%s", t ? t : "");
        if (h->view) { snprintf(h->view->title, sizeof h->view->title, "%s", h->title); hv_titles(h->view); }
        return 0;
    }
    case HM_SET_HELP_LIBRARY_NAME: {
        char *l = gs(mp1);
        snprintf(h->lib, sizeof h->lib, "%s", l ? l : "");
        if (h->view && win(h->view->frame)) win_destroy(win(h->view->frame));
        inf_free(h->f); h->f = 0; h->tried = 0;
        return help_inf(h) ? 0 : HMERR_OPEN_LIB_FILE;
    }
    case WM_DESTROY:                                         /* (h itself: the window's ctl, freed with it) */
        for (int i = 0; i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].helpi == h->hwnd) pmwins[i].helpi = 0;
        if (desktop_helpi == h->hwnd) desktop_helpi = 0;
        if (h->view && win(h->view->frame)) win_destroy(win(h->view->frame));
        inf_free(h->f);
        help_table(h, 0, 0);
        return 0;
    }
    return 0;
}
static uint32_t api_WinCreateHelpInstance(void)     /* (hab, pHelpInit) */
{
    if (!pm_start()) return 0;
    pmwin_t *w = win_new(0, "#help", 0, 0);
    helpi_t *h = w ? calloc(1, sizeof *h) : 0;
    if (!h) { if (w) win_destroy(w); return 0; }
    w->iproc = help_proc;
    w->ctl = h;
    h->hwnd = w->hwnd;
    uint32_t hi = A(1);
    if (hi && ok(hi, HI_SIZE)) {
        char *t = g32(hi + HI_TITLE) ? gs(g32(hi + HI_TITLE)) : 0, *l = g32(hi + HI_LIBRARY) ? gs(g32(hi + HI_LIBRARY)) : 0;
        snprintf(h->title, sizeof h->title, "%s", t ? t : "");
        snprintf(h->lib, sizeof h->lib, "%s", l ? l : "");
        help_table(h, g32(hi + HI_HELPTABLE), g32(hi + HI_HELPTABLEMOD));
        p32(hi + HI_RETURNCODE, 0);                          /* none */
    }
    return w->hwnd;
}
static uint32_t api_WinDestroyHelpInstance(void)
{
    if (!helpi_of(A(0))) return 0;
    win_destroy(win(A(0)));
    return 1;
}
static uint32_t api_WinAssociateHelpInstance(void)  /* (hwndHelpInstance, hwndApp): 0 - the association goes */
{
    pmwin_t *w = win(A(1));
    if (A(1) == HWND_DESKTOP_) desktop_helpi = A(0);
    else if (w) w->helpi = A(0);
    return w || A(1) == HWND_DESKTOP_;
}
static uint32_t api_WinQueryHelpInstance(void) { helpi_t *h = help_for(win(A(0))); return h ? h->hwnd : 0; }
static uint32_t api_WinLoadHelpTable(void)          /* (hwndHelpInstance, idHelpTable, hmod) */
{
    helpi_t *h = helpi_of(A(0));
    if (h) help_table(h, 0xFFFF0000u | (A(1) & 0xFFFF), A(2));
    return h != 0;
}
static uint32_t api_WinCreateHelpTable(void)        /* (hwndHelpInstance, phtHelpTable) */
{
    helpi_t *h = helpi_of(A(0));
    if (h) help_table(h, A(1), 0);
    return h != 0;
}
/* F1, as the system's accelerator makes it: WM_HELP to the focus (a control of ours: to the window it's in) */
static int help_key(pmwin_t *f)
{
    pmwin_t *w = win(pm_focus);
    if (!w || frame_of(w) != f) w = f->client ? f->client : f;
    uint32_t id = w->id & 0xFFFF;
    while (w && !w->top && w->iproc && !w->proc) w = w->owner ? w->owner : w->parent;
    if (!w) return 0;
    post(w, w->hwnd, WM_HELP, id, CMDSRC_ACCELERATOR);    /* (usCmd, its source) */
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
