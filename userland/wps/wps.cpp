/*
 * wps - the Workplace Shell for OS/2 Warp ARM64: a Wayland compositor on
 * wlroots 0.19, in the style of OS/2 Warp 4.
 *
 *   - the desktop: OS/2 blue, a column of objects (OS/2 System, Drives,
 *     Command Prompt, Drive C, Drive D, Programs) and Shut Down; double-
 *     click opens one
 *   - the WarpCenter along the top: the system menu, Window List, Command
 *     Prompt, Drives, Programs, and the clock
 *   - every window gets an OS/2 frame (server-side decoration): a 3D
 *     border to resize by, a title bar to move by - blue when active - with
 *     the system menu icon (double-click: close) and hide, maximize and
 *     close buttons
 *   - folder windows ("Drive D - Icon View"): a folder's objects as icons;
 *     double-click a folder for its own window, a program to run it, a file
 *     to edit it
 *   - the Window List (Ctrl+Esc): every window, hidden ones too
 *   - keys: Ctrl+Esc Window List, Alt+Tab next window, Alt+F4 close,
 *     Alt+O / Alt+Shift+Enter Command Prompt, Alt+P Programs,
 *     Alt+Shift+Q back to the console
 *
 * The pictures are drawn here (pixman), the text by text.c (fcft). Clients
 * are ordinary Wayland programs: foot (an OS/2 Command Prompt), fuzzel
 * (Programs, a layer-shell surface), anything xdg-shell.
 */
#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include <drm_fourcc.h>
#include <pixman.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
/* wlroots' headers are C: "[static 4]" array parameters aren't C++ */
#define static
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
/* ... and "namespace" is a field name in C */
#define namespace namespace_
#include <wlr/types/wlr_layer_shell_v1.h>
#undef namespace
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#undef static
#include "os2_backend.h"
}
#include "text.h"

extern char **environ;

/* ── the system's programs (the stick's \OS2 folder) ─────────────────── */
static const char *SYS = "/h/OS2";
static std::string sys(const char *p) { return std::string(SYS) + "/" + p; }

/* ── colours (ARGB) and sizes ────────────────────────────────────────── */
enum : uint32_t {
    C_DESK = 0xFF2C4DA3, C_DESK2 = 0xFF27479B,      /* the desktop's two blues */
    C_FACE = 0xFFC0C0C0, C_LIGHT = 0xFFFFFFFF, C_SHADOW = 0xFF808080, C_DARK = 0xFF000000,
    C_TITLE = 0xFF1C3C9C, C_TITLE2 = 0xFF5878C8,    /* active title bar: a gradient */
    C_TITLE_OFF = 0xFFA8A8A8, C_TEXT = 0xFF000000, C_WHITE = 0xFFFFFFFF,
    C_SEL = 0xFF0A246A, C_FOLDERBG = 0xFFFFFFFF,
};
static const int B = 4;         /* frame border */
static const int T = 20;        /* title bar */
static const int BAR = 28;      /* the WarpCenter */
static const int MENUBAR = 20;  /* a folder's menu bar */

/* ── listeners ───────────────────────────────────────────────────────── */
struct Listener {
    wl_listener l{};
    std::function<void(void *)> fn;
    bool on = false;
    void connect(wl_signal *s, std::function<void(void *)> f) {
        fn = std::move(f);
        l.notify = [](wl_listener *x, void *d) { reinterpret_cast<Listener *>(x)->fn(d); };
        wl_signal_add(s, &l);
        on = true;
    }
    void disconnect() { if (on) { wl_list_remove(&l.link); on = false; } }
    ~Listener() { disconnect(); }
};

/* ── buffers we draw into ───────────────────────────────────────────── */
struct PixBuf {
    wlr_buffer base;                 /* first: the wlr_buffer is the PixBuf */
    uint32_t *px;
    pixman_image_t *img;
    int w, h;
};

static const wlr_buffer_impl *pixbuf_impl()
{
    static wlr_buffer_impl impl;
    static bool ready;
    if (!ready) {
        impl.destroy = [](wlr_buffer *b) {
            PixBuf *p = reinterpret_cast<PixBuf *>(b);
            pixman_image_unref(p->img);
            free(p->px);
            delete p;
        };
        impl.begin_data_ptr_access = [](wlr_buffer *b, uint32_t, void **data, uint32_t *fmt,
                                        size_t *stride) {
            PixBuf *p = reinterpret_cast<PixBuf *>(b);
            *data = p->px;
            *fmt = DRM_FORMAT_ARGB8888;
            *stride = (size_t)p->w * 4;
            return true;
        };
        impl.end_data_ptr_access = [](wlr_buffer *) {};
        ready = true;
    }
    return &impl;
}

static PixBuf *pixbuf_new(int w, int h)
{
    PixBuf *p = new PixBuf();
    p->w = w > 0 ? w : 1;
    p->h = h > 0 ? h : 1;
    p->px = (uint32_t *)calloc((size_t)p->w * p->h, 4);
    p->img = pixman_image_create_bits(PIXMAN_a8r8g8b8, p->w, p->h, p->px, p->w * 4);
    wlr_buffer_init(&p->base, pixbuf_impl(), p->w, p->h);
    return p;
}

/* a scene node showing our latest picture */
struct Picture {
    wlr_scene_buffer *node = nullptr;
    void show(wlr_scene_tree *parent, PixBuf *pb, void *tag) {
        if (!node) {
            node = wlr_scene_buffer_create(parent, &pb->base);
            node->node.data = tag;
        } else {
            wlr_scene_buffer_set_buffer(node, &pb->base);
        }
        wlr_buffer_drop(&pb->base);              /* the scene holds it now */
    }
    void destroy() { if (node) { wlr_scene_node_destroy(&node->node); node = nullptr; } }
};

/* ── drawing ────────────────────────────────────────────────────────── */
static pixman_color_t pcolor(uint32_t c)
{
    return { (uint16_t)(((c >> 16) & 0xFF) * 257), (uint16_t)(((c >> 8) & 0xFF) * 257),
             (uint16_t)((c & 0xFF) * 257), (uint16_t)(((c >> 24) & 0xFF) * 257) };
}

