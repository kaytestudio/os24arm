/* userland/os2x86/pm_drag.inc - Presentation Manager's direct manipulation
 * (PMDRAG; included at the end of pm.inc): string handles, DRAGINFO and
 * its DRAGITEMs in the program's memory, the type and rendering checks,
 * and DrgDrag - the drag itself, a loop of its own until the button that
 * began it comes up: DM_DRAGOVER to the window under the pointer (its
 * answer - DOR_DROP, DOR_NODROP, ... - shows in the pointer), DM_DRAGLEAVE
 * when it leaves, DM_DROP where it ends. Between the windows of one
 * program (and its frames); not yet with other programs. Button 2 down
 * and moved gives the window WM_BEGINDRAG (pm_mouse), where programs
 * call DrgDrag; containers turn it into CN_INITDRAG. */

/* string handles (HSTR): ours, 1 up */
static char *drg_str[512];
static uint32_t api_DrgAddStrHandle(void)           /* (psz) */
{
    char *s = gs(A(0));
    if (!s) return 0;
    for (int i = 0; i < 512; i++) if (drg_str[i] && !strcmp(drg_str[i], s)) return (uint32_t)i + 1;   /* (the same string: the same handle) */
    for (int i = 0; i < 512; i++) if (!drg_str[i]) { drg_str[i] = strdup(s); return (uint32_t)i + 1; }
    return 0;
}
static const char *drg_name(uint32_t h) { return h >= 1 && h <= 512 ? drg_str[h - 1] : 0; }
static uint32_t api_DrgDeleteStrHandle(void) { return drg_name(A(0)) ? 1 : 0; }   /* (kept: shared by every drag) */
static uint32_t api_DrgQueryStrName(void)            /* (hstr, cbBuffer, pBuffer): the length copied */
{
    const char *s = drg_name(A(0));
    if (!A(1) || !ok(A(2), A(1))) return 0;
    snprintf((char *)C.mem + A(2), A(1), "%s", s ? s : "");
    return (uint32_t)strlen((char *)C.mem + A(2));
}
static uint32_t api_DrgQueryStrNameLen(void) { const char *s = drg_name(A(0)); return s ? (uint32_t)strlen(s) : 0; }

/* DRAGINFO: cbDraginfo, cbDragitem (16 bits), usOperation, hwndSource, xDrop, yDrop, cditem, then the DRAGITEMs (36 bytes:
 * hwndItem, ulItemID, hstrType, hstrRMF, hstrContainerName, hstrSourceName, hstrTargetName, cxOffset, cyOffset, fsControl,
 * fsSupportedOps) */
