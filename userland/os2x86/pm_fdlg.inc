/* userland/os2x86/pm_fdlg.inc - the standard file dialog (PMCTLS:
 * WinFileDlg; included at the end of pm.inc): a dialog of our own built
 * from the usual controls - the file name, the directory, its
 * subdirectories and the drives, the files that match the filter - run
 * modally as WinDlgBox's are. Open: the file must be there; Save as: a
 * name will do. The result in FILEDLG's szFullFile, lReturn DID_OK or
 * DID_CANCEL. Not yet: FDS_CUSTOM templates, a dialog procedure of the
 * program's, several files (FDS_MULTIPLESEL), modeless. */
#include <dirent.h>
#include <fnmatch.h>

enum { FD_W = 440, FD_H = 340, FD_NAME = 258, FD_DIRTXT = 263, FD_DIRS = 264, FD_FILES = 266 };
typedef struct { uint32_t fd; char dir[PATH_MAX], pat[64]; int save; } fdlg_t;   /* (dir: OS/2's, "C:\DOCS") */
static fdlg_t fdlg;

static pmwin_t *fd_item(pmwin_t *dlg, uint32_t id)
{
    for (int i = 0; i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].parent == dlg && pmwins[i].id == id) return &pmwins[i];
    return 0;
}
static void fd_settext(pmwin_t *e, const char *s)     /* (the entry field: the text, the cursor after it) */
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
    list_proc(lbw, 0x0161, 0xFFFF, g);                       /* LM_INSERTITEM, LIT_END */
}
static int fd_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }
/* the lists again: the directory's subdirectories (and the drives), its files that match */
static void fd_fill(pmwin_t *dlg)
{
    pmwin_t *dl = fd_item(dlg, FD_DIRS), *fl = fd_item(dlg, FD_FILES), *dt = fd_item(dlg, FD_DIRTXT);
    if (!dl || !fl) return;
    list_proc(dl, 0x016e, 0, 0); list_proc(fl, 0x016e, 0, 0);   /* LM_DELETEALL */
    char host[PATH_MAX];
    int drv;
    if (dt) {                                                /* (too long: its start left out) */
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
/* into a directory: "..", a name, "[D:]", or a path */
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
/* the name typed (or chosen): a filter, a directory, or the file - 1 when it's done */
static int fd_accept(pmwin_t *dlg)
{
    pmwin_t *e = fd_item(dlg, FD_NAME);
    char name[260];
    snprintf(name, sizeof name, "%s", e ? e->text : "");
    char *s = name;
    while (*s == ' ') s++;
    if (!*s) return 0;
    char full[PATH_MAX];
    if (s[1] == ':' || s[0] == '\\') { if (s[0] == '\\') snprintf(full, sizeof full, "%c:%s", fdlg.dir[0], s); else snprintf(full, sizeof full, "%s", s); }
    else snprintf(full, sizeof full, "%s%s%s", fdlg.dir, fdlg.dir[strlen(fdlg.dir) - 1] == '\\' ? "" : "\\", s);
    char *last = strrchr(full, '\\');
    if (strpbrk(last ? last : full, "*?")) {                  /* a filter: its directory, its pattern */
        if (last) { *last = 0; if (strlen(full) == 2) strcat(full, "\\"); fd_chdir(dlg, full); snprintf(fdlg.pat, sizeof fdlg.pat, "%s", last + 1); }
        fd_fill(dlg);
        return 0;
    }
    char host[PATH_MAX];
    struct stat st;
    int drv;
    if (os2_to_host(full, host, sizeof host, &drv)) { pm_msgbox("The drive or path isn't valid.", "File", 0x0020); return 0; }
    if (!stat(host, &st) && S_ISDIR(st.st_mode)) { fd_chdir(dlg, full); if (e) { e->text[0] = 0; e->invalid = 1; } return 0; }
    if (!fdlg.save && stat(host, &st)) {
        char t[400];
        snprintf(t, sizeof t, "%s\nThe file can't be found.", full);
        pm_msgbox(t, "Open", 0x0020);                        /* MB_ICONEXCLAMATION */
        return 0;
    }
    snprintf((char *)C.mem + fdlg.fd + 52, 260, "%s", full);   /* szFullFile */
    return 1;
}
static uint32_t fdlg_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    switch (msg) {
    case WM_COMMAND:
        if ((mp1 & 0xFFFF) == 1) { if (fd_accept(w)) { w->dlg_done = 1; w->dlg_result = 1; } return 0; }   /* DID_OK */
        w->dlg_done = 1; w->dlg_result = 2;                    /* DID_CANCEL */
        return 0;
    case WM_CONTROL: {
        uint32_t id = mp1 & 0xFFFF, code = mp1 >> 16;
        pmwin_t *c = fd_item(w, id);
        lb_t *l = c && c->ctl && (id == FD_DIRS || id == FD_FILES) ? c->ctl : 0;
        const char *sel = l && l->sel >= 0 && l->sel < l->n ? l->it[l->sel].text : 0;
        if (id == FD_FILES && sel && code == 1) {             /* LN_SELECT: the name, typed for you */
            pmwin_t *e = fd_item(w, FD_NAME);
            if (e) fd_settext(e, sel);
        }
        if (id == FD_FILES && sel && code == 5) { pmwin_t *e = fd_item(w, FD_NAME); if (e) fd_settext(e, sel); if (fd_accept(w)) { w->dlg_done = 1; w->dlg_result = 1; } }   /* LN_ENTER: that one */
        if (id == FD_DIRS && sel && code == 5) { char s[300]; snprintf(s, sizeof s, "%s", sel); fd_chdir(w, s); }   /* LN_ENTER: into it */
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
static uint32_t api_WinFileDlg(void)                /* (hwndParent, hwndOwner, pfild): the dialog's handle (gone by now), 0 when it can't */
{
    uint32_t fd = A(2);
    if (!pm_start() || !ok(fd, 328) || g32(fd) < 328) return 0;
    if (g32(fd + 4) & 0x40) { p32(fd + 16, 3); return 0; }   /* FDS_MODELESS: FDS_ERR_INVALID_DIALOG */
    memset(&fdlg, 0, sizeof fdlg);
    fdlg.fd = fd;
    fdlg.save = (g32(fd + 4) & 0x200) != 0;                  /* FDS_SAVEAS_DIALOG */
    snprintf(fdlg.pat, sizeof fdlg.pat, "*");
    char cw[PATH_MAX];
    host_to_os2(host_cwd(curdrv, cw, sizeof cw), fdlg.dir, sizeof fdlg.dir);
    char init[260];
    snprintf(init, sizeof init, "%s", (char *)C.mem + fd + 52);
    pmwin_t *dlg = win_new(0, "#dialog", 0, 256);           /* DID_FILE_DIALOG */
    if (!dlg) return 0;
    dlg->top = 1; dlg->fixed = 1;
    dlg->iproc = fdlg_proc;
    dlg->owner = win(A(1));
    frame_pixels(dlg, FD_W, FD_H);
    char *title = g32(fd + 20) ? gs(g32(fd + 20)) : 0, *okb = g32(fd + 24) ? gs(g32(fd + 24)) : 0;
    snprintf(dlg->text, sizeof dlg->text, "%s", title && *title ? title : fdlg.save ? "Save As" : "Open");
    fd_ctl(dlg, "#5", 1, 257, 12, FD_H - 30, 300, 20, fdlg.save ? "Save as filename:" : "Open filename:");
    pmwin_t *e = fd_ctl(dlg, "#6", 8 | WS_TABSTOP_, FD_NAME, 12, FD_H - 56, FD_W - 24, 24, "");
    if (e) entry_proc(e, 0x0143, 255, 0);
    fd_ctl(dlg, "#5", 1, FD_DIRTXT, 12, FD_H - 84, FD_W - 24, 20, "");
    fd_ctl(dlg, "#5", 1, 263, 12, FD_H - 108, 200, 20, "Directory:");
    fd_ctl(dlg, "#5", 1, 265, 228, FD_H - 108, 200, 20, "File:");
    fd_ctl(dlg, "#7", WS_TABSTOP_, FD_DIRS, 12, 52, 200, FD_H - 164, "");
    fd_ctl(dlg, "#7", WS_TABSTOP_, FD_FILES, 228, 52, 200, FD_H - 164, "");
    fd_ctl(dlg, "#3", 0x400 | WS_TABSTOP_, 1, 12, 12, 96, 30, okb && *okb ? okb : fdlg.save ? "Save" : "OK");   /* BS_DEFAULT */
    fd_ctl(dlg, "#3", WS_TABSTOP_, 2, 116, 12, 96, 30, "Cancel");
    /* the program's starting name: a directory, a filter, a file (its directory first) */
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
    p32(fd + 12, r == 1 ? 1 : 2);                             /* lReturn: DID_OK, DID_CANCEL */
    p32(fd + 16, 0);                                         /* lSRC: FDS_SUCCESSFUL */
    p32(fd + 316, r == 1 ? 1 : 0);                           /* ulFQFCount */
    return hwnd;
}
static uint32_t api_WinDefFileDlgProc(void) { pmwin_t *w = win(A(0)); return w ? fdlg_proc(w, A(1), A(2), A(3)) : 0; }
static uint32_t api_WinFreeFileDlgList(void) { return 1; }