static void fill(pixman_image_t *im, int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    pixman_color_t col = pcolor(c);
    pixman_rectangle16_t r = { (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h };
    pixman_image_fill_rectangles((c >> 24) == 0xFF ? PIXMAN_OP_SRC : PIXMAN_OP_OVER, im, &col, 1, &r);
}

/* a 3D edge: raised (light top-left, dark bottom-right) or sunken */
static void bevel(pixman_image_t *im, int x, int y, int w, int h, bool raised, int depth = 2)
{
    uint32_t a1 = raised ? C_LIGHT : C_SHADOW, b1 = raised ? C_DARK : C_LIGHT;
    uint32_t a2 = raised ? 0xFFDFDFDF : C_DARK, b2 = raised ? C_SHADOW : 0xFFDFDFDF;
    for (int i = 0; i < depth; i++) {
        uint32_t a = i == 0 ? a1 : a2, b = i == 0 ? b1 : b2;
        fill(im, x + i, y + i, w - 2 * i, 1, a);
        fill(im, x + i, y + i, 1, h - 2 * i, a);
        fill(im, x + i, y + h - 1 - i, w - 2 * i, 1, b);
        fill(im, x + w - 1 - i, y + i, 1, h - 2 * i, b);
    }
}

static void gradient_h(pixman_image_t *im, int x, int y, int w, int h, uint32_t c1, uint32_t c2)
{
    for (int i = 0; i < w; i++) {
        auto mix = [&](int sh) {
            int a = (c1 >> sh) & 0xFF, b = (c2 >> sh) & 0xFF;
            return (uint32_t)(a + (b - a) * i / (w > 1 ? w - 1 : 1)) << sh;
        };
        fill(im, x + i, y, 1, h, 0xFF000000u | mix(16) | mix(8) | mix(0));
    }
}

/* ── icons: drawn here, OS/2 Warp in spirit ─────────────────────────── */
enum Icon { I_FOLDER, I_FILE, I_TEXT, I_PROGRAM, I_PROMPT, I_DRIVE, I_SYSTEM, I_PROGRAMS,
            I_WINLIST, I_SHUTDOWN, I_DRIVES, I_COUNT };

static void disc(pixman_image_t *im, int cx, int cy, int r, uint32_t c)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) w++;
        fill(im, cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

/* draws icon i at (x, y), s pixels square (32 on the desktop, 16 in title bars) */
static void draw_icon(pixman_image_t *im, int i, int x, int y, int s)
{
    auto S = [&](int v) { return v * s / 32; };
    switch (i) {
    case I_FOLDER:
        fill(im, x + S(2), y + S(6), S(12), S(5), 0xFFB08820);              /* tab */
        fill(im, x + S(2), y + S(9), S(28), S(19), 0xFFB08820);             /* outline */
        fill(im, x + S(3), y + S(10), S(26), S(17), 0xFFF0C850);            /* body */
        fill(im, x + S(3), y + S(10), S(26), S(2), 0xFFF8E098);             /* light edge */
        break;
    case I_FILE: case I_TEXT:
        fill(im, x + S(7), y + S(3), S(18), S(26), C_DARK);
        fill(im, x + S(8), y + S(4), S(16), S(24), C_WHITE);
        fill(im, x + S(19), y + S(4), S(5), S(5), 0xFFC0C0C0);              /* folded corner */
        if (i == I_TEXT)
            for (int l = 0; l < 5; l++) fill(im, x + S(10), y + S(11 + 3 * l), S(12), 1, C_SHADOW);
        break;
    case I_PROGRAM:
        fill(im, x + S(3), y + S(5), S(26), S(22), C_DARK);
        fill(im, x + S(4), y + S(6), S(24), S(20), C_FACE);
        fill(im, x + S(5), y + S(7), S(22), S(4), C_TITLE);
        fill(im, x + S(5), y + S(12), S(22), S(13), C_WHITE);
        break;
    case I_PROMPT:
        fill(im, x + S(2), y + S(4), S(28), S(24), C_FACE);
        bevel(im, x + S(2), y + S(4), S(28), S(24), true, 1);
        fill(im, x + S(4), y + S(6), S(24), S(20), C_DARK);
        if (s >= 32) text_draw(im, TEXT_SMALL, x + 6, y + 19, "C:>", 0xFFE0E0E0, 0);
        else fill(im, x + S(7), y + S(14), S(8), 2, 0xFFE0E0E0);
        break;
    case I_DRIVE: case I_DRIVES:
        fill(im, x + S(3), y + S(11), S(26), S(12), C_FACE);
        bevel(im, x + S(3), y + S(11), S(26), S(12), true, 1);
        fill(im, x + S(7), y + S(16), S(14), S(2), C_SHADOW);                /* slot */
        fill(im, x + S(23), y + S(15), S(3), S(3), 0xFF20C020);              /* the light */
        if (i == I_DRIVES) {
            fill(im, x + S(5), y + S(4), S(22), S(6), C_FACE);
            bevel(im, x + S(5), y + S(4), S(22), S(6), true, 1);
        }
        break;
    case I_SYSTEM:
        fill(im, x + S(3), y + S(3), S(26), S(20), C_FACE);
        bevel(im, x + S(3), y + S(3), S(26), S(20), true, 1);
        fill(im, x + S(6), y + S(6), S(20), S(14), 0xFF1C3C9C);              /* screen */
        fill(im, x + S(8), y + S(8), S(10), S(2), 0xFF88A8E8);
        fill(im, x + S(12), y + S(23), S(8), S(3), C_SHADOW);                /* stand */
        fill(im, x + S(7), y + S(26), S(18), S(3), C_FACE);
        break;
    case I_PROGRAMS:
        draw_icon(im, I_PROGRAM, x - S(3), y - S(3), S(28));
        draw_icon(im, I_PROGRAM, x + S(5), y + S(5), S(28));
        break;
    case I_WINLIST:
        fill(im, x + S(4), y + S(3), S(24), S(26), C_DARK);
        fill(im, x + S(5), y + S(4), S(22), S(24), C_WHITE);
        fill(im, x + S(5), y + S(4), S(22), S(4), C_TITLE);
        for (int l = 0; l < 5; l++) fill(im, x + S(8), y + S(11 + 3 * l), S(16), S(2), C_SHADOW);
        break;
    case I_SHUTDOWN:
        disc(im, x + S(16), y + S(16), S(13), 0xFF801010);
        disc(im, x + S(16), y + S(16), S(11), 0xFFD02020);
        disc(im, x + S(16), y + S(17), S(6), C_WHITE);
        disc(im, x + S(16), y + S(17), S(4), 0xFFD02020);
        fill(im, x + S(15), y + S(7), S(3), S(10), C_WHITE);
        break;
    }
}

/* ── objects: what an icon stands for ───────────────────────────────── */
enum Action { A_NONE, A_FOLDER, A_RUN, A_EDIT, A_SCRIPT, A_PROMPT, A_PROGRAMS, A_WINLIST,
              A_SHUTDOWN };
struct Object {
    std::string label;
    int icon;
    Action action;
    std::string path;                /* A_FOLDER: a directory or "::system"/"::drives" */
};

/* ── windows ────────────────────────────────────────────────────────── */
struct Window;
struct Tag { enum K { DESK, BAR, WIN, MENU } k; Window *w; };

struct Window {
    enum Kind { XDG, FOLDER, WINLIST } kind;
    Tag tag{ Tag::WIN, this };
    wlr_scene_tree *tree = nullptr;
    Picture frame;
    wlr_scene_tree *content = nullptr;            /* XDG: the client's surfaces */
    int x = 0, y = 0, cw = 400, ch = 260;         /* position, content size */
    bool mapped = false, hidden = false, maximized = false;
    int sx, sy, scw, sch;                         /* before maximizing */
    /* XDG */
    wlr_xdg_toplevel *top = nullptr;
    Listener map, unmap, commit, destroy, title, req_move, req_resize, req_max, req_min, req_full;
    /* FOLDER */
    std::string path, name;
    std::vector<Object> objects;
    int sel = -1, scroll = 0;
    /* WINLIST */
    int lsel = -1;

    std::string titleText() const;
    int fw() const { return cw + 2 * B; }
    int fh() const { return ch + 2 * B + T; }
};

struct Menu {
    struct Item { std::string label; std::function<void()> run; };
    std::vector<Item> items;
    int x = 0, y = 0, w = 0, h = 0, hover = -1;
    Picture pic;
    Tag tag{ Tag::MENU, nullptr };
};

/* ── the server ─────────────────────────────────────────────────────── */
struct Server {
    wl_display *display;
    wlr_backend *backend;
    wlr_renderer *renderer;
    wlr_allocator *allocator;
    wlr_scene *scene;
    wlr_scene_output_layout *scene_layout;
    wlr_output_layout *layout;
    wlr_output *output = nullptr;
    wlr_xdg_shell *xdg;
    wlr_layer_shell_v1 *layers;
    wlr_xdg_decoration_manager_v1 *decos;
    wlr_cursor *cursor;
    wlr_seat *seat;
    wlr_scene_tree *t_desk, *t_wins, *t_bar, *t_layers, *t_menu;
    int W = 1024, H = 768;

    Picture desk, bar;
    Tag desk_tag{ Tag::DESK, nullptr }, bar_tag{ Tag::BAR, nullptr };
    std::vector<Object> desk_objs;
    std::vector<std::pair<int, int>> desk_pos;   /* where each desktop object is */
    int desk_sel = -1;

    std::vector<Window *> windows;               /* back to front */
    Window *active = nullptr;
    Window *winlist = nullptr;
    Menu *menu = nullptr;
    PixBuf *arrow = nullptr;
    bool arrow_shown = false;
    int cascade = 0;

    /* pointer grabs */
    enum Mode { NONE, MOVE, RESIZE } mode = NONE;
    Window *grab = nullptr;
    uint32_t grab_edges = 0;
    double gx, gy;
    int gwx, gwy, gcw, gch;

    /* double clicks */
    uint32_t last_click = 0;
    const void *last_target = nullptr;
    int last_index = -1;

    Listener new_output, new_input, new_toplevel, new_popup, new_layer, new_deco,
             cursor_motion, cursor_motion_abs, cursor_button, cursor_axis, cursor_frame,
             request_cursor, request_selection;
};
static Server S;

/* ── launching ──────────────────────────────────────────────────────── */
static void spawn(const std::vector<std::string> &args)
{
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
        wlr_log(WLR_ERROR, "wps: can't start %s", argv[0]);
}

static std::vector<std::string> terminal(bool hold = false)
{
    std::vector<std::string> v = { sys("foot"), "-c", sys("foot.ini") };
    if (hold) v.push_back("--hold");
    return v;
}

static void open_folder(const std::string &path, const std::string &name);
static void toggle_winlist();
static void focus(Window *w);

static void run_object(const Object &o)
{
    switch (o.action) {
    case A_FOLDER:   open_folder(o.path, o.label); break;
    case A_PROMPT:   spawn(terminal()); break;
    case A_PROGRAMS: spawn({ sys("fuzzel"), "--config", sys("fuzzel.ini") }); break;
    case A_WINLIST:  toggle_winlist(); break;
    case A_SHUTDOWN: spawn({ sys("os2cmd"), "SHUTDOWN" }); break;
    case A_RUN:    { auto v = terminal(true); v.push_back(o.path); spawn(v); break; }
    case A_SCRIPT: { auto v = terminal(true); v.push_back(sys("busybox")); v.push_back("sh");
                     v.push_back(o.path); spawn(v); break; }
    case A_EDIT:   { auto v = terminal(); v.push_back(sys("busybox")); v.push_back("vi");
                     v.push_back(o.path); spawn(v); break; }
    case A_NONE: break;
    }
}

/* ── folders ────────────────────────────────────────────────────────── */
static bool hidden_at_root(const std::string &n)
{
    static const char *h[] = { "OS2", "bin", "emx", "etc", "home", "usr", "tmp", "var", "lib",
                               "opt", "sbin", "dev", "proc", "h", "H", "BOOT", nullptr };
    for (int i = 0; h[i]; i++) if (n == h[i]) return true;
    return false;
}

static std::vector<Object> folder_objects(const std::string &path)
{
    std::vector<Object> v;
    if (path == "::system") {
        v.push_back({ "Drives", I_DRIVES, A_FOLDER, "::drives" });
        v.push_back({ "Command Prompt", I_PROMPT, A_PROMPT, "" });
        v.push_back({ "Programs", I_PROGRAMS, A_PROGRAMS, "" });
        v.push_back({ "Window List", I_WINLIST, A_WINLIST, "" });
        v.push_back({ "Shut Down", I_SHUTDOWN, A_SHUTDOWN, "" });
        return v;
    }
    if (path == "::drives") {
        v.push_back({ "Drive C", I_DRIVE, A_FOLDER, "/" });
        v.push_back({ "Drive D", I_DRIVE, A_FOLDER, "/h" });
        return v;
    }
    DIR *d = opendir(path.c_str());
    if (!d) return v;
    bool root = path == "/" || path == "/h";
    std::vector<Object> dirs, files;
    while (dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        if (root && hidden_at_root(n)) continue;
        std::string p = (path == "/" ? "" : path) + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { dirs.push_back({ n, I_FOLDER, A_FOLDER, p }); continue; }
        char head[4] = { 0 };
        int fd = open(p.c_str(), O_RDONLY);
        if (fd >= 0) { if (read(fd, head, 4) < 0) head[0] = 0; close(fd); }
        auto ends = [&](const char *x) {
            size_t l = strlen(x);
            return n.size() > l && strcasecmp(n.c_str() + n.size() - l, x) == 0;
        };
        if (!memcmp(head, "\x7f" "ELF", 4)) files.push_back({ n, I_PROGRAM, A_RUN, p });
        else if ((head[0] == '#' && head[1] == '!') || ends(".sh")) files.push_back({ n, I_PROGRAM, A_SCRIPT, p });
        else if (ends(".txt") || ends(".c") || ends(".h") || ends(".cpp") || ends(".s") || ends(".md") ||
                 ends(".ini") || ends(".sys") || ends("makefile"))
            files.push_back({ n, I_TEXT, A_EDIT, p });
        else files.push_back({ n, I_FILE, A_EDIT, p });
    }
    closedir(d);
    auto by = [](const Object &a, const Object &b) { return strcasecmp(a.label.c_str(), b.label.c_str()) < 0; };
    std::sort(dirs.begin(), dirs.end(), by);
    std::sort(files.begin(), files.end(), by);
    v = dirs;
    v.insert(v.end(), files.begin(), files.end());
    return v;
}

std::string Window::titleText() const
{
    if (kind == FOLDER) return name + " - Icon View";
    if (kind == WINLIST) return "Window List";
    if (top && top->title && *top->title) return top->title;
    if (top && top->app_id && *top->app_id) return top->app_id;
    return "(untitled)";
}

/* ── drawing the desktop and the WarpCenter ─────────────────────────── */
static void draw_label(pixman_image_t *im, int cx, int y, const std::string &s, bool sel, bool on_desk)
{
    int w = std::min(text_width(TEXT_SMALL, s.c_str()), 86), h = text_height(TEXT_SMALL);
    int x = cx - w / 2;
    if (sel) fill(im, x - 2, y, w + 4, h + 2, C_SEL);
    if (on_desk && !sel) text_draw(im, TEXT_SMALL, x + 1, y + 1 + text_ascent(TEXT_SMALL), s.c_str(), C_DARK, x + 87);
    text_draw(im, TEXT_SMALL, x, y + text_ascent(TEXT_SMALL), s.c_str(),
              (sel || on_desk) ? C_WHITE : C_TEXT, x + 86);
}

static void draw_desktop()
{
    PixBuf *pb = pixbuf_new(S.W, S.H);
    pixman_image_t *im = pb->img;
    fill(im, 0, 0, S.W, S.H, C_DESK);
    for (int y = 0; y < S.H; y += 2)                      /* a quiet texture */
        for (int x = (y / 2) % 4; x < S.W; x += 4) pb->px[y * S.W + x] = C_DESK2;
    S.desk_pos.clear();
    for (size_t i = 0; i < S.desk_objs.size(); i++) {
        int x, y;
        if (S.desk_objs[i].action == A_SHUTDOWN) { x = S.W - 70; y = S.H - 80; }
        else { x = 18; y = BAR + 16 + (int)i * 72; }
        S.desk_pos.push_back({ x, y });
        draw_icon(im, S.desk_objs[i].icon, x + 11, y, 32);
        draw_label(im, x + 27, y + 36, S.desk_objs[i].label, (int)i == S.desk_sel, true);
    }
    S.desk.show(S.t_desk, pb, &S.desk_tag);
}

/* the WarpCenter's buttons, left to right */
struct BarButton { int icon; Action action; const char *label; };
static const BarButton bar_buttons[] = {
    { I_SYSTEM, A_NONE, "System" }, { I_WINLIST, A_WINLIST, "Window List" },
    { I_PROMPT, A_PROMPT, "Command Prompt" }, { I_DRIVES, A_FOLDER, "Drives" },
    { I_PROGRAMS, A_PROGRAMS, "Programs" },
};
static const int BAR_BTN = 26;

static void draw_bar()
{
    PixBuf *pb = pixbuf_new(S.W, BAR);
    pixman_image_t *im = pb->img;
    fill(im, 0, 0, S.W, BAR, C_FACE);
    bevel(im, 0, 0, S.W, BAR, true, 1);
    int x = 4;
    for (auto &b : bar_buttons) {
        bevel(im, x, 3, BAR_BTN - 2, BAR - 6, true, 1);
        draw_icon(im, b.icon, x + 4, 5, 16);
        x += BAR_BTN;
    }
    /* the window being used, then the clock */
    if (S.active) {
        fill(im, x + 8, 5, 1, BAR - 10, C_SHADOW);
        text_draw(im, TEXT_UI, x + 16, BAR / 2 + text_ascent(TEXT_UI) / 2 - 1, S.active->titleText().c_str(),
                  C_TEXT, S.W - 140);
    }
    char clk[32];
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    snprintf(clk, sizeof clk, "%d:%02d:%02d %s", h12, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? "AM" : "PM");
    int cw = text_width(TEXT_UI, clk) + 14;
    fill(im, S.W - cw - 6, 4, cw, BAR - 8, C_FACE);
    bevel(im, S.W - cw - 6, 4, cw, BAR - 8, false, 1);
    text_draw(im, TEXT_UI, S.W - cw + 1, BAR / 2 + text_ascent(TEXT_UI) / 2 - 1, clk, C_TEXT, 0);
    S.bar.show(S.t_bar, pb, &S.bar_tag);
}

/* ── drawing windows ────────────────────────────────────────────────── */
static int window_icon(const Window *w)
{
    if (w->kind == Window::FOLDER) return w->path.rfind("::", 0) == 0 ? I_SYSTEM : (w->path == "/" || w->path == "/h") ? I_DRIVE : I_FOLDER;
    if (w->kind == Window::WINLIST) return I_WINLIST;
    std::string t = w->titleText();
    return t.find("Command Prompt") != std::string::npos ? I_PROMPT : I_PROGRAM;
}

/* title bar buttons, from the right: close, maximize, hide */
enum { HIT_NONE, HIT_CONTENT, HIT_TITLE, HIT_SYSMENU, HIT_CLOSE, HIT_MAX, HIT_HIDE, HIT_BORDER,
       HIT_MENUBAR };
static const int BTN_W = 18, BTN_H = 16;
static int btn_x(const Window *w, int i) { return w->fw() - B - 2 - (i + 1) * (BTN_W + 1); }

static void draw_button(pixman_image_t *im, int x, int y, int kind)
{
    fill(im, x, y, BTN_W, BTN_H, C_FACE);
    bevel(im, x, y, BTN_W, BTN_H, true, 1);
    int cx = x + BTN_W / 2, cy = y + BTN_H / 2;
    if (kind == HIT_CLOSE) {
        for (int i = -4; i <= 4; i++) { fill(im, cx + i, cy + i, 2, 1, C_DARK); fill(im, cx + i, cy - i, 2, 1, C_DARK); }
    } else if (kind == HIT_MAX) {
        fill(im, cx - 5, cy - 4, 11, 9, C_DARK);
        fill(im, cx - 4, cy - 2, 9, 6, C_FACE);
    } else {
        fill(im, cx - 2, cy + 1, 5, 3, C_DARK);
    }
}

static void draw_folder_content(Window *w, pixman_image_t *im, int ox, int oy);
static void draw_winlist_content(Window *w, pixman_image_t *im, int ox, int oy);

static void draw_window(Window *w)
{
    if (!w->mapped) return;
    PixBuf *pb = pixbuf_new(w->fw(), w->fh());
    pixman_image_t *im = pb->img;
    int W = w->fw(), H = w->fh();
    bool on = S.active == w;
    fill(im, 0, 0, W, H, C_FACE);
    bevel(im, 0, 0, W, H, true, 2);
    bevel(im, B - 1, B + T - 1, w->cw + 2, w->ch + 2, false, 1);
    if (on) gradient_h(im, B, B, W - 2 * B, T - 1, C_TITLE, C_TITLE2);
    else fill(im, B, B, W - 2 * B, T - 1, C_TITLE_OFF);
    draw_icon(im, window_icon(w), B + 2, B + 1, 16);
    int text_right = btn_x(w, 2) - 6;
    std::string t = w->titleText();
    int tw = text_width(TEXT_BOLD, t.c_str());
    int tx = std::max(B + 24, B + 20 + (text_right - B - 20 - tw) / 2);
    text_draw(im, TEXT_BOLD, tx, B + T / 2 + text_ascent(TEXT_BOLD) / 2 - 1, t.c_str(),
              on ? C_WHITE : 0xFF303030, text_right);
    draw_button(im, btn_x(w, 0), B + 1, HIT_CLOSE);
    draw_button(im, btn_x(w, 1), B + 1, HIT_MAX);
    draw_button(im, btn_x(w, 2), B + 1, HIT_HIDE);
    if (w->kind == Window::FOLDER) draw_folder_content(w, im, B, B + T);
    else if (w->kind == Window::WINLIST) draw_winlist_content(w, im, B, B + T);
    else fill(im, B, B + T, w->cw, w->ch, C_WHITE);       /* under the client */
    w->frame.show(w->tree, pb, &w->tag);
    wlr_scene_node_lower_to_bottom(&w->frame.node->node);
}

/* folder: a menu bar, then a grid of icons */
static const int CELL_W = 88, CELL_H = 66;
static int folder_cols(const Window *w) { return std::max(1, (w->cw - 8) / CELL_W); }
static int folder_rows_visible(const Window *w) { return std::max(1, (w->ch - MENUBAR - 4) / CELL_H); }

static void draw_folder_content(Window *w, pixman_image_t *im, int ox, int oy)
{
    fill(im, ox, oy, w->cw, MENUBAR, C_FACE);
    text_draw(im, TEXT_UI, ox + 8, oy + MENUBAR / 2 + text_ascent(TEXT_UI) / 2 - 1, "Folder", C_TEXT, 0);
    fill(im, ox, oy + MENUBAR - 1, w->cw, 1, C_SHADOW);
    int ty = oy + MENUBAR;
    fill(im, ox, ty, w->cw, w->ch - MENUBAR, C_FOLDERBG);
    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, ox, ty, w->cw, w->ch - MENUBAR);
    pixman_image_set_clip_region32(im, &clip);
    int cols = folder_cols(w);
    for (size_t i = 0; i < w->objects.size(); i++) {
        int r = (int)i / cols - w->scroll, c = (int)i % cols;
        if (r < 0) continue;
        int x = ox + 4 + c * CELL_W, y = ty + 6 + r * CELL_H;
        if (y > oy + w->ch) break;
        draw_icon(im, w->objects[i].icon, x + (CELL_W - 32) / 2, y, 32);
        draw_label(im, x + CELL_W / 2, y + 36, w->objects[i].label, (int)i == w->sel, false);
    }
    pixman_image_set_clip_region32(im, nullptr);
    pixman_region32_fini(&clip);
    if (w->objects.empty())
        text_draw(im, TEXT_UI, ox + 12, ty + 24, "(empty)", C_SHADOW, 0);
}

