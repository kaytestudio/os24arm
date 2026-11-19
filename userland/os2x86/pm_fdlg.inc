/* userland/os2x86/pm_fdlg.inc - the standard file and font dialogs (PMCTLS
 * WinFileDlg, WinFontDlg), included at the end of pm.inc.
 *
 * The file dialog is built from ordinary controls: file name, directory,
 * subdirectories and drives, and the files matching the filter. It runs
 * modally like WinDlgBox. Open needs an existing file; Save As takes any
 * name. The result goes to FILEDLG's szFullFile, lReturn DID_OK or
 * DID_CANCEL. Not done: FDS_CUSTOM templates, a program dialog procedure,
 * FDS_MULTIPLESEL, modeless. */
#include <dirent.h>
#include <fnmatch.h>

enum { FD_W = 440, FD_H = 340, FD_NAME = 258, FD_DIRTXT = 263, FD_DIRS = 264, FD_FILES = 266 };
typedef struct { uint32_t fd; char dir[PATH_MAX], pat[64]; int save; } fdlg_t;   /* dir is an OS/2 path, "C:\DOCS" */
static fdlg_t fdlg;

static void fd_settext(pmwin_t *e, const char *s)     /* set an entry field's text and put the cursor at the end */
{
    snprintf(e->text, sizeof e->text, "%s", s);
    if (e->ctl) ef(e)->cur = ef(e)->an = -1;
    repaint(e);
}
static void fd_add(pmwin_t *lbw, const char *s)
{
    uint32_t g = gscratch((uint32_t)strlen(s) + 1);
    if (!g) return;
    strcpy((char *)C.mem + g, s);
    list_proc(lbw, LM_INSERTITEM, (uint16_t)LIT_END, g);
}
static int fd_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }
/* refill the lists: subdirectories (and drives) and matching files */
static void fd_fill(pmwin_t *dlg)
{
    pmwin_t *dl = win_child(dlg, FD_DIRS), *fl = win_child(dlg, FD_FILES), *dt = win_child(dlg, FD_DIRTXT);
    if (!dl || !fl) return;
    list_proc(dl, LM_DELETEALL, 0, 0); list_proc(fl, LM_DELETEALL, 0, 0);
    char host[PATH_MAX];
    int drv;
    if (dt) {                                                /* too long: cut from the start */
        const char *p = fdlg.dir;
        snprintf(dt->text, sizeof dt->text, "Directory: %s", p);
        while (text_w(dt->text, (int)strlen(dt->text)) > dt->cx - 8 && (p = strchr(p + 1, '\\')))
            snprintf(dt->text, sizeof dt->text, "Directory: %.2s...%s", fdlg.dir, p);
        dt->invalid = 1;
    }
    if (os2_to_host(fdlg.dir, host, sizeof host, &drv)) return;
    if (strlen(fdlg.dir) > 3) fd_add(dl, "..");
    DIR *d = opendir(host);
    char **dirs = 0, **files = 0;
    int nd = 0, nf = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[PATH_MAX];
            struct stat st;
            snprintf(p, sizeof p, "%s/%s", host, e->d_name);
            if (stat(p, &st)) continue;
            if (S_ISDIR(st.st_mode)) { dirs = realloc(dirs, (size_t)(nd + 1) * sizeof *dirs); dirs[nd++] = strdup(e->d_name); }
            else if (!fnmatch(fdlg.pat, e->d_name, FNM_CASEFOLD)) { files = realloc(files, (size_t)(nf + 1) * sizeof *files); files[nf++] = strdup(e->d_name); }
        }
        closedir(d);
    }
    qsort(dirs, (size_t)nd, sizeof *dirs, fd_cmp);
    qsort(files, (size_t)nf, sizeof *files, fd_cmp);
    for (int i = 0; i < nd; i++) { fd_add(dl, dirs[i]); free(dirs[i]); }
    for (int i = 0; i < nf; i++) { fd_add(fl, files[i]); free(files[i]); }
    free(dirs); free(files);
    for (int k = 0; k < 26; k++) if (droot[k][0]) { char s[8]; snprintf(s, sizeof s, "[%c:]", 'A' + k); fd_add(dl, s); }
    dl->invalid = fl->invalid = 1;
}
/* change directory: "..", a name, "[D:]", or a path */
static void fd_chdir(pmwin_t *dlg, const char *s)
{
    char nd[PATH_MAX];
    if (!strcmp(s, "..")) {
        snprintf(nd, sizeof nd, "%s", fdlg.dir);
        char *b = strrchr(nd, '\\');
        if (b && b > nd + 2) *b = 0; else if (b) b[1] = 0;
    } else if (s[0] == '[' && s[2] == ':') {
        int d = (s[1] | 0x20) - 'a';
        const char *cw = d >= 0 && d < 26 && dcwd[d][0] ? dcwd[d] : 0;
        if (cw) host_to_os2(cw, nd, sizeof nd); else snprintf(nd, sizeof nd, "%c:\\", s[1]);
    } else if (s[1] == ':' || s[0] == '\\') {
        if (s[0] == '\\') snprintf(nd, sizeof nd, "%c:%s", fdlg.dir[0], s); else snprintf(nd, sizeof nd, "%s", s);
    } else snprintf(nd, sizeof nd, "%s%s%s", fdlg.dir, fdlg.dir[strlen(fdlg.dir) - 1] == '\\' ? "" : "\\", s);
    size_t l = strlen(nd);
    if (l > 3 && nd[l - 1] == '\\') nd[l - 1] = 0;
    if (l == 2) strcat(nd, "\\");
    char host[PATH_MAX];
    struct stat st;
    int drv;
    if (os2_to_host(nd, host, sizeof host, &drv) || stat(host, &st) || !S_ISDIR(st.st_mode)) return;
    snprintf(fdlg.dir, sizeof fdlg.dir, "%s", nd);
    fd_fill(dlg);
}
/* handle the typed or selected name: a filter, a directory, or the file; 1 when done */
static int fd_accept(pmwin_t *dlg)
{
    pmwin_t *e = win_child(dlg, FD_NAME);
    char name[260];
    snprintf(name, sizeof name, "%s", e ? e->text : "");
    char *s = name;
    while (*s == ' ') s++;
    if (!*s) return 0;
    char full[PATH_MAX];
    if (s[1] == ':' || s[0] == '\\') { if (s[0] == '\\') snprintf(full, sizeof full, "%c:%s", fdlg.dir[0], s); else snprintf(full, sizeof full, "%s", s); }
    else snprintf(full, sizeof full, "%s%s%s", fdlg.dir, fdlg.dir[strlen(fdlg.dir) - 1] == '\\' ? "" : "\\", s);
    char *last = strrchr(full, '\\');
    if (strpbrk(last ? last : full, "*?")) {                  /* a filter: split into directory and pattern */
        if (last) { *last = 0; if (strlen(full) == 2) strcat(full, "\\"); fd_chdir(dlg, full); snprintf(fdlg.pat, sizeof fdlg.pat, "%s", last + 1); }
        fd_fill(dlg);
        return 0;
    }
    char host[PATH_MAX];
    struct stat st;
    int drv;
    if (os2_to_host(full, host, sizeof host, &drv)) { pm_msgbox("The drive or path isn't valid.", "File", MB_WARNING); return 0; }
    if (!stat(host, &st) && S_ISDIR(st.st_mode)) { fd_chdir(dlg, full); if (e) { e->text[0] = 0; e->invalid = 1; } return 0; }
    if (!fdlg.save && stat(host, &st)) {
        char t[400];
        snprintf(t, sizeof t, "%s\nThe file can't be found.", full);
        pm_msgbox(t, "Open", MB_WARNING);
        return 0;
    }
    snprintf((char *)C.mem + fdlg.fd + 52, 260, "%s", full);   /* szFullFile */
    return 1;
}
static uint32_t fdlg_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    switch (msg) {
    case WM_COMMAND:
        if ((mp1 & 0xFFFF) == DID_OK) { if (fd_accept(w)) { w->dlg_done = 1; w->dlg_result = 1; } return 0; }
        w->dlg_done = 1; w->dlg_result = DID_CANCEL;
        return 0;
    case WM_CONTROL: {
        uint32_t id = mp1 & 0xFFFF, code = mp1 >> 16;
        pmwin_t *c = win_child(w, id);
        lb_t *l = c && c->ctl && (id == FD_DIRS || id == FD_FILES) ? c->ctl : 0;
        const char *sel = l && l->sel >= 0 && l->sel < l->n ? l->it[l->sel].text : 0;
        if (id == FD_FILES && sel && code == LN_SELECT) {             /* selecting a file fills in the name */
            pmwin_t *e = win_child(w, FD_NAME);
            if (e) fd_settext(e, sel);
        }
        if (id == FD_FILES && sel && code == LN_ENTER) { pmwin_t *e = win_child(w, FD_NAME); if (e) fd_settext(e, sel); if (fd_accept(w)) { w->dlg_done = 1; w->dlg_result = 1; } }   /* Enter on a file: take it */
        if (id == FD_DIRS && sel && code == LN_ENTER) { char s[300]; snprintf(s, sizeof s, "%s", sel); fd_chdir(w, s); }   /* Enter on a directory: go into it */
        return 0;
    }
    }
    return def_dlg_proc(w, msg, mp1, mp2);
}
static pmwin_t *fd_ctl(pmwin_t *dlg, const char *cls, uint32_t style, uint32_t id, int x, int y, int cx, int cy, const char *text)
{
    pmwin_t *c = win_new(dlg, cls, style | WS_VISIBLE_, id);
    if (!c) return 0;
    c->x = x; c->y = y; c->cx = cx; c->cy = cy;
    c->owner = dlg;
    c->visible = 1;
    snprintf(c->text, sizeof c->text, "%s", text ? text : "");
    pm_send(c, WM_CREATE, 0, 0);
    return c;
}
static uint32_t api_WinFileDlg(void)                /* (hwndParent, hwndOwner, pfild); returns the dialog handle (already gone), 0 on failure */
{
    uint32_t fd = A(2);
    if (!pm_start() || !ok(fd, 328) || g32(fd) < 328) return 0;
    if (g32(fd + 4) & FDS_MODELESS) { p32(fd + 16, FDS_ERR_INVALID_DIALOG); return 0; }
    memset(&fdlg, 0, sizeof fdlg);
    fdlg.fd = fd;
    fdlg.save = (g32(fd + 4) & FDS_SAVEAS_DIALOG) != 0;
    snprintf(fdlg.pat, sizeof fdlg.pat, "*");
    char cw[PATH_MAX];
    host_to_os2(host_cwd(curdrv, cw, sizeof cw), fdlg.dir, sizeof fdlg.dir);
    char init[260];
    snprintf(init, sizeof init, "%s", (char *)C.mem + fd + 52);
    pmwin_t *dlg = win_new(0, "#dialog", 0, DID_FILE_DIALOG);
    if (!dlg) return 0;
    dlg->top = 1; dlg->fixed = 1;
    dlg->iproc = fdlg_proc;
    dlg->owner = win(A(1));
    frame_pixels(dlg, FD_W, FD_H);
    char *title = g32(fd + 20) ? gs(g32(fd + 20)) : 0, *okb = g32(fd + 24) ? gs(g32(fd + 24)) : 0;
    snprintf(dlg->text, sizeof dlg->text, "%s", title && *title ? title : fdlg.save ? "Save As" : "Open");
    fd_ctl(dlg, "#5", SS_TEXT, 257, 12, FD_H - 30, 300, 20, fdlg.save ? "Save as filename:" : "Open filename:");
    pmwin_t *e = fd_ctl(dlg, "#6", ES_MARGIN | WS_TABSTOP_, FD_NAME, 12, FD_H - 56, FD_W - 24, 24, "");
    if (e) entry_proc(e, EM_SETTEXTLIMIT, 255, 0);
    fd_ctl(dlg, "#5", SS_TEXT, FD_DIRTXT, 12, FD_H - 84, FD_W - 24, 20, "");
    fd_ctl(dlg, "#5", SS_TEXT, 263, 12, FD_H - 108, 200, 20, "Directory:");
    fd_ctl(dlg, "#5", SS_TEXT, 265, 228, FD_H - 108, 200, 20, "File:");
    fd_ctl(dlg, "#7", WS_TABSTOP_, FD_DIRS, 12, 52, 200, FD_H - 164, "");
    fd_ctl(dlg, "#7", WS_TABSTOP_, FD_FILES, 228, 52, 200, FD_H - 164, "");
    fd_ctl(dlg, "#3", BS_DEFAULT | WS_TABSTOP_, 1, 12, 12, 96, 30, okb && *okb ? okb : fdlg.save ? "Save" : "OK");
    fd_ctl(dlg, "#3", WS_TABSTOP_, 2, 116, 12, 96, 30, "Cancel");
    /* the program's initial name: a directory, a filter, or a file (its directory first) */
    if (init[0]) {
        char *b = strrchr(init, '\\');
        if (!b && init[1] == ':') b = init + 1;
        if (b) {
            char d[260];
            snprintf(d, sizeof d, "%.*s", (int)(b - init + (b == init + 1 || b == init + 2 ? 1 : 0)), init);
            if (b == init + 1) snprintf(d, sizeof d, "%.2s\\", init);
            if (b == init) snprintf(d, sizeof d, "\\");
            fd_chdir(dlg, d);
        }
        const char *n = b ? b + 1 : init;
        if (strpbrk(n, "*?")) snprintf(fdlg.pat, sizeof fdlg.pat, "%s", n);
        else if (e) snprintf(e->text, sizeof e->text, "%s", n);
    }
    fd_fill(dlg);
    if (pmb->title) pmb->title(dlg);
    win_show(dlg, 1);
    if (e) set_focus(e->hwnd);
    uint32_t hwnd = dlg->hwnd;
    uint32_t r = dlg_process(hwnd);
    if (win(hwnd)) win_destroy(win(hwnd));
    p32(fd + 12, r == DID_OK ? DID_OK : DID_CANCEL);          /* lReturn */
    p32(fd + 16, FDS_SUCCESSFUL);                            /* lSRC */
    p32(fd + 316, r == 1 ? 1 : 0);                           /* ulFQFCount */
    return hwnd;
}
static uint32_t api_WinDefFileDlgProc(void) { pmwin_t *w = win(A(0)); return w ? fdlg_proc(w, A(1), A(2), A(3)) : 0; }
static uint32_t api_WinFreeFileDlgList(void) { return 1; }