enum { DI_SIZE = 20, DITEM = 36 };
static uint32_t api_DrgAllocDraginfo(void)          /* (cditem) */
{
    uint32_t n = A(0) ? A(0) : 1, d = galloc(DI_SIZE + DITEM * n);
    if (!d) return 0;
    p32(d, DI_SIZE + DITEM * n); p16(d + 4, DITEM); p16(d + 6, 0xBFFE);   /* DO_DEFAULT */
    p16(d + 16, (uint16_t)A(0));
    return d;
}
static int drg_ok(uint32_t d) { return d && ok(d, DI_SIZE) && g16(d + 4) >= DITEM; }
static uint32_t drg_item(uint32_t d, uint32_t i) { return drg_ok(d) && i < g16(d + 16) ? d + DI_SIZE + i * g16(d + 4) : 0; }
static uint32_t api_DrgFreeDraginfo(void) { return drg_ok(A(0)); }         /* (the sub-allocator keeps it) */
static uint32_t api_DrgAccessDraginfo(void) { return drg_ok(A(0)); }
static uint32_t api_DrgPushDraginfo(void) { return drg_ok(A(0)); }
static uint32_t api_DrgDeleteDraginfoStrHandles(void) { return drg_ok(A(0)); }
static uint32_t api_DrgQueryDragitemCount(void) { return drg_ok(A(0)) ? g16(A(0) + 16) : 0; }
static uint32_t api_DrgQueryDragitemPtr(void) { return drg_item(A(0), A(1)); }   /* (pdinfo, i) */
static uint32_t api_DrgSetDragitem(void)            /* (pdinfo, pditem, cbBuffer, iItem) */
{
    uint32_t t = drg_item(A(0), A(3)), n = A(2) < DITEM ? A(2) : DITEM;
    if (!t || !ok(A(1), n)) return 0;
    memmove(C.mem + t, C.mem + A(1), n);
    return 1;
}
static uint32_t api_DrgQueryDragitem(void)          /* (pdinfo, cbBuffer, pditem, iItem) */
{
    uint32_t t = drg_item(A(0), A(3)), n = A(1) < DITEM ? A(1) : DITEM;
    if (!t || !ok(A(2), n)) return 0;
    memmove(C.mem + A(2), C.mem + t, n);
    return n;
}
/* types: "Plain Text,C Code"; rendering mechanisms and formats: "<DRM_OS2FILE,DRF_TEXT>" or "(a,b)x(c,d)" */
static int drg_list_has(const char *list, const char *w)
{
    if (!list || !w) return 0;
    size_t n = strlen(w);
    for (const char *p = list; *p; ) {
        while (*p == ' ' || *p == ',' || *p == '<' || *p == '>' || *p == '(' || *p == ')' || *p == 'x') {
            if (*p == 'x' && p[1] != '(' && p[1] != ' ' && p[1]) break;
            p++;
        }
        if (!strncmp(p, w, n) && (!p[n] || strchr(",>) ", p[n]))) return 1;
        while (*p && !strchr(",>)", *p)) p++;
        if (*p) p++;
    }
    return 0;
}
static uint32_t api_DrgVerifyType(void)             /* (pditem, pszType) */
{
    if (!ok(A(0), DITEM)) return 0;
    return (uint32_t)drg_list_has(drg_name(g32(A(0) + 8)), gs(A(1)));
}
static uint32_t api_DrgVerifyTrueType(void)         /* (pditem, pszType): the first one */
{
    const char *l = ok(A(0), DITEM) ? drg_name(g32(A(0) + 8)) : 0, *t = gs(A(1));
    if (!l || !t) return 0;
    size_t n = strlen(t);
    return !strncmp(l, t, n) && (!l[n] || l[n] == ',');
}
static uint32_t api_DrgVerifyRMF(void)              /* (pditem, pszMech, pszFmt): 0 = any */
{
    const char *l = ok(A(0), DITEM) ? drg_name(g32(A(0) + 12)) : 0;
    if (!l) return 0;
    char *m = A(1) ? gs(A(1)) : 0, *f = A(2) ? gs(A(2)) : 0;
    return (!m || drg_list_has(l, m)) && (!f || drg_list_has(l, f));
}
static uint32_t api_DrgVerifyNativeRMF(void) { return api_DrgVerifyRMF(); }
static uint32_t api_DrgQueryNativeRMF(void)         /* (pditem, cbBuffer, pBuffer) */
{
    const char *l = ok(A(0), DITEM) ? drg_name(g32(A(0) + 12)) : 0;
    if (!A(1) || !ok(A(2), A(1))) return 0;
    snprintf((char *)C.mem + A(2), A(1), "%s", l ? l : "");
    return 1;
}
static uint32_t api_DrgQueryNativeRMFLen(void) { const char *l = ok(A(0), DITEM) ? drg_name(g32(A(0) + 12)) : 0; return l ? (uint32_t)strlen(l) + 1 : 0; }
static uint32_t api_DrgQueryTrueType(void)          /* (pditem, cbBuffer, pBuffer): the first type */
{
    const char *l = ok(A(0), DITEM) ? drg_name(g32(A(0) + 8)) : 0;
    if (!l || !A(1) || !ok(A(2), A(1))) return 0;
    size_t n = strcspn(l, ",");
    snprintf((char *)C.mem + A(2), A(1), "%.*s", (int)n, l);
    return 1;
}
static uint32_t api_DrgQueryTrueTypeLen(void) { const char *l = ok(A(0), DITEM) ? drg_name(g32(A(0) + 8)) : 0; return l ? (uint32_t)strcspn(l, ",") + 1 : 0; }
static uint32_t api_DrgGetPS(void) { pmwin_t *w = win(A(0)); return w ? ps_new(w, 2) : 0; }
static uint32_t api_DrgReleasePS(void) { pmps_t *ps = ps_get(A(0)); if (!ps) return 0; present(ps_win(ps)); ps->used = 0; return 1; }
static uint32_t api_DrgSetDragPointer(void) { return 1; }
static uint32_t api_DrgSetDragImage(void) { return 1; }
static uint32_t api_DrgQueryDragStatus(void);
/* transfers (DM_RENDER and the like) go straight to the window: one program */
static uint32_t api_DrgSendTransferMsg(void) { pmwin_t *w = win(A(0)); return w ? pm_send(w, A(1), A(2), A(3)) : 0; }
static uint32_t api_DrgPostTransferMsg(void)        /* (hwnd, msg, pdxfer, fl, ulReserved, fRetry) */
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    post(w, w->hwnd, A(1), A(2), A(3));
    return 1;
}
static uint32_t api_DrgAllocDragtransfer(void)      /* (cdxfer): DRAGTRANSFERs, 36 bytes each, cb first */
{
    uint32_t n = A(0) ? A(0) : 1, d = galloc(36 * n);
    if (d) for (uint32_t i = 0; i < n; i++) p32(d + 36 * i, 36);
    return d;
}
static uint32_t api_DrgFreeDragtransfer(void) { return 1; }