static std::vector<Window *> listed_windows()
{
    std::vector<Window *> v;
    for (Window *w : S.windows) if (w != S.winlist && w->mapped) v.push_back(w);
    return v;
}

static const int ROW = 18;
static void draw_winlist_content(Window *w, pixman_image_t *im, int ox, int oy)
{
    fill(im, ox, oy, w->cw, w->ch, C_WHITE);
    auto list = listed_windows();
    int y = oy + 4;
    text_draw(im, TEXT_BOLD, ox + 6, y + text_ascent(TEXT_BOLD), "Desktop - Icon View", C_TEXT, ox + w->cw - 4);
    y += ROW;
    for (size_t i = 0; i < list.size(); i++) {
        bool sel = (int)i == w->lsel;
        if (sel) fill(im, ox + 2, y - 1, w->cw - 4, ROW, C_SEL);
        std::string t = list[i]->titleText() + (list[i]->hidden ? "  (hidden)" : "");
        draw_icon(im, window_icon(list[i]), ox + 10, y, 16);
        text_draw(im, TEXT_UI, ox + 32, y + text_ascent(TEXT_UI) + 1, t.c_str(), sel ? C_WHITE : C_TEXT, ox + w->cw - 4);
        y += ROW;
    }
}

static void redraw_winlist() { if (S.winlist) draw_window(S.winlist); }

