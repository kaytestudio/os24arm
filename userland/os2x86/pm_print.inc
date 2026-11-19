/* userland/os2x86/pm_print.inc - printing (included at the end of pm.inc):
 * one printer, "PDF". A queued or direct printer DC (DevOpenDC OD_QUEUED,
 * OD_DIRECT; OD_INFO to ask it things) is a page - A4 at 150 dots an inch,
 * a bitmap the presentation space on it draws into, with the page units
 * at its resolution; DevEscape's STARTDOC, NEWFRAME, ENDDOC (ABORTDOC)
 * make the document a PDF: each page an image, RunLengthDecode (no zlib
 * needed), in $OS2X86_PRINT (else $OS2X86_HOME/print, else
 * ~/.os2x86/print), named after the document. The spooler's queue
 * listings (SplEnumQueue, SplQueryQueue) and DevQueryHardcopyCaps tell
 * of it. */
enum { PR_W = 1240, PR_H = 1754, PR_DPM = 5906 };   /* (A4, 210 x 297 mm, at 150 dpi) */
typedef struct { uint8_t *rgb; } prpage_t;
typedef struct { uint32_t page; int started, dirty, npages; prpage_t *pages; char doc[128]; } prdoc_t;
static prdoc_t prdocs[8];

static pmwin_t *pr_page(pmdc_t *d) { return d && d->printer ? bmp_get(d->hwnd) : 0; }
static void pr_clear(pmwin_t *b) { if (b) for (int i = 0; i < b->pw * b->ph; i++) b->px[i] = 0xFFFFFFFFu; }
static void pr_keep(prdoc_t *p, pmwin_t *b)             /* the page as it is: kept (RGB, top row first) */
{
    p->pages = realloc(p->pages, (size_t)(p->npages + 1) * sizeof *p->pages);
    uint8_t *rgb = malloc((size_t)b->pw * b->ph * 3);
    for (int i = 0; i < b->pw * b->ph; i++) { uint32_t c = b->px[i]; rgb[3 * i] = (uint8_t)(c >> 16); rgb[3 * i + 1] = (uint8_t)(c >> 8); rgb[3 * i + 2] = (uint8_t)c; }
    p->pages[p->npages++].rgb = rgb;
}
static void pr_free(prdoc_t *p) { for (int i = 0; i < p->npages; i++) free(p->pages[i].rgb); free(p->pages); p->pages = 0; p->npages = 0; }
/* PDF's RunLengthDecode: n 0..127 - n+1 bytes as they are; n 129..255 - the next byte 257-n times; 128 the end */
static size_t rle(const uint8_t *s, size_t n, uint8_t *o)
{
    size_t i = 0, k = 0;
    while (i < n) {
        size_t r = 1;
        while (i + r < n && r < 128 && s[i + r] == s[i]) r++;
        if (r >= 3) { o[k++] = (uint8_t)(257 - r); o[k++] = s[i]; i += r; continue; }
        size_t l = 0;                                          /* (as they are, until a run of three starts) */
        while (i + l < n && l < 128 && !(i + l + 2 < n && s[i + l] == s[i + l + 1] && s[i + l] == s[i + l + 2])) l++;
        o[k++] = (uint8_t)(l - 1);
        memcpy(o + k, s + i, l); k += l; i += l;
    }
    o[k++] = 128;
    return k;
}
static int pr_write(prdoc_t *p, char *out, size_t outn)
{
    const char *dir = getenv("OS2X86_PRINT");
    char d[PATH_MAX], path[PATH_MAX + 200], name[128];
    if (dir && *dir) snprintf(d, sizeof d, "%s", dir);
    else if (getenv("OS2X86_HOME") && *getenv("OS2X86_HOME")) snprintf(d, sizeof d, "%s/print", getenv("OS2X86_HOME"));
    else snprintf(d, sizeof d, "%s/.os2x86/print", getenv("HOME") && *getenv("HOME") ? getenv("HOME") : "/tmp");
    mkdir(d, 0755);
    snprintf(name, sizeof name, "%s", p->doc[0] ? p->doc : prog_name);
    for (char *c = name; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_' && *c != '.') *c = '_';
    for (int k = 1; ; k++) {                                 /* (a name of its own: doc.pdf, doc-2.pdf, ...) */
        if (k == 1) snprintf(path, sizeof path, "%s/%s.pdf", d, name); else snprintf(path, sizeof path, "%s/%s-%d.pdf", d, name, k);
        if (access(path, F_OK) != 0 || k > 999) break;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    long *off = calloc((size_t)(3 + 3 * p->npages), sizeof *off);
    int obj = 0;
    #define OBJ() (off[++obj] = ftell(f), fprintf(f, "%d 0 obj\n", obj))
    fprintf(f, "%%PDF-1.4\n%%\xe2\xe3\xcf\xd3\n");
    OBJ(); fprintf(f, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    OBJ(); fprintf(f, "<< /Type /Pages /Count %d /Kids [", p->npages);
    for (int i = 0; i < p->npages; i++) fprintf(f, " %d 0 R", 3 + 3 * i);
    fprintf(f, " ] >>\nendobj\n");
    size_t raw = (size_t)PR_W * PR_H * 3;
    uint8_t *buf = malloc(raw + raw / 128 + 16);
    for (int i = 0; i < p->npages; i++) {                   /* (a page: the page, its image, its contents) */
        int pg = 3 + 3 * i;
        OBJ(); fprintf(f, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595.28 841.89] /Resources << /XObject << /Im0 %d 0 R >> >> /Contents %d 0 R >>\nendobj\n", pg + 1, pg + 2);
        size_t n = rle(p->pages[i].rgb, raw, buf);
        OBJ(); fprintf(f, "<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /RunLengthDecode /Length %zu >>\nstream\n", PR_W, PR_H, n);
        fwrite(buf, 1, n, f);
        fprintf(f, "\nendstream\nendobj\n");
        const char *cs = "q 595.28 0 0 841.89 0 0 cm /Im0 Do Q\n";
        OBJ(); fprintf(f, "<< /Length %zu >>\nstream\n%sendstream\nendobj\n", strlen(cs), cs);
    }
    free(buf);
    long xref = ftell(f);
    fprintf(f, "xref\n0 %d\n0000000000 65535 f \n", obj + 1);
    for (int i = 1; i <= obj; i++) fprintf(f, "%010ld 00000 n \n", off[i]);
    fprintf(f, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", obj + 1, xref);
    #undef OBJ
    free(off);
    int good = fclose(f) == 0;
    if (out) snprintf(out, outn, "%s", path);
    return good;
}
static uint32_t pr_jobs;
/* DevEscape (hdc, lCode, cbIn, pbIn, pcbOut, pbOut): DEV_OK 1, DEVESC_NOTIMPLEMENTED 0, DEVESC_ERROR -1 */
static uint32_t api_DevEscape(void)
{
    pmdc_t *d = dc_get(A(0));
    pmwin_t *b = pr_page(d);
    if (!b) return 0;
    prdoc_t *p = &prdocs[d - pmdcs < 8 ? (int)(d - pmdcs) : 0];
    switch (A(1)) {
    case DEVESC_QUERYESCSUPPORT:                             /* (the code, in pbIn) */
        if (A(3) && ok(A(3), 4)) { uint32_t c = g32(A(3)); return c == 8150 || c == 8151 || c == 8153 || c == 16300 || c == 8160; }
        return 0;
    case DEVESC_STARTDOC: {                                  /* the document's name */
        pr_free(p);
        char *n = A(3) && A(2) ? gp(A(3), A(2)) : 0;
        snprintf(p->doc, sizeof p->doc, "%.*s", n ? (int)strnlen(n, A(2)) : 0, n ? n : "");
        p->started = 1; p->dirty = 0;
        pr_clear(b);
        return 1;
    }
    case DEVESC_NEWFRAME:                                    /* the page done, a new one */
        pr_keep(p, b);
        pr_clear(b);
        return 1;
    case DEVESC_ABORTDOC: pr_free(p); p->started = 0; pr_clear(b); return 1;
    case DEVESC_GETJOBID: if (A(4) && A(5) && ok(A(5), 2)) { p16(A(5), (uint16_t)pr_jobs); p32(A(4), 2); } return 1;
    case DEVESC_ENDDOC: {                                    /* the last page, the PDF; its job id back */
        int blank = 1;
        for (int i = 0; i < b->pw * b->ph && blank; i++) if (b->px[i] != 0xFFFFFFFFu) blank = 0;
        if (!blank || !p->npages) pr_keep(p, b);
        char path[PATH_MAX + 200];
        int good = pr_write(p, path, sizeof path);
        if (good) fprintf(stderr, "%s: printed %d page(s) to %s\n", prog_name, p->npages, path);
        pr_free(p);
        p->started = 0;
        pr_clear(b);
        pr_jobs++;
        if (A(4) && A(5) && ok(A(5), 2)) { p16(A(5), (uint16_t)pr_jobs); p32(A(4), 2); }
        return good ? 1 : 0xFFFFFFFF;
    }
    }
    return 0;
}
/* DevQueryHardcopyCaps (hdc, lStartForm, lForms, phcInfo): 0 forms asked - how many there are; one: A4 */
static uint32_t api_DevQueryHardcopyCaps(void)
{
    pmdc_t *d = dc_get(A(0));
    if (!d || !d->printer) return 0xFFFFFFFF;
    if (!A(2)) return 1;
    uint32_t h = A(3);
    if (!ok(h, 68)) return 0xFFFFFFFF;
    memset(C.mem + h, 0, 68);
    snprintf((char *)C.mem + h, 32, "A4");
    p32(h + 32, 210); p32(h + 36, 297);                      /* (mm) */
    p32(h + 40, 0); p32(h + 44, 0); p32(h + 48, 210); p32(h + 52, 297);
    p32(h + 56, PR_W); p32(h + 60, PR_H); p32(h + 64, HCAPS_CURRENT);
    return 1;
}
static uint32_t api_DevPostDeviceModes(void) { return DEV_OK; }  /* (no job properties to ask) */
static uint32_t api_DevQueryDeviceNames(void)               /* (hab, pszDriver, pldn, aDeviceName, aDeviceDesc, pldt, aDataType) */
{
    if (A(2) && ok(A(2), 4)) { if (g32(A(2)) && A(3) && ok(A(3), 32)) snprintf((char *)C.mem + A(3), 32, "PDF"); if (g32(A(2)) && A(4) && ok(A(4), 64)) snprintf((char *)C.mem + A(4), 64, "PDF file"); p32(A(2), 1); }
    if (A(5) && ok(A(5), 4)) p32(A(5), 0);
    return 1;
}
/* the queue: PRQINFO3 (pszName, uPriority, uStartTime, uUntilTime, fsType, pszSepFile, pszPrProc, pszParms, pszComment, fsStatus,
 * cJobs, pszPrinters, pszDriverName, pDriverData: 44 bytes), its strings after it */
static uint32_t pr_queue_info(uint32_t buf, uint32_t cb, uint32_t *need)
{
    static const char *const s[] = { "PDF", "PMPRINT", "Print to a PDF file", "PDF", "PDF.PDF" };
    uint32_t n = 44;
    for (int i = 0; i < 5; i++) n += (uint32_t)strlen(s[i]) + 1;
    *need = n;
    if (cb < n || !ok(buf, n)) return 0;
    memset(C.mem + buf, 0, 44);
    uint32_t at = buf + 44, ptr[5];
    for (int i = 0; i < 5; i++) { ptr[i] = at; memcpy(C.mem + at, s[i], strlen(s[i]) + 1); at += (uint32_t)strlen(s[i]) + 1; }
    p32(buf, ptr[0]); p16(buf + 4, 5); p32(buf + 16, ptr[1]); p32(buf + 24, ptr[2]); p32(buf + 32, ptr[3]); p32(buf + 36, ptr[4]);
    return 1;
}
static uint32_t api_SplEnumQueue(void)  /* (pszComputer, ulLevel, pBuf, cbBuf, pcReturned, pcTotal, pcbNeeded, pReserved): NO_ERROR 0, ERROR_MORE_DATA 234, NERR_BufTooSmall 2123 */
{
    uint32_t need;
    if (A(5)) p32(A(5), 1);
    if (A(1) != 3 && A(1) != 4 && A(1) != 6) { if (A(4)) p32(A(4), 0); if (A(6)) p32(A(6), 0); return ERROR_INVALID_LEVEL; }
    int got = pr_queue_info(A(2), A(3), &need);
    if (A(6)) p32(A(6), need);
    if (A(4)) p32(A(4), got ? 1 : 0);
    return got ? 0 : 2123;
}
static uint32_t api_SplQueryQueue(void)  /* (pszComputer, pszQueueName, ulLevel, pBuf, cbBuf, pcbNeeded) */
{
    char *n = gs(A(1));
    if (!n || strcasecmp(n, "PDF")) return NERR_QNotFound;
    uint32_t need;
    int got = pr_queue_info(A(3), A(4), &need);
    if (A(5)) p32(A(5), need);
    return got ? 0 : 2123;
}
