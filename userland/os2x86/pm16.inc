/* userland/os2x86/pm16.inc - 16-bit Presentation Manager for OS/2 1.x programs
 * (NE). Included by os2x86.c after ne16.inc.
 *
 * Their PMWIN/PMGPI calls (far, Pascal) are forwarded to the 32-bit ones,
 * mostly through one wrapper driven by an argument format string: L long or
 * handle, W unsigned word, S signed word, P far pointer (made flat; WC_ atoms,
 * 0xFFFF:n, kept). Results go back in DX:AX. QMSG (26 bytes in 16-bit) is
 * converted both ways. A class registered by a 16-bit program keeps its 16-bit
 * window procedure: PM calls it far Pascal on the program's stack and reads
 * the MRESULT from DX:AX; WM_CREATE's pointers are made 16:16 for it.
 * Not done yet: NE resources (menus, dialogs, strings), and controls' pointer
 * messages in 16:16. */

typedef struct { int lib; uint16_t ord; const char *fmt; api_fn fn; } pm16spec_t;
static const pm16spec_t pm16specs[] = {
    { L_PMWIN, 246, "W", api_WinInitialize },
    { L_PMWIN, 247, "L", api_WinTerminate },
    { L_PMWIN, 58, "LS", api_WinCreateMsgQueue },
    { L_PMWIN, 59, "L", api_WinDestroyMsgQueue },
    { L_PMWIN, 140, "LLPPPLWWP", api_WinCreateStdWindow },
    { L_PMWIN, 6, "LPPLSSSSLLWPP", api_WinCreateWindow },
    { L_PMWIN, 7, "L", api_WinDestroyWindow },
    { L_PMWIN, 8, "LLSSSSW", api_WinSetWindowPos },
    { L_PMWIN, 26, "LP", api_WinQueryWindowRect },
    { L_PMWIN, 45, "LLP", api_WinBeginPaint },
    { L_PMWIN, 46, "L", api_WinEndPaint },
    { L_PMWIN, 47, "LPW", api_WinInvalidateRect },
    { L_PMWIN, 69, "LWLL", api_WinPostMsg },
    { L_PMWIN, 61, "LWLL", api_WinSendMsg },
    { L_PMWIN, 173, "LPL", api_WinFillRect },
    { L_PMWIN, 139, "LLPPWW", api_WinMessageBox },
    { L_PMWIN, 84, "LLWW", api_WinStartTimer },
    { L_PMWIN, 85, "LLW", api_WinStopTimer },
    { L_PMWIN, 178, "LWLL", api_WinDefWindowProc },
    { L_PMWIN, 13, "LW", api_WinShowWindow },
    { L_PMWIN, 17, "LSP", api_WinQueryWindowText },
    { L_PMWIN, 16, "LP", api_WinSetWindowText },
    { L_PMWIN, 19, "LW", api_WinWindowFromID },
    { L_PMWIN, 149, "LS", api_WinQuerySysValue },
    { L_PMWIN, 267, "LSPPLLW", api_WinDrawText },
    { L_PMWIN, 43, "L", api_WinGetPS },
    { L_PMWIN, 44, "L", api_WinReleasePS },
    { L_PMWIN, 77, "LL", api_WinSetFocus },
    { L_PMWIN, 78, "LW", api_WinQueryFocus },
    { L_PMWIN, 11, "LW", api_WinEnableWindow },
    { L_PMWIN, 15, "L", api_WinIsWindowVisible },
    { L_PMWIN, 25, "LSW", api_WinQueryWindow },
    { L_PMWIN, 138, "LW", api_WinAlarm },
    { L_PMWIN, 53, "L", api_WinUpdateWindow },
    { L_PMWIN, 31, "LS", api_WinQueryWindowULong },
    { L_PMWIN, 30, "LSL", api_WinSetWindowULong },
    { L_PMWIN, 29, "LS", api_WinQueryWindowUShort },
    { L_PMWIN, 28, "LSW", api_WinSetWindowUShort },
    { L_PMWIN, 269, "LS", api_WinQueryWindowPtr },
    { L_PMWIN, 270, "LSL", api_WinSetWindowPtr },
    { L_PMWIN, 79, "LL", api_WinSetActiveWindow },
    { L_PMWIN, 91, "LSW", api_WinQuerySysPointer },
    { L_PMWIN, 92, "LL", api_WinSetPointer },
    { L_PMWIN, 93, "L", api_WinQueryPointer },
    { L_PMWIN, 177, "LSSLW", api_WinDrawPointer },
    { L_PMWIN, 89, "L", api_WinDestroyPointer },
    { L_PMWIN, 135, "LWSP", api_WinQueryDlgItemText },
    { L_PMWIN, 134, "LWP", api_WinSetDlgItemText },
    { L_PMWIN, 126, "LW", api_WinDismissDlg },
    { L_PMWIN, 179, "LWLL", api_WinDefDlgProc },
    { L_PMWIN, 127, "LWWLL", api_WinSendDlgItemMsg },
    { L_PMWIN, 124, "L", api_WinProcessDlg },
    { L_PMWIN, 211, "LS", api_WinGetKeyState },
    { L_PMWIN, 96, "LP", api_WinQueryPointerPos },
    { L_PMWIN, 94, "LW", api_WinShowPointer },
    { L_PMWIN, 35, "LPWW", api_WinWindowFromPoint },
    { L_PMWIN, 36, "LLPS", api_WinMapWindowPoints },
    { L_PMWIN, 4, "LSP", api_WinQueryClassName },
    { L_PMWIN, 136, "LWW", api_WinLoadMenu },
    { L_PMWIN, 157, "LWWSP", api_WinLoadString },
    { L_PMWIN, 87, "LWW", api_WinLoadPointer },
    { L_PMWIN, 102, "LWW", api_WinLoadAccelTable },
    { L_PMGPI, 108, "LL", api_GpiSetColor },
    { L_PMGPI, 128, "LP", api_GpiMove },
    { L_PMGPI, 131, "LLPLL", api_GpiBox },
    { L_PMGPI, 181, "LPLP", api_GpiCharStringAt },
    { L_PMGPI, 129, "LP", api_GpiLine },
    { L_PMGPI, 110, "LL", api_GpiSetBackColor },
    { L_PMGPI, 112, "LL", api_GpiSetMix },
    { L_PMGPI, 114, "LL", api_GpiSetBackMix },
    { L_PMGPI, 16, "L", api_GpiErase },
    { L_PMGPI, 137, "LLL", api_GpiFullArc },
    { L_PMGPI, 127, "LP", api_GpiQueryCurrentPosition },
    { L_PMGPI, 180, "LLP", api_GpiCharString },
    { L_PMGPI, 159, "LLP", api_GpiQueryFontMetrics },
    { L_PMGPI, 206, "LP", api_GpiSetPel },
    { L_PMGPI, 130, "LLP", api_GpiPolyLine },
    { L_PMGPI, 116, "LL", api_GpiSetLineType },
    { L_PMGPI, 146, "LL", api_GpiSetPattern },
    { L_PMGPI, 109, "L", api_GpiQueryColor },
    { L_PMGPI, 134, "LP", api_GpiSetArcParams },
    { L_PMGPI, 99, "LLLLLP", api_GpiCreateLogColorTable },
    { L_PMGPI, 164, "LLPLP", api_GpiQueryTextBox },
};
static uint32_t to_flat16(uint32_t v) { return !v || (v >> 16) == 0xFFFF ? v : fp16((uint16_t)(v >> 16), (uint16_t)v); }
static uint32_t to_far16(uint32_t flat) { return flat ? (uint32_t)sel16(flat) << 16 | (flat & 0xFFFF) : 0; }
static uint32_t d16_gen(void)
{
    const pm16spec_t *sp = 0;
    for (size_t i = 0; i < sizeof pm16specs / sizeof *pm16specs; i++)
        if (cur16 && pm16specs[i].lib == cur16->lib && pm16specs[i].ord == cur16->ord) { sp = &pm16specs[i]; break; }
    if (!sp) return 0;
    uint32_t a[16], total = 0;
    int n = (int)strlen(sp->fmt);
    for (int i = 0; i < n; i++) total += sp->fmt[i] == 'L' || sp->fmt[i] == 'P' ? 4 : 2;
    uint32_t off = total;                                    /* Pascal pushes the first argument first, so it's highest */
    for (int i = 0; i < n && i < 16; i++) {
        char f = sp->fmt[i];
        off -= f == 'L' || f == 'P' ? 4 : 2;
        a[i] = f == 'L' ? g32(a16 + off) : f == 'P' ? to_flat16(g32(a16 + off)) : f == 'S' ? (uint32_t)(int32_t)(int16_t)g16(a16 + off) : g16(a16 + off);
    }
    return call32(sp->fn, n, a);
}
/* call a 16-bit window procedure: far Pascal (HWND, USHORT msg, MPARAM, MPARAM) on the program's stack, MRESULT in DX:AX */
static uint32_t guest_call16(uint32_t fp, uint32_t hwnd, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    if (msg == WM_CREATE) { mp1 = to_far16(mp1); mp2 = to_far16(mp2); }
    if (msg == WM_INITDLG) mp2 = to_far16(mp2);              /* create parameters */
    uint32_t r[8], eip = C.eip, fl = C.eflags;
    uint16_t sr[6];
    for (int i = 0; i < 6; i++) sr[i] = C.sreg[i];
    memcpy(r, C.r, sizeof r);
    uint32_t esp = c32_depth ? c32_prog_esp : C.r[ESP], sp = esp & 0xFFFF, ss = C.ss_base, back = thunk_addr(T_CBRET);
    sp -= 4; p32(ss + sp, hwnd);
    sp -= 2; p16(ss + sp, (uint16_t)msg);
    sp -= 4; p32(ss + sp, mp1);
    sp -= 4; p32(ss + sp, mp2);
    sp -= 4; p16(ss + sp, (uint16_t)(back & 0xFFFF)); p16(ss + sp + 2, sel16(back));
    C.r[ESP] = (esp & 0xFFFF0000) | sp;
    cpu_load_seg(&C, 1, (uint16_t)(fp >> 16));
    C.eip = fp & 0xFFFF;
    run(1);                                                  /* calls it makes nest below ours (call32 levels) */
    uint32_t ret = (C.r[EAX] & 0xFFFF) | (C.r[EDX] & 0xFFFF) << 16;
    memcpy(C.r, r, sizeof r);
    for (int i = 5; i >= 0; i--) if (i != 4) cpu_load_seg(&C, i, sr[i]);
    C.eip = eip; C.eflags = fl;
    return ret;
}
/* WinRegisterClass (HAB, PSZ, PFNWP, ULONG style, USHORT cb): the class keeps a 16-bit procedure */
static uint32_t d16_WinRegisterClass(void)
{
    uint32_t a[] = { L(14), to_flat16(L(10)), L(6), L(2), W(0) }, r = call32(api_WinRegisterClass, 5, a);
    if (r) {
        char b[64];
        pmclass_t *c = class_find(class_name(a[1], b));
        if (c) c->p16 = 1;
    }
    return r;
}
/* build a 16-bit QMSG (hwnd, USHORT msg, mp1, mp2, time, ptl) from the 32-bit one */
static void q16_from32(uint32_t q16, uint32_t q32)
{
    p32(q16, g32(q32)); p16(q16 + 4, (uint16_t)g32(q32 + 4)); p32(q16 + 6, g32(q32 + 8)); p32(q16 + 10, g32(q32 + 12));
    p32(q16 + 14, g32(q32 + 16)); p32(q16 + 18, g32(q32 + 20)); p32(q16 + 22, g32(q32 + 24));
}
static void q32_from16(uint32_t q32, uint32_t q16)
{
    p32(q32, g32(q16)); p32(q32 + 4, g16(q16 + 4)); p32(q32 + 8, g32(q16 + 6)); p32(q32 + 12, g32(q16 + 10));
    p32(q32 + 16, g32(q16 + 14)); p32(q32 + 20, g32(q16 + 18)); p32(q32 + 24, g32(q16 + 22)); p32(q32 + 28, 0);
}
static uint32_t d16_WinGetMsg(void)                       /* (HAB, PQMSG, HWND, USHORT first, USHORT last) */
{
    uint32_t q = FP(8), t = T(0), a[] = { L(12), t, L(4), W(2), W(0) }, r = call32(api_WinGetMsg, 5, a);
    q16_from32(q, t);
    return r;
}
static uint32_t d16_WinPeekMsg(void)                      /* (HAB, PQMSG, HWND, USHORT first, USHORT last, USHORT fs) */
{
    uint32_t q = FP(10), t = T(0), a[] = { L(14), t, L(6), W(4), W(2), W(0) }, r = call32(api_WinPeekMsg, 6, a);
    if (r) q16_from32(q, t);
    return r;
}
static uint32_t d16_WinDispatchMsg(void)                  /* (HAB, PQMSG) */
{
    uint32_t t = T(0);
    q32_from16(t, FP(0));
    uint32_t a[] = { L(4), t };
    return call32(api_WinDispatchMsg, 2, a);
}