/* ── window management ─────────────────────────────────────────────── */
static void place(Window *w)
{
    w->x = 90 + 28 * (S.cascade % 8);
    w->y = BAR + 16 + 24 * (S.cascade % 8);
    S.cascade++;
    if (w->x + w->fw() > S.W) w->x = std::max(0, S.W - w->fw());
    if (w->y + w->fh() > S.H) w->y = std::max(BAR, S.H - w->fh());
    wlr_scene_node_set_position(&w->tree->node, w->x, w->y);
}

static void position_content(Window *w)
{
    if (!w->top) return;
    wlr_box g = w->top->base->geometry;
    wlr_scene_node_set_position(&w->content->node, B - g.x, B + T - g.y);
}

static void focus(Window *w)
{
    Window *old = S.active;
    if (w && w->hidden) { w->hidden = false; wlr_scene_node_set_enabled(&w->tree->node, true); }
    if (old && old != w && old->top) wlr_xdg_toplevel_set_activated(old->top, false);
    S.active = w;
    if (w) {
        auto it = std::find(S.windows.begin(), S.windows.end(), w);
        if (it != S.windows.end()) { S.windows.erase(it); S.windows.push_back(w); }
        wlr_scene_node_raise_to_top(&w->tree->node);
        if (w->top) {
            wlr_xdg_toplevel_set_activated(w->top, true);
            wlr_keyboard *kb = wlr_seat_get_keyboard(S.seat);
            if (kb) wlr_seat_keyboard_notify_enter(S.seat, w->top->base->surface, kb->keycodes,
                                                   kb->num_keycodes, &kb->modifiers);
        } else {
            wlr_seat_keyboard_notify_clear_focus(S.seat);
        }
    } else {
        wlr_seat_keyboard_notify_clear_focus(S.seat);
    }
    if (old && old != w) draw_window(old);
    if (w) draw_window(w);
    draw_bar();
    if (w != S.winlist) redraw_winlist();
}

static void focus_next_visible(Window *except)
{
    for (auto it = S.windows.rbegin(); it != S.windows.rend(); ++it)
        if (*it != except && (*it)->mapped && !(*it)->hidden) { focus(*it); return; }
    focus(nullptr);
}

static void hide(Window *w)
{
    w->hidden = true;
    wlr_scene_node_set_enabled(&w->tree->node, false);
    if (S.active == w) focus_next_visible(w);
    redraw_winlist();
}

static void close_window(Window *w);