/* the drag */
static int drg_active;
static uint32_t api_DrgQueryDragStatus(void) { return drg_active ? 1 : 0; }   /* DGS_DRAGINPROGRESS */
static void drg_desktop_xy(pmwin_t *w, uint32_t mp1, int *x, int *y)    /* a mouse message's place, the desktop's */
{
    int ax, ay;
    win_abs(w, &ax, &ay);
    pmwin_t *f = frame_of(w);
    *x = (int16_t)(mp1 & 0xFFFF) + ax + (f ? f->x : 0);
    *y = (int16_t)(mp1 >> 16) + ay + (f ? f->y : 0);
}
static uint32_t api_DrgDrag(void)   /* (hwndSource, pdinfo, pdimg, cdimg, vkTerminate, pRsvd): the target, or 0 */
{
    uint32_t d = A(1), term = A(4) == 0x01 ? 1 : A(4) == 0x03 ? 3 : 2;   /* VK_BUTTON1, VK_BUTTON3; else button 2 (VK_BUTTON2, VK_ENDDRAG) */
    pmq_t *q = myq();
    if (!drg_ok(d) || !q || drg_active) return 0;
    p32(d + 8, A(0));                                        /* hwndSource */
    uint32_t img = A(2) && ok(A(2), 8) ? g32(A(2) + 4) : 0;  /* (DRAGIMAGE's hImage: an icon to drag, when DRG_ICON) */
    uint32_t ptr_drag = A(2) && (g32(A(2) + 20) & 1) && ptr_get(img) ? img : sys_pointer(1), ptr_no = sys_pointer(18);
    uint32_t target = 0, answer = 0, before = pm_pointer;
    drg_active = 1;
    pm_capture = 0;
    pm_set_pointer(ptr_drag);
    for (;;) {
        pmmsg_t m;
        pm_wait_msg(q, 0, 0, 0, &m);
        if (m.msg == WM_QUIT) { post(0, 0, WM_QUIT, 0, 0); answer = 0; break; }
        pmwin_t *t = win(m.hwnd);
        int up = (term == 1 && m.msg == WM_BUTTON1UP) || (term == 2 && m.msg == WM_BUTTON2UP) || (term == 3 && m.msg == 0x0078);
        if (m.msg == WM_CHAR && (m.mp1 & KC_VIRTUALKEY) && (m.mp2 >> 16) == 0x0f && !(m.mp1 & KC_KEYUP)) { answer = 0; break; }   /* Esc: no drop */
        if (m.msg == WM_MOUSEMOVE || up) {
            int x, y;
            if (!t) continue;
            drg_desktop_xy(t, m.mp1, &x, &y);
            p16(d + 12, (uint16_t)x); p16(d + 14, (uint16_t)y);
            int kc = (int)(m.mp2 >> 16);                       /* the keys: Ctrl copies, Shift moves, both link */
            p16(d + 6, (kc & KC_CTRL) && (kc & KC_SHIFT) ? 0x18 : (kc & KC_CTRL) ? 0x10 : (kc & KC_SHIFT) ? 0x20 : 0xBFFE);
            if (t->hwnd != target) {
                if (win(target)) pm_send(win(target), 0x032d, d, 0);   /* DM_DRAGLEAVE */
                target = t->hwnd;
            }
            answer = pm_send(t, 0x032e, d, (uint32_t)(x & 0xFFFF) | (uint32_t)(y & 0xFFFF) << 16);   /* DM_DRAGOVER: (DOR_*, DO_*) */
            pm_set_pointer((answer & 0xFFFF) == 1 ? ptr_drag : ptr_no);
            if (up) break;
            continue;
        }
        if (m.msg >= WM_BUTTON1DOWN && m.msg <= 0x0079) continue;   /* (other presses during it: nothing) */
        if (t) pm_send(t, m.msg, m.mp1, m.mp2);
    }
    pm_set_pointer(win(before) ? before : sys_pointer(1));
    drg_active = 0;
    pmwin_t *t = win(target);
    if (t && (answer & 0xFFFF) == 1) {                       /* DOR_DROP */
        if (answer >> 16) p16(d + 6, (uint16_t)(answer >> 16));   /* (the operation the target said) */
        pm_send(t, 0x032f, d, 0);                            /* DM_DROP */
        return target;
    }
    if (t) pm_send(t, 0x032d, d, 0);                         /* DM_DRAGLEAVE */
    return 0;
}