/* dialogs with 16-bit procedures */
static uint32_t d16_dlg(api_fn fn, const char *fmt)
{
    uint32_t a[8], total = 0;
    int n = (int)strlen(fmt);
    for (int i = 0; i < n; i++) total += fmt[i] == 'W' ? 2 : 4;
    uint32_t off = total;
    for (int i = 0; i < n; i++) { off -= fmt[i] == 'W' ? 2 : 4; a[i] = fmt[i] == 'W' ? g16(a16 + off) : fmt[i] == 'P' ? to_flat16(g32(a16 + off)) : g32(a16 + off); }
    int save = pm_dlg16;
    pm_dlg16 = 1;
    uint32_t r = call32(fn, n, a);
    pm_dlg16 = save;
    return r;
}
static uint32_t d16_WinDlgBox(void) { return d16_dlg(api_WinDlgBox, "LLLWWP"); }     /* (parent, owner, proc, hmod, id, create params) */
static uint32_t d16_WinLoadDlg(void) { return d16_dlg(api_WinLoadDlg, "LLLWWP"); }
static uint32_t d16_WinCreateDlg(void) { return d16_dlg(api_WinCreateDlg, "LLLPP"); }  /* (parent, owner, proc, template, create params) */
/* control messages with pointer parameters: make the 16:16 pointers flat (list and menu text and items) */
static void ptr_params16(uint32_t hwnd, uint32_t msg, uint32_t *mp1, uint32_t *mp2)
{
    pmwin_t *w = win(hwnd);
    if (!w || !w->iproc) return;
    switch (msg) {
    case LM_INSERTITEM: case LM_QUERYITEMTEXT: case LM_SETITEMTEXT: case LM_SEARCHSTRING:
    case MM_SETITEMTEXT: case MM_QUERYITEMTEXT: case MM_QUERYITEM: case MM_SETITEM:      /* MENUITEM, both directions */
        *mp2 = to_flat16(*mp2); break;
    case MM_INSERTITEM: *mp1 = to_flat16(*mp1); *mp2 = to_flat16(*mp2); break;   /* MENUITEM and text */
    }
}
static uint32_t d16_WinSendMsg(void)                       /* (hwnd, USHORT msg, mp1, mp2) */
{
    uint32_t h = L(10), m = W(8), p1 = L(4), p2 = L(0);
    ptr_params16(h, m, &p1, &p2);
    uint32_t a[] = { h, m, p1, p2 };
    return call32(api_WinSendMsg, 4, a);
}
static uint32_t d16_WinSendDlgItemMsg(void)                /* (hwndDlg, USHORT id, USHORT msg, mp1, mp2) */
{
    uint32_t d = L(12), id = W(10), m = W(8), p1 = L(4), p2 = L(0);
    pmwin_t *c = dlg_item(d, id);
    ptr_params16(c ? c->hwnd : 0, m, &p1, &p2);
    uint32_t a[] = { d, id, m, p1, p2 };
    return call32(api_WinSendDlgItemMsg, 5, a);
}