static void toggle_maximize(Window *w)
{
    if (!w->maximized) {
        w->sx = w->x; w->sy = w->y; w->scw = w->cw; w->sch = w->ch;
        w->x = 0; w->y = BAR;
        int cw = S.W - 2 * B, ch = S.H - BAR - 2 * B - T;
        w->maximized = true;
        if (w->top) { wlr_xdg_toplevel_set_maximized(w->top, true); wlr_xdg_toplevel_set_size(w->top, cw, ch); }
        else { w->cw = cw; w->ch = ch; }
    } else {
        w->maximized = false;
        w->x = w->sx; w->y = w->sy;
        if (w->top) { wlr_xdg_toplevel_set_maximized(w->top, false); wlr_xdg_toplevel_set_size(w->top, w->scw, w->sch); }
        else { w->cw = w->scw; w->ch = w->sch; }
    }
    wlr_scene_node_set_position(&w->tree->node, w->x, w->y);
    draw_window(w);
}

/* internal windows: folders and the Window List */
static Window *new_internal(Window::Kind k, int cw, int ch, Window **slot = nullptr)
{
    Window *w = new Window();
    if (slot) *slot = w;                         /* (known before it's first drawn) */
    w->kind = k;
    w->cw = cw; w->ch = ch;
    w->tree = wlr_scene_tree_create(S.t_wins);
    w->tree->node.data = &w->tag;
    w->mapped = true;
    S.windows.push_back(w);
    place(w);
    focus(w);
    return w;
}

static void open_folder(const std::string &path, const std::string &name)
{
    for (Window *w : S.windows)
        if (w->kind == Window::FOLDER && w->path == path) { focus(w); return; }
    Window *w = new Window();
    w->kind = Window::FOLDER;
    w->path = path;
    w->name = name;
    w->objects = folder_objects(path);
    w->cw = 470; w->ch = 290;
    w->tree = wlr_scene_tree_create(S.t_wins);
    w->tree->node.data = &w->tag;
    w->mapped = true;
    S.windows.push_back(w);
    place(w);
    focus(w);
}

static void toggle_winlist()
{
    if (S.winlist) {
        if (S.active == S.winlist && !S.winlist->hidden) { close_window(S.winlist); return; }
        focus(S.winlist);
        return;
    }
    new_internal(Window::WINLIST, 300, 220, &S.winlist);
    S.winlist->x = (S.W - S.winlist->fw()) / 2;
    S.winlist->y = (S.H - S.winlist->fh()) / 2;
    wlr_scene_node_set_position(&S.winlist->tree->node, S.winlist->x, S.winlist->y);
}

static void forget(Window *w)
{
    auto it = std::find(S.windows.begin(), S.windows.end(), w);
    if (it != S.windows.end()) S.windows.erase(it);
    if (S.grab == w) { S.grab = nullptr; S.mode = Server::NONE; }
    if (S.winlist == w) S.winlist = nullptr;
    if (S.active == w) { S.active = nullptr; focus_next_visible(w); }
    redraw_winlist();
    draw_bar();
}

static void close_window(Window *w)
{
    if (w->top) { wlr_xdg_toplevel_send_close(w->top); return; }
    forget(w);
    w->frame.destroy();
    wlr_scene_node_destroy(&w->tree->node);
    delete w;
}

/* ── xdg-shell windows ─────────────────────────────────────────────── */
static void new_toplevel(wlr_xdg_toplevel *top)
{
    Window *w = new Window();
    w->kind = Window::XDG;
    w->top = top;
    w->tree = wlr_scene_tree_create(S.t_wins);
    w->tree->node.data = &w->tag;
    w->content = wlr_scene_xdg_surface_create(w->tree, top->base);
    top->base->data = w->content;                  /* (popups hang off it) */

    w->map.connect(&top->base->surface->events.map, [w](void *) {
        wlr_box g = w->top->base->geometry;
        w->cw = g.width > 0 ? g.width : 400;
        w->ch = g.height > 0 ? g.height : 260;
        w->mapped = true;
        S.windows.push_back(w);
        place(w);
        position_content(w);
        focus(w);
    });
    w->unmap.connect(&top->base->surface->events.unmap, [w](void *) {
        w->mapped = false;
        w->frame.destroy();
        forget(w);
    });
    w->commit.connect(&top->base->surface->events.commit, [w](void *) {
        if (w->top->base->initial_commit) {
            wlr_xdg_toplevel_set_size(w->top, 0, 0);
            return;
        }
        if (!w->mapped) return;
        wlr_box g = w->top->base->geometry;
        if (g.width > 0 && g.height > 0 && (g.width != w->cw || g.height != w->ch)) {
            w->cw = g.width; w->ch = g.height;
            draw_window(w);
        }
        position_content(w);
    });
    w->title.connect(&top->events.set_title, [w](void *) {
        if (w->mapped) { draw_window(w); draw_bar(); redraw_winlist(); }
    });
    auto begin = [w](Server::Mode m, uint32_t edges) {
        S.grab = w; S.mode = m; S.grab_edges = edges;
        S.gx = S.cursor->x; S.gy = S.cursor->y;
        S.gwx = w->x; S.gwy = w->y; S.gcw = w->cw; S.gch = w->ch;
    };
    w->req_move.connect(&top->events.request_move, [begin](void *) { begin(Server::MOVE, 0); });
    w->req_resize.connect(&top->events.request_resize, [begin](void *d) {
        begin(Server::RESIZE, static_cast<wlr_xdg_toplevel_resize_event *>(d)->edges);
    });
    w->req_max.connect(&top->events.request_maximize, [w](void *) {
        if (w->mapped) toggle_maximize(w);
        else if (w->top->base->initialized) wlr_xdg_surface_schedule_configure(w->top->base);
    });
    w->req_min.connect(&top->events.request_minimize, [w](void *) { if (w->mapped) hide(w); });
    w->req_full.connect(&top->events.request_fullscreen, [w](void *) {
        if (w->top->base->initialized) wlr_xdg_surface_schedule_configure(w->top->base);
    });
    w->destroy.connect(&top->events.destroy, [w](void *) {
        if (w->mapped) forget(w);
        w->frame.destroy();
        delete w;                                   /* (the scene tree goes with the surface) */
    });
}

struct Popup { Listener commit, destroy; };
static void new_popup(wlr_xdg_popup *p)
{
    wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(p->parent);
    if (!parent || !parent->data) return;
    p->base->data = wlr_scene_xdg_surface_create(static_cast<wlr_scene_tree *>(parent->data), p->base);
    Popup *pp = new Popup();
    pp->commit.connect(&p->base->surface->events.commit, [p](void *) {
        if (p->base->initial_commit) wlr_xdg_surface_schedule_configure(p->base);
    });
    pp->destroy.connect(&p->events.destroy, [pp](void *) { delete pp; });
}

/* layer-shell (fuzzel, ...): above everything, keyboard when it asks */
struct Layer { wlr_layer_surface_v1 *ls; wlr_scene_layer_surface_v1 *sl; Listener commit, map, unmap, destroy; };
static void new_layer(wlr_layer_surface_v1 *ls)
{
    if (!ls->output) ls->output = S.output;
    Layer *l = new Layer();
    l->ls = ls;
    l->sl = wlr_scene_layer_surface_v1_create(S.t_layers, ls);
    l->commit.connect(&ls->surface->events.commit, [l](void *) {
        wlr_box full = { 0, 0, S.W, S.H }, usable = full;
        wlr_scene_layer_surface_v1_configure(l->sl, &full, &usable);
    });
    l->map.connect(&ls->surface->events.map, [l](void *) {
        if (l->ls->current.keyboard_interactive) {
            wlr_keyboard *kb = wlr_seat_get_keyboard(S.seat);
            if (kb) wlr_seat_keyboard_notify_enter(S.seat, l->ls->surface, kb->keycodes,
                                                   kb->num_keycodes, &kb->modifiers);
        }
    });
    l->unmap.connect(&ls->surface->events.unmap, [](void *) { focus(S.active); });
    l->destroy.connect(&ls->events.destroy, [l](void *) { delete l; });
}