/* ── font dialog (WinFontDlg) ──
 * Lists the families GpiQueryFonts knows, the sizes (the program's list or
 * ours), bold, italic, underline, strikeout, and a preview of the program's
 * text. Returns the family, fxPointSize, flType, usWeight and an FATTRS for
 * GpiCreateLogFont (outline font, sized with GpiSetCharBox). There's no
 * italic face, so italic is returned but not previewed. */
enum { FN_W = 460, FN_H = 330, FN_NAMES = 301, FN_SIZES = 305, FN_BOLD = 310, FN_ITALIC = 311, FN_UNDER = 312, FN_STRIKE = 313, FN_PREV = 320 };
static struct { uint32_t fd; char fam[64], prev[128]; int pt, bold, italic, under, strike; } fndlg;
static int fn_mono(const char *fam) { return strcasestr(fam, "courier") || strcasestr(fam, "mono") || strcasestr(fam, "vio"); }
static uint32_t fnprev_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    (void)mp1; (void)mp2;
    if (msg != WM_PAINT) return 0;
    w->invalid = 0;
    fill(w, 0, 0, w->cx, w->cy, C_LIGHT);
    bevel(w, 0, 0, w->cx, w->cy, 0);
    pmfont_t *save = pmf;
    pmf = font_get((fn_mono(fndlg.fam) ? 2 : 0) | (fndlg.bold ? 1 : 0), fndlg.pt * 3 / 2 > 4 ? fndlg.pt * 3 / 2 : 4);
    const char *t = fndlg.prev[0] ? fndlg.prev : "abcdABCD";
    int tw = text_w(t, (int)strlen(t)), x = (w->cx - tw) / 2, y = (w->cy - pm_ascent + pm_descent) / 2;
    if (x < 4) x = 4;
    text_at(w, x, y, t, (int)strlen(t), C_TEXT);
    if (fndlg.under) line(w, x, y - 2, x + tw, y - 2, C_TEXT);
    if (fndlg.strike) line(w, x, y + pm_ascent / 3, x + tw, y + pm_ascent / 3, C_TEXT);
    pmf = save;
    present(w);
    return 0;
}
static void fn_sync(pmwin_t *dlg)                    /* read the controls and redraw the preview */
{
    pmwin_t *n = win_child(dlg, FN_NAMES), *s = win_child(dlg, FN_SIZES), *c;
    lb_t *l;
    if (n && (l = n->ctl) && l->sel >= 0) snprintf(fndlg.fam, sizeof fndlg.fam, "%s", l->it[l->sel].text);
    if (s && (l = s->ctl) && l->sel >= 0) fndlg.pt = atoi(l->it[l->sel].text);
    if ((c = win_child(dlg, FN_BOLD))) fndlg.bold = (int)button_proc(c, BM_QUERYCHECK, 0, 0);
    if ((c = win_child(dlg, FN_ITALIC))) fndlg.italic = (int)button_proc(c, BM_QUERYCHECK, 0, 0);
    if ((c = win_child(dlg, FN_UNDER))) fndlg.under = (int)button_proc(c, BM_QUERYCHECK, 0, 0);
    if ((c = win_child(dlg, FN_STRIKE))) fndlg.strike = (int)button_proc(c, BM_QUERYCHECK, 0, 0);
    if ((c = win_child(dlg, FN_PREV))) repaint(c);
}
static uint32_t fndlg_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    switch (msg) {
    case WM_COMMAND: w->dlg_done = 1; w->dlg_result = (mp1 & 0xFFFF) == 1 ? 1 : 2; return 0;
    case WM_CONTROL:
        fn_sync(w);
        if ((mp1 >> 16) == LN_ENTER && ((mp1 & 0xFFFF) == FN_NAMES || (mp1 & 0xFFFF) == FN_SIZES)) { w->dlg_done = 1; w->dlg_result = 1; }   /* Enter in either list means OK */
        return 0;
    }
    return def_dlg_proc(w, msg, mp1, mp2);
}
static uint32_t api_WinFontDlg(void)                /* (hwndParent, hwndOwner, pfntd) */
{
    uint32_t fd = A(2);
    if (!pm_start() || !ok(fd, 168) || g32(fd) < 168) return 0;
    uint32_t fl = g32(fd + 36);
    if (fl & FNTS_MODELESS) { p32(fd + 76, FNTS_ERR_INVALID_DIALOG); return 0; }
    memset(&fndlg, 0, sizeof fndlg);
    fndlg.fd = fd;
    char *fam = g32(fd + 28) ? gs(g32(fd + 28)) : 0, *prev = g32(fd + 16) ? gs(g32(fd + 16)) : 0, *sizes = g32(fd + 20) ? gs(g32(fd + 20)) : 0;
    snprintf(fndlg.fam, sizeof fndlg.fam, "%s", fam && *fam ? fam : "Helvetica");
    if ((fl & FNTS_INITFROMFATTRS) && C.mem[fd + 96 + 8]) {  /* take the face from the FATTRS */
        snprintf(fndlg.fam, sizeof fndlg.fam, "%.32s", (char *)C.mem + fd + 96 + 8);
        char *b = strcasestr(fndlg.fam, " bold");
        if (b) { *b = 0; fndlg.bold = 1; }
        uint16_t sel = g16(fd + 96 + 2);
        if (sel & 0x20) fndlg.bold = 1;
        if (sel & 1) fndlg.italic = 1;
        if (sel & 2) fndlg.under = 1;
        if (sel & 0x10) fndlg.strike = 1;
    }
    fndlg.pt = (int)((int32_t)g32(fd + 32) >> 16);
    if (fndlg.pt <= 0) fndlg.pt = 10;
    if (g16(fd + 154) >= FWEIGHT_BOLD) fndlg.bold = 1;        /* usWeight */
    if (g32(fd + 44) & FTYPE_ITALIC) fndlg.italic = 1;       /* flType */
    snprintf(fndlg.prev, sizeof fndlg.prev, "%s", prev && *prev ? prev : "abcdABCD");
    pmwin_t *dlg = win_new(0, "#dialog", 0, 300);
    if (!dlg) return 0;
    dlg->top = 1; dlg->fixed = 1;
    dlg->iproc = fndlg_proc;
    dlg->owner = win(A(1));
    frame_pixels(dlg, FN_W, FN_H);
    char *title = g32(fd + 12) ? gs(g32(fd + 12)) : 0;
    snprintf(dlg->text, sizeof dlg->text, "%s", title && *title ? title : "Font");
    fd_ctl(dlg, "#5", SS_TEXT, 300, 12, FN_H - 30, 200, 20, "Name:");
    fd_ctl(dlg, "#5", SS_TEXT, 304, 260, FN_H - 30, 100, 20, "Size:");
    pmwin_t *nl = fd_ctl(dlg, "#7", WS_TABSTOP_, FN_NAMES, 12, 150, 236, FN_H - 186, "");
    pmwin_t *sl = fd_ctl(dlg, "#7", WS_TABSTOP_, FN_SIZES, 260, 150, 80, FN_H - 186, "");
    fd_ctl(dlg, "#3", BS_AUTOCHECKBOX | WS_TABSTOP_, FN_BOLD, 356, FN_H - 56, 96, 22, "Bold");
    fd_ctl(dlg, "#3", BS_AUTOCHECKBOX | WS_TABSTOP_, FN_ITALIC, 356, FN_H - 82, 96, 22, "Italic");
    fd_ctl(dlg, "#3", BS_AUTOCHECKBOX | WS_TABSTOP_, FN_UNDER, 356, FN_H - 108, 96, 22, "Underline");
    fd_ctl(dlg, "#3", BS_AUTOCHECKBOX | WS_TABSTOP_, FN_STRIKE, 356, FN_H - 134, 96, 22, "Strikeout");
    pmwin_t *pv = fd_ctl(dlg, "#fnprev", 0, FN_PREV, 12, 56, FN_W - 24, 84, "");
    if (pv) pv->iproc = fnprev_proc;
    fd_ctl(dlg, "#3", BS_DEFAULT | WS_TABSTOP_, 1, 12, 12, 96, 30, "OK");
    fd_ctl(dlg, "#3", WS_TABSTOP_, 2, 116, 12, 96, 30, "Cancel");
    /* families, filtered by FIXEDWIDTHONLY, PROPORTIONALONLY, BITMAPONLY, VECTORONLY */
    int seln = -1, sels = -1, k = 0;
    for (int i = 0; i < NFONTLIST; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (!strcmp(font_list[j].family, font_list[i].family)) dup = 1;
        int mono = font_list[i].ours >= 2;
        if (dup || ((fl & 0x400) && !mono) || ((fl & 0x800) && mono) || ((fl & 0x100) && font_list[i].outline) || ((fl & 0x200) && !font_list[i].outline)) continue;
        if (nl) fd_add(nl, font_list[i].family);
        if (!strcasecmp(font_list[i].family, fndlg.fam)) seln = k;
        k++;
    }
    /* sizes: the program's ("8 10 12") or these */
    char sz[256];
    snprintf(sz, sizeof sz, "%s", sizes && *sizes ? sizes : "6 8 9 10 11 12 14 16 18 20 24 28 36 48 72");
    k = 0;
    for (char *t = strtok(sz, " ,"); t; t = strtok(0, " ,"))
        if (atoi(t) > 0) { if (sl) fd_add(sl, t); if (atoi(t) == fndlg.pt) sels = k; k++; }
    if (nl) { lb_t *l = lb(nl); l->sel = seln >= 0 ? seln : 0; lb_show_sel(nl, l); }
    if (sl) { lb_t *l = lb(sl); l->sel = sels >= 0 ? sels : (l->n > 3 ? 3 : 0); lb_show_sel(sl, l); }
    pmwin_t *c;
    if ((c = win_child(dlg, FN_BOLD))) button_proc(c, BM_SETCHECK, (uint32_t)fndlg.bold, 0);
    if ((c = win_child(dlg, FN_ITALIC))) button_proc(c, BM_SETCHECK, (uint32_t)fndlg.italic, 0);
    if ((c = win_child(dlg, FN_UNDER))) button_proc(c, BM_SETCHECK, (uint32_t)fndlg.under, 0);
    if ((c = win_child(dlg, FN_STRIKE))) button_proc(c, BM_SETCHECK, (uint32_t)fndlg.strike, 0);
    fn_sync(dlg);
    if (pmb->title) pmb->title(dlg);
    win_show(dlg, 1);
    if (nl) set_focus(nl->hwnd);
    uint32_t hwnd = dlg->hwnd;
    uint32_t r = dlg_process(hwnd);
    if (win(hwnd)) win_destroy(win(hwnd));
    p32(fd + 72, r == 1 ? 1 : 2);                             /* lReturn */
    p32(fd + 76, 0);                                         /* lSRC */
    if (r != 1) return hwnd;
    /* the result */
    uint32_t fb = g32(fd + 28), fbl = g16(fd + 164);
    if (fb && fbl && ok(fb, fbl)) snprintf((char *)C.mem + fb, fbl, "%s", fndlg.fam);   /* pszFamilyname */
    p32(fd + 32, (uint32_t)fndlg.pt << 16);                 /* fxPointSize */
    p32(fd + 44, fndlg.italic ? FTYPE_ITALIC : 0);           /* flType */
    p16(fd + 152, (uint16_t)(fndlg.pt * 10));                /* sNominalPointSize */
    p16(fd + 154, fndlg.bold ? FWEIGHT_BOLD : FWEIGHT_NORMAL);   /* usWeight */
    p16(fd + 156, FWIDTH_NORMAL);                            /* usWidth */
    int px = fndlg.pt * 3 / 2;
    p32(fd + 80, (uint32_t)px); p32(fd + 84, (uint32_t)(px / 2)); p32(fd + 88, 0);   /* lEmHeight, lXHeight, lExternalLeading */
    uint32_t fa = fd + 96;                                   /* FATTRS */
    memset(C.mem + fa, 0, 56);
    p16(fa, 56);
    p16(fa + 2, (uint16_t)((fndlg.italic ? 1 : 0) | (fndlg.under ? 2 : 0) | (fndlg.strike ? 0x10 : 0) | (fndlg.bold ? 0x20 : 0)));
    snprintf((char *)C.mem + fa + 8, 32, "%s%s", fndlg.fam, fndlg.bold ? " Bold" : "");
    p16(fa + 54, FATTR_FONTUSE_OUTLINE | FATTR_FONTUSE_TRANSFORMABLE);   /* fsFontUse */
    return hwnd;
}
static uint32_t api_WinDefFontDlgProc(void) { pmwin_t *w = win(A(0)); return w ? fndlg_proc(w, A(1), A(2), A(3)) : 0; }