/* xdg-decoration: OS/2 frames for everyone */
struct Deco { Listener mode, destroy, commit; };
static void new_deco(wlr_xdg_toplevel_decoration_v1 *d)
{
    Deco *dc = new Deco();
    auto set = [d]() {
        if (d->toplevel->base->initialized)
            wlr_xdg_toplevel_decoration_v1_set_mode(d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    };
    dc->mode.connect(&d->events.request_mode, [set](void *) { set(); });
    dc->commit.connect(&d->toplevel->base->surface->events.commit, [d, dc](void *) {
        if (d->toplevel->base->initialized) {
            wlr_xdg_toplevel_decoration_v1_set_mode(d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
            dc->commit.disconnect();
        }
    });
    dc->destroy.connect(&d->events.destroy, [dc](void *) { delete dc; });
}

/* ── menus ──────────────────────────────────────────────────────────── */
static void close_menu()
{
    if (!S.menu) return;
    S.menu->pic.destroy();
    delete S.menu;
    S.menu = nullptr;
}

static void draw_menu()
{
    Menu *m = S.menu;
    PixBuf *pb = pixbuf_new(m->w, m->h);
    pixman_image_t *im = pb->img;
    fill(im, 0, 0, m->w, m->h, C_FACE);
    bevel(im, 0, 0, m->w, m->h, true, 2);
    for (size_t i = 0; i < m->items.size(); i++) {
        int y = 3 + (int)i * ROW;
        bool h = (int)i == m->hover;
        if (h) fill(im, 3, y, m->w - 6, ROW, C_SEL);
        text_draw(im, TEXT_UI, 12, y + text_ascent(TEXT_UI) + 2, m->items[i].label.c_str(), h ? C_WHITE : C_TEXT, 0);
    }
    m->pic.show(S.t_menu, pb, &m->tag);
    wlr_scene_node_set_position(&m->pic.node->node, m->x, m->y);
}

static void open_menu(int x, int y, std::vector<Menu::Item> items)
{
    close_menu();
    Menu *m = new Menu();
    m->items = std::move(items);
    int w = 0;
    for (auto &it : m->items) w = std::max(w, text_width(TEXT_UI, it.label.c_str()));
    m->w = w + 36;
    m->h = (int)m->items.size() * ROW + 6;
    m->x = std::min(x, S.W - m->w);
    m->y = std::min(y, S.H - m->h);
    S.menu = m;
    draw_menu();
}

static void system_menu(int x, int y)
{
    open_menu(x, y, {
        { "Command Prompt", [] { spawn(terminal()); } },
        { "Programs...", [] { spawn({ sys("fuzzel"), "--config", sys("fuzzel.ini") }); } },
        { "Drives", [] { open_folder("::drives", "Drives"); } },
        { "OS/2 System", [] { open_folder("::system", "OS/2 System"); } },
        { "Window List", [] { toggle_winlist(); } },
        { "Shut Down...", [] { spawn({ sys("os2cmd"), "SHUTDOWN" }); } },
    });
}

static void window_menu(Window *w, int x, int y)
{
    open_menu(x, y, {
        { w->maximized ? "Restore" : "Maximize", [w] { toggle_maximize(w); } },
        { "Hide", [w] { hide(w); } },
        { "Window List", [] { toggle_winlist(); } },
        { "Close", [w] { close_window(w); } },
    });
}

/* ── pointer ────────────────────────────────────────────────────────── */
struct Hit {
    wlr_surface *surface = nullptr;
    double sx = 0, sy = 0;
    Tag *tag = nullptr;
};

static Hit hit_at(double lx, double ly)
{
    Hit h;
    wlr_scene_node *n = wlr_scene_node_at(&S.scene->tree.node, lx, ly, &h.sx, &h.sy);
    if (!n) return h;
    if (n->type == WLR_SCENE_NODE_BUFFER) {
        wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n));
        if (ss) h.surface = ss->surface;
    }
    for (wlr_scene_node *p = n; p; p = p->parent ? &p->parent->node : nullptr)
        if (p->data) { h.tag = static_cast<Tag *>(p->data); break; }
    return h;
}

static void show_arrow()
{
    if (!S.arrow_shown) { wlr_cursor_set_buffer(S.cursor, &S.arrow->base, 0, 0, 1.0f); S.arrow_shown = true; }
}

static int frame_hit(Window *w, int fx, int fy, uint32_t *edges)
{
    int W = w->fw(), H = w->fh();
    *edges = 0;
    if (fx < B || fy < B || fx >= W - B || fy >= H - B) {
        const int C = 16;
        if (fx < C) *edges |= WLR_EDGE_LEFT;
        if (fx >= W - C) *edges |= WLR_EDGE_RIGHT;
        if (fy < C) *edges |= WLR_EDGE_TOP;
        if (fy >= H - C) *edges |= WLR_EDGE_BOTTOM;
        return HIT_BORDER;
    }
    if (fy < B + T) {
        if (fx < B + 20) return HIT_SYSMENU;
        for (int i = 0; i < 3; i++)
            if (fx >= btn_x(w, i) && fx < btn_x(w, i) + BTN_W) return i == 0 ? HIT_CLOSE : i == 1 ? HIT_MAX : HIT_HIDE;
        return HIT_TITLE;
    }
    if (w->kind == Window::FOLDER && fy < B + T + MENUBAR) return HIT_MENUBAR;
    return HIT_CONTENT;
}

static bool double_click(uint32_t time, const void *target, int index)
{
    bool dbl = target == S.last_target && index == S.last_index && time - S.last_click < 450;
    S.last_click = dbl ? 0 : time;
    S.last_target = target;
    S.last_index = index;
    return dbl;
}

static int folder_index_at(Window *w, int cx, int cy)
{
    int ty = cy - MENUBAR - 6;
    if (ty < 0 || cx < 4) return -1;
    int c = (cx - 4) / CELL_W, r = ty / CELL_H + w->scroll;
    if (c >= folder_cols(w)) return -1;
    int i = r * folder_cols(w) + c;
    return i < (int)w->objects.size() ? i : -1;
}

static void content_click(Window *w, int cx, int cy, uint32_t time)
{
    if (w->kind == Window::FOLDER) {
        int i = folder_index_at(w, cx, cy);
        bool dbl = double_click(time, w, i);
        w->sel = i;
        draw_window(w);
        if (dbl && i >= 0) run_object(w->objects[i]);
    } else if (w->kind == Window::WINLIST) {
        auto list = listed_windows();
        int i = (cy - 4 - ROW) / ROW;
        if (cy < 4 + ROW || i >= (int)list.size()) i = -1;
        bool dbl = double_click(time, w, i);
        w->lsel = i;
        draw_window(w);
        if (dbl && i >= 0) focus(list[i]);
    }
}

static void pointer_motion(uint32_t time)
{
    double lx = S.cursor->x, ly = S.cursor->y;
    if (S.mode == Server::MOVE && S.grab) {
        Window *w = S.grab;
        w->x = S.gwx + (int)(lx - S.gx);
        w->y = std::max(BAR - T, S.gwy + (int)(ly - S.gy));
        wlr_scene_node_set_position(&w->tree->node, w->x, w->y);
        return;
    }
    if (S.mode == Server::RESIZE && S.grab) {
        Window *w = S.grab;
        int dx = (int)(lx - S.gx), dy = (int)(ly - S.gy);
        int nx = S.gwx, ny = S.gwy, cw = S.gcw, ch = S.gch;
        if (S.grab_edges & WLR_EDGE_RIGHT) cw = S.gcw + dx;
        if (S.grab_edges & WLR_EDGE_BOTTOM) ch = S.gch + dy;
        if (S.grab_edges & WLR_EDGE_LEFT) { cw = S.gcw - dx; nx = S.gwx + dx; }
        if (S.grab_edges & WLR_EDGE_TOP) { ch = S.gch - dy; ny = S.gwy + dy; }
        if (cw < 160) { if (S.grab_edges & WLR_EDGE_LEFT) nx -= 160 - cw; cw = 160; }
        if (ch < 80) { if (S.grab_edges & WLR_EDGE_TOP) ny -= 80 - ch; ch = 80; }
        w->x = nx; w->y = ny;
        wlr_scene_node_set_position(&w->tree->node, w->x, w->y);
        if (w->top) wlr_xdg_toplevel_set_size(w->top, cw, ch);
        else { w->cw = cw; w->ch = ch; draw_window(w); }
        return;
    }
    if (S.menu) {
        int mx = (int)lx - S.menu->x, my = (int)ly - S.menu->y;
        int h = (mx >= 0 && mx < S.menu->w && my >= 3 && my < S.menu->h - 3) ? (my - 3) / ROW : -1;
        if (h >= (int)S.menu->items.size()) h = -1;
        if (h != S.menu->hover) { S.menu->hover = h; draw_menu(); }
    }
    Hit h = hit_at(lx, ly);
    if (h.surface) {
        S.arrow_shown = false;
        wlr_seat_pointer_notify_enter(S.seat, h.surface, h.sx, h.sy);
        wlr_seat_pointer_notify_motion(S.seat, time, h.sx, h.sy);
    } else {
        wlr_seat_pointer_clear_focus(S.seat);
        show_arrow();
    }
}

static void pointer_button(wlr_pointer_button_event *e)
{
    double lx = S.cursor->x, ly = S.cursor->y;
    if (e->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (S.mode != Server::NONE) {
            Window *w = S.grab;
            S.mode = Server::NONE; S.grab = nullptr;
            if (w && !w->top) draw_window(w);
            return;
        }
        wlr_seat_pointer_notify_button(S.seat, e->time_msec, e->button, e->state);
        return;
    }
    bool right = e->button == 0x111;                    /* BTN_RIGHT */
    if (S.menu) {
        Menu *m = S.menu;
        int mx = (int)lx - m->x, my = (int)ly - m->y;
        if (mx >= 0 && mx < m->w && my >= 0 && my < m->h) {
            int i = (my - 3) / ROW;
            if (i >= 0 && i < (int)m->items.size()) {
                auto run = m->items[i].run;
                close_menu();
                run();
            }
            return;
        }
        close_menu();
    }
    Hit h = hit_at(lx, ly);
    if (h.tag && h.tag->k == Tag::BAR) {
        int i = ((int)lx - 4) / BAR_BTN;
        if (lx >= 4 && i >= 0 && i < (int)(sizeof bar_buttons / sizeof bar_buttons[0])) {
            const BarButton &b = bar_buttons[i];
            if (b.icon == I_SYSTEM) system_menu(4 + i * BAR_BTN, BAR);
            else if (b.action == A_FOLDER) open_folder("::drives", "Drives");
            else run_object({ b.label, b.icon, b.action, "" });
        }
        return;
    }
    if (h.tag && h.tag->k == Tag::WIN) {
        Window *w = h.tag->w;
        if (S.active != w) focus(w);
        if (h.surface) {                                 /* the client's own */
            wlr_seat_pointer_notify_button(S.seat, e->time_msec, e->button, e->state);
            return;
        }
        int fx = (int)lx - w->x, fy = (int)ly - w->y;
        uint32_t edges;
        switch (frame_hit(w, fx, fy, &edges)) {
        case HIT_BORDER:
            S.grab = w; S.mode = Server::RESIZE; S.grab_edges = edges;
            S.gx = lx; S.gy = ly; S.gwx = w->x; S.gwy = w->y; S.gcw = w->cw; S.gch = w->ch;
            break;
        case HIT_TITLE:
            if (right) { window_menu(w, (int)lx, (int)ly); break; }
            if (double_click(e->time_msec, &w->tag, -2)) { toggle_maximize(w); break; }
            S.grab = w; S.mode = Server::MOVE;
            S.gx = lx; S.gy = ly; S.gwx = w->x; S.gwy = w->y;
            break;
        case HIT_SYSMENU:
            if (double_click(e->time_msec, &w->tag, -3)) close_window(w);
            else window_menu(w, w->x + B, w->y + B + T);
            break;
        case HIT_CLOSE: close_window(w); break;
        case HIT_MAX: toggle_maximize(w); break;
        case HIT_HIDE: hide(w); break;
        case HIT_MENUBAR:
            if (fx - B < 60)
                open_menu(w->x + B, w->y + B + T + MENUBAR, {
                    { "Open parent", [w] {
                        std::string p = w->path;
                        if (p.rfind("::", 0) == 0 || p == "/" || p == "/h") { open_folder("::drives", "Drives"); return; }
                        size_t s = p.rfind('/');
                        std::string up = s == 0 ? "/" : p.substr(0, s);
                        std::string nm = up == "/" ? "Drive C" : up == "/h" ? "Drive D" : up.substr(up.rfind('/') + 1);
                        open_folder(up, nm);
                    } },
                    { "Refresh", [w] { w->objects = folder_objects(w->path); w->sel = -1; w->scroll = 0; draw_window(w); } },
                    { "Close", [w] { close_window(w); } },
                });
            break;
        case HIT_CONTENT:
            content_click(w, fx - B, fy - B - T, e->time_msec);
            break;
        }
        return;
    }
    if (h.tag && h.tag->k == Tag::DESK) {
        int i = -1;
        for (size_t k = 0; k < S.desk_pos.size(); k++) {
            int x = S.desk_pos[k].first, y = S.desk_pos[k].second;
            if (lx >= x && lx < x + 56 && ly >= y && ly < y + 54) i = (int)k;
        }
        if (right && i < 0) { system_menu((int)lx, (int)ly); return; }
        bool dbl = double_click(e->time_msec, &S.desk_tag, i);
        if (i != S.desk_sel) { S.desk_sel = i; draw_desktop(); }
        if (dbl && i >= 0) run_object(S.desk_objs[i]);
        return;
    }
    if (h.surface) {                                     /* a layer surface (fuzzel) */
        wlr_seat_pointer_notify_button(S.seat, e->time_msec, e->button, e->state);
        return;
    }
}

static void pointer_axis(wlr_pointer_axis_event *e)
{
    Hit h = hit_at(S.cursor->x, S.cursor->y);
    if (h.surface) {
        wlr_seat_pointer_notify_axis(S.seat, e->time_msec, e->orientation, e->delta,
                                     e->delta_discrete, e->source, e->relative_direction);
        return;
    }
    if (h.tag && h.tag->k == Tag::WIN && h.tag->w->kind == Window::FOLDER) {
        Window *w = h.tag->w;
        int rows = ((int)w->objects.size() + folder_cols(w) - 1) / folder_cols(w);
        int max = std::max(0, rows - folder_rows_visible(w));
        w->scroll = std::clamp(w->scroll + (e->delta > 0 ? 1 : -1), 0, max);
        draw_window(w);
    }
}

/* ── keyboard ───────────────────────────────────────────────────────── */
struct Keyboard { wlr_keyboard *kb; Listener key, mods, destroy; };

static bool binding(uint32_t mods, xkb_keysym_t sym)
{
    bool alt = mods & WLR_MODIFIER_ALT, ctrl = mods & WLR_MODIFIER_CTRL, shift = mods & WLR_MODIFIER_SHIFT;
    if (ctrl && sym == XKB_KEY_Escape) { toggle_winlist(); return true; }
    if (!alt) return false;
    switch (sym) {
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: {
        auto list = listed_windows();
        if (list.empty()) return true;
        focus(list.front());                              /* the one at the back comes forward */
        return true;
    }
    case XKB_KEY_F4: if (S.active) close_window(S.active); return true;
    case XKB_KEY_o: case XKB_KEY_O: spawn(terminal()); return true;
    case XKB_KEY_Return: if (shift) { spawn(terminal()); return true; } return false;
    case XKB_KEY_p: case XKB_KEY_P: spawn({ sys("fuzzel"), "--config", sys("fuzzel.ini") }); return true;
    case XKB_KEY_q: case XKB_KEY_Q: if (shift) { wl_display_terminate(S.display); return true; } return false;
    }
    return false;
}

static void new_keyboard(wlr_input_device *dev)
{
    wlr_keyboard *kb = wlr_keyboard_from_input_device(dev);
    Keyboard *k = new Keyboard();
    k->kb = kb;
    xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
    xkb_keymap *map = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map) map = xkb_keymap_new_from_string(ctx, wlr_os2_fallback_keymap(), XKB_KEYMAP_FORMAT_TEXT_V1,
                                               XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(kb, map);
    xkb_keymap_unref(map);
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(kb, 25, 600);
    k->mods.connect(&kb->events.modifiers, [k](void *) {
        wlr_seat_set_keyboard(S.seat, k->kb);
        wlr_seat_keyboard_notify_modifiers(S.seat, &k->kb->modifiers);
    });
    k->key.connect(&kb->events.key, [k](void *d) {
        auto *e = static_cast<wlr_keyboard_key_event *>(d);
        if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            const xkb_keysym_t *syms;
            int n = xkb_state_key_get_syms(k->kb->xkb_state, e->keycode + 8, &syms);
            uint32_t mods = wlr_keyboard_get_modifiers(k->kb);
            for (int i = 0; i < n; i++) if (binding(mods, syms[i])) return;
            if (S.menu && syms && n && syms[0] == XKB_KEY_Escape) { close_menu(); return; }
        }
        wlr_seat_set_keyboard(S.seat, k->kb);
        wlr_seat_keyboard_notify_key(S.seat, e->time_msec, e->keycode, e->state);
    });
    k->destroy.connect(&dev->events.destroy, [k](void *) { delete k; });
    wlr_seat_set_keyboard(S.seat, kb);
}

/* ── output ─────────────────────────────────────────────────────────── */
struct Output { wlr_output *o; Listener frame, state, destroy; };

static void new_output(wlr_output *o)
{
    wlr_output_init_render(o, S.allocator, S.renderer);
    wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    if (wlr_output_mode *m = wlr_output_preferred_mode(o)) wlr_output_state_set_mode(&st, m);
    wlr_output_commit_state(o, &st);
    wlr_output_state_finish(&st);
    Output *out = new Output();
    out->o = o;
    out->frame.connect(&o->events.frame, [o](void *) {
        wlr_scene_output *so = wlr_scene_get_scene_output(S.scene, o);
        wlr_scene_output_commit(so, nullptr);
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(so, &now);
    });
    out->state.connect(&o->events.request_state, [o](void *d) {
        wlr_output_commit_state(o, static_cast<wlr_output_event_request_state *>(d)->state);
    });
    out->destroy.connect(&o->events.destroy, [out](void *) { delete out; });
    wlr_output_layout_output *lo = wlr_output_layout_add_auto(S.layout, o);
    wlr_scene_output *so = wlr_scene_output_create(S.scene, o);
    wlr_scene_output_layout_add_output(S.scene_layout, lo, so);
    if (!S.output) {
        S.output = o;
        S.W = o->width; S.H = o->height;
        draw_desktop();
        draw_bar();
    }
}

/* ── the cursor's picture ───────────────────────────────────────────── */
static PixBuf *make_arrow()
{
    static const char *rows[] = {
        "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
        "X........X", "X.....XXXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X",
        "      X..X", "      XXX", nullptr,
    };
    PixBuf *p = pixbuf_new(12, 19);
    for (int y = 0; rows[y]; y++)
        for (int x = 0; rows[y][x]; x++)
            p->px[y * 12 + x] = rows[y][x] == 'X' ? C_DARK : rows[y][x] == '.' ? C_WHITE : 0;
    return p;
}

/* ── main ───────────────────────────────────────────────────────────── */
/* children: nobody waits for them, so they're reaped here */
static void reap(int)
{
    int e = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    errno = e;
}

int main(int argc, char **argv)
{
    wlr_log_init(WLR_ERROR, nullptr);
    if (text_init() != 0) { fprintf(stderr, "wps: no font (fonts.conf, DejaVu Sans)\n"); return 1; }
    struct sigaction sa = {};
    sa.sa_handler = reap;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, nullptr);

    S.display = wl_display_create();
    wl_event_loop *loop = wl_display_get_event_loop(S.display);
    S.backend = wlr_os2_backend_create(loop, nullptr, nullptr, nullptr);
    if (!S.backend) S.backend = wlr_backend_autocreate(loop, nullptr);
    if (!S.backend) { fprintf(stderr, "wps: no backend\n"); return 1; }
    S.renderer = wlr_renderer_autocreate(S.backend);
    wlr_renderer_init_wl_display(S.renderer, S.display);
    S.allocator = wlr_allocator_autocreate(S.backend, S.renderer);
    wlr_compositor_create(S.display, 5, S.renderer);
    wlr_subcompositor_create(S.display);
    wlr_data_device_manager_create(S.display);

    S.layout = wlr_output_layout_create(S.display);
    S.scene = wlr_scene_create();
    S.scene_layout = wlr_scene_attach_output_layout(S.scene, S.layout);
    S.t_desk = wlr_scene_tree_create(&S.scene->tree);
    S.t_wins = wlr_scene_tree_create(&S.scene->tree);
    S.t_bar = wlr_scene_tree_create(&S.scene->tree);
    S.t_layers = wlr_scene_tree_create(&S.scene->tree);
    S.t_menu = wlr_scene_tree_create(&S.scene->tree);

    S.desk_objs = {
        { "OS/2 System", I_SYSTEM, A_FOLDER, "::system" },
        { "Drives", I_DRIVES, A_FOLDER, "::drives" },
        { "Command Prompt", I_PROMPT, A_PROMPT, "" },
        { "Drive D", I_DRIVE, A_FOLDER, "/h" },
        { "Programs", I_PROGRAMS, A_PROGRAMS, "" },
        { "Window List", I_WINLIST, A_WINLIST, "" },
        { "Shut Down", I_SHUTDOWN, A_SHUTDOWN, "" },
    };

    S.new_output.connect(&S.backend->events.new_output, [](void *d) { new_output(static_cast<wlr_output *>(d)); });

    S.xdg = wlr_xdg_shell_create(S.display, 3);
    S.new_toplevel.connect(&S.xdg->events.new_toplevel, [](void *d) { new_toplevel(static_cast<wlr_xdg_toplevel *>(d)); });
    S.new_popup.connect(&S.xdg->events.new_popup, [](void *d) { new_popup(static_cast<wlr_xdg_popup *>(d)); });
    S.layers = wlr_layer_shell_v1_create(S.display, 4);
    S.new_layer.connect(&S.layers->events.new_surface, [](void *d) { new_layer(static_cast<wlr_layer_surface_v1 *>(d)); });
    S.decos = wlr_xdg_decoration_manager_v1_create(S.display);
    S.new_deco.connect(&S.decos->events.new_toplevel_decoration,
                       [](void *d) { new_deco(static_cast<wlr_xdg_toplevel_decoration_v1 *>(d)); });

    S.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(S.cursor, S.layout);
    S.arrow = make_arrow();
    S.cursor_motion.connect(&S.cursor->events.motion, [](void *d) {
        auto *e = static_cast<wlr_pointer_motion_event *>(d);
        wlr_cursor_move(S.cursor, &e->pointer->base, e->delta_x, e->delta_y);
        pointer_motion(e->time_msec);
    });
    S.cursor_motion_abs.connect(&S.cursor->events.motion_absolute, [](void *d) {
        auto *e = static_cast<wlr_pointer_motion_absolute_event *>(d);
        wlr_cursor_warp_absolute(S.cursor, &e->pointer->base, e->x, e->y);
        pointer_motion(e->time_msec);
    });
    S.cursor_button.connect(&S.cursor->events.button, [](void *d) { pointer_button(static_cast<wlr_pointer_button_event *>(d)); });
    S.cursor_axis.connect(&S.cursor->events.axis, [](void *d) { pointer_axis(static_cast<wlr_pointer_axis_event *>(d)); });
    S.cursor_frame.connect(&S.cursor->events.frame, [](void *) { wlr_seat_pointer_notify_frame(S.seat); });

    S.seat = wlr_seat_create(S.display, "seat0");
    S.new_input.connect(&S.backend->events.new_input, [](void *d) {
        auto *dev = static_cast<wlr_input_device *>(d);
        if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) new_keyboard(dev);
        else if (dev->type == WLR_INPUT_DEVICE_POINTER) wlr_cursor_attach_input_device(S.cursor, dev);
        wlr_seat_set_capabilities(S.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    });
    S.request_cursor.connect(&S.seat->events.request_set_cursor, [](void *d) {
        auto *e = static_cast<wlr_seat_pointer_request_set_cursor_event *>(d);
        if (S.seat->pointer_state.focused_client == e->seat_client) {
            wlr_cursor_set_surface(S.cursor, e->surface, e->hotspot_x, e->hotspot_y);
            S.arrow_shown = false;
        }
    });
    S.request_selection.connect(&S.seat->events.request_set_selection, [](void *d) {
        auto *e = static_cast<wlr_seat_request_set_selection_event *>(d);
        wlr_seat_set_selection(S.seat, e->source, e->serial);
    });

    const char *socket = wl_display_add_socket_auto(S.display);
    if (!socket || !wlr_backend_start(S.backend)) { fprintf(stderr, "wps: can't start\n"); return 1; }
    setenv("WAYLAND_DISPLAY", socket, 1);
    show_arrow();

    /* the clock: the timer finds itself through its data */
    static wl_event_source *tick;
    tick = wl_event_loop_add_timer(loop, [](void *) {
        draw_bar();
        wl_event_source_timer_update(tick, 1000);
        return 0;
    }, nullptr);
    wl_event_source_timer_update(tick, 1000);

    for (int i = 1; i < argc; i++)                       /* wps [-s command]: run it */
        if (!strcmp(argv[i], "-s") && i + 1 < argc) spawn({ sys("busybox"), "sh", "-c", argv[++i] });

    wl_display_run(S.display);
    wl_display_destroy_clients(S.display);
    wlr_scene_node_destroy(&S.scene->tree.node);
    wlr_cursor_destroy(S.cursor);
    wlr_allocator_destroy(S.allocator);
    wlr_renderer_destroy(S.renderer);
    wlr_backend_destroy(S.backend);
    wl_display_destroy(S.display);
    return 0;
}
