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
#include <map>
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
            I_WINLIST, I_SHUTDOWN, I_DRIVES, I_SHREDDER, I_HELP, I_COUNT };

static void disc(pixman_image_t *im, int cx, int cy, int r, uint32_t c)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) w++;
        fill(im, cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

/* a rectangle with a black edge */
static void boxed(pixman_image_t *im, int x, int y, int w, int h, uint32_t inside)
{
    fill(im, x, y, w, h, C_DARK);
    fill(im, x + 1, y + 1, w - 2, h - 2, inside);
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
        boxed(im, x + S(7), y + S(3), S(18), S(26), C_WHITE);
        fill(im, x + S(7) + S(18) - 1 - S(5), y + S(3) + 1, S(5), S(5), 0xFFC0C0C0);   /* folded corner */
        if (i == I_TEXT)
            for (int l = 0; l < 5; l++) fill(im, x + S(10), y + S(11 + 3 * l), S(12), 1, C_SHADOW);
        break;
    case I_PROGRAM:
        boxed(im, x + S(3), y + S(5), S(26), S(22), C_FACE);
        fill(im, x + S(3) + 2, y + S(5) + 2, S(26) - 4, std::max(2, S(4)), C_TITLE);
        fill(im, x + S(3) + 2, y + S(5) + 3 + std::max(2, S(4)), S(26) - 4, S(22) - 5 - std::max(2, S(4)), C_WHITE);
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
    case I_HELP: {                                                          /* a book, a question mark */
        boxed(im, x + S(5), y + S(3), S(22), S(26), 0xFF2050B0);
        fill(im, x + S(5) + 1, y + S(3) + 1, std::max(1, S(3)), S(26) - 2, 0xFF102C70);   /* the spine */
        int q = std::max(1, S(3)), cx = x + S(17), cy = y + S(8);
        fill(im, cx - S(4), cy, S(8), q, C_WHITE);                          /* the "?" */
        fill(im, cx + S(4) - q, cy, q, S(6), C_WHITE);
        fill(im, cx - 1, cy + S(6) - q, S(4) + 1, q, C_WHITE);
        fill(im, cx - 1, cy + S(6) - q, q, S(4), C_WHITE);
        fill(im, cx - 1, cy + S(12), q, q, C_WHITE);
        break;
    }
    case I_SHREDDER:
        boxed(im, x + S(4), y + S(14), S(24), S(16), C_FACE);                 /* the bin */
        fill(im, x + S(4) + 1, y + S(14) + 1, S(24) - 2, 1, C_LIGHT);
        fill(im, x + S(7), y + S(17), S(18), std::max(1, S(2)), C_DARK);       /* the slot */
        boxed(im, x + S(9), y + S(2), S(14), S(15), C_WHITE);                 /* the paper going in */
        for (int l = 0; l < 3; l++) fill(im, x + S(11), y + S(5 + 3 * l), S(10), 1, C_SHADOW);
        for (int k = 0; k < 4; k++) fill(im, x + S(8 + 5 * k), y + S(22), std::max(1, S(2)), S(6), C_SHADOW);  /* strips */
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
enum Action { A_NONE, A_HELP, A_APP, A_FOLDER, A_RUN, A_OS2RUN, A_EDIT, A_SHREDDER, A_SCRIPT, A_PROMPT, A_PROGRAMS, A_WINLIST,
              A_SHUTDOWN };
struct Object {
    std::string label;
    int icon;
    Action action;
    std::string path;                /* A_FOLDER: a directory or "::system"/"::drives" */
    long long size = -1;             /* files: for the Details view */
    std::string exec;                /* A_APP: its command line (a .desktop entry's Exec) */
    bool term = false;               /* ... in a terminal */
    bool shadow = false;             /* a shadow: a link to the object at path */
    time_t mtime = 0;
};

/* a folder in a Tree view */
struct Node {
    std::string path, name;
    int depth;
    bool open, kids;
};

/* ── windows ────────────────────────────────────────────────────────── */
struct Window;
struct Tag { enum K { DESK, BAR, WIN, MENU } k; Window *w; };

struct Window {
    enum Kind { XDG, FOLDER, WINLIST, INFO, NOTEBOOK, HELP } kind;
    std::vector<std::pair<std::string, std::string>> topics;   /* HELP: title, file */
    int tab = 0;                                  /* NOTEBOOK: the page shown */
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
    enum View { ICON, TREE, DETAILS } view = ICON;
    std::vector<Object> objects;                  /* ICON, DETAILS */
    std::vector<Node> nodes;                      /* TREE */
    int sel = -1, scroll = 0;                     /* scroll: in rows */
    bool all = false;                             /* Edit > Select all */
    /* INFO (Properties, About, dialogs) */
    std::string info_title;
    std::vector<std::string> lines;
    struct Button { std::string label; std::function<void(Window *)> run; };
    std::vector<Button> buttons;                  /* the first: Enter */
    bool editing = false;                         /* an entry field */
    bool edit_all = false;                        /* its text selected: typing replaces it */
    std::string edit;
    /* WINLIST */
    int lsel = -1;

    std::string titleText() const;
    int fw() const { return cw + 2 * B; }
    int fh() const { return ch + 2 * B + T; }
};

struct Menu {
    struct Item { std::string label; std::function<void()> run; bool enabled = true; bool checked = false; };
    std::vector<Item> items;
    int x = 0, y = 0, w = 0, h = 0, hover = -1;
    Window *owner = nullptr;                      /* a folder's menu bar menu: */
    int bar = -1;                                 /* which one */
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
    std::map<std::string, std::pair<int, int>> desk_place;   /* ... moved there by hand */
    std::vector<Object> desk_builtin;            /* the desktop's own objects */
    std::vector<Object> shadows;                 /* ... and the shadows put on it */
    std::vector<Object> startup;                 /* the Startup folder's shadows */
    bool restoring = false;                      /* (no saving while restoring) */
    uint32_t desk_color = C_DESK;                /* the Settings notebook's: the background, */
    bool pattern = true;                         /* ... with the pattern, */
    int scheme = 0;                              /* ... and the title bars' colours */
    int desk_sel = -1;

    std::vector<Window *> windows;               /* back to front */
    Window *active = nullptr;
    Window *winlist = nullptr;
    Menu *menu = nullptr;
    PixBuf *arrow = nullptr;
    bool arrow_shown = false;
    int cascade = 0;

    /* pointer grabs */
    enum Mode { NONE, MOVE, RESIZE, THUMB } mode = NONE;
    Window *grab = nullptr;                      /* (also: a window going away ends a drag) */
    uint32_t grab_edges = 0;
    double gx, gy;
    int gwx, gwy, gcw, gch;

    /* dragging an object */
    struct Drag {
        bool armed = false, active = false;
        Window *src = nullptr;
        int desk = -1;                           /* a desktop object being moved, or -1 */
        double dx = 0, dy = 0;                   /* (where it was taken, in it) */
        Object obj;
        double x0 = 0, y0 = 0;
        Picture pic;
    } drag;

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

static void open_folder(const std::string &path, const std::string &name, int view = 0);
static void close_menu();
static std::vector<Object> program_objects();
static void run_app(const Object &o);
static void save_state();
static void open_help(int topic);
static void open_desktop_settings();
static void rebuild_desktop();
static void remove_shadow(const std::string &where, const std::string &path);
static bool classify(const std::string &p, const std::string &label, Object *o);
static std::string desk_key(const Object &o);
static bool resolve_shadow(const std::string &path, const std::string &label, Object *sh);
static void toggle_winlist();
static void focus(Window *w);

static std::string dos_path(const std::string &p);
static void open_info(const std::string &title, std::vector<std::string> lines);
static void run_object(const Object &o)
{
    switch (o.action) {
    case A_FOLDER:   open_folder(o.path, o.label); break;
    case A_PROMPT:   spawn(terminal()); break;
    case A_PROGRAMS: spawn({ sys("fuzzel"), "--config", sys("fuzzel.ini") }); break;
    case A_APP:      run_app(o); break;
    case A_HELP:     open_help(0); break;
    case A_WINLIST:  toggle_winlist(); break;
    case A_SHUTDOWN: spawn({ sys("os2cmd"), "SHUTDOWN" }); break;
    case A_RUN:    { auto v = terminal(true); v.push_back(o.path); spawn(v); break; }
    case A_OS2RUN: { auto v = terminal(true); v.push_back(sys("os2cmd")); v.push_back("RUN");
                     v.push_back(dos_path(o.path)); spawn(v); break; }   /* an OS/2 program: a process */
    case A_SHREDDER: open_info("Shredder", { "Shredder", "Drag an object here to delete it." }); break;     /* (the LX loader: the console's) */
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

/* what the object at path p is (named label): its icon, what opening it does */
static bool classify(const std::string &p, const std::string &label, Object *o)
{
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {
        int icon = p == "/" || p == "/h" ? I_DRIVE : I_FOLDER;
        *o = { label, icon, A_FOLDER, p };
        o->mtime = st.st_mtime;
        return true;
    }
    char head[4] = { 0 };
    int fd = open(p.c_str(), O_RDONLY);
    if (fd >= 0) { if (read(fd, head, 4) < 0) head[0] = 0; close(fd); }
    std::string n = p.substr(p.rfind('/') + 1);
    auto ends = [&](const char *x) {
        size_t l = strlen(x);
        return n.size() > l && strcasecmp(n.c_str() + n.size() - l, x) == 0;
    };
    if (!memcmp(head, "\x7f" "ELF", 4)) *o = { label, I_PROGRAM, A_RUN, p };
    else if (!memcmp(head, "MZ", 2) || !memcmp(head, "LX", 2))
        *o = { label, I_PROGRAM, A_OS2RUN, p };                    /* an OS/2 program: a process */
    else if ((head[0] == '#' && head[1] == '!') || ends(".sh")) *o = { label, I_PROGRAM, A_SCRIPT, p };
    else if (ends(".txt") || ends(".c") || ends(".h") || ends(".cpp") || ends(".s") || ends(".md") ||
             ends(".ini") || ends(".sys") || ends("makefile"))
        *o = { label, I_TEXT, A_EDIT, p };
    else *o = { label, I_FILE, A_EDIT, p };
    o->size = st.st_size;
    o->mtime = st.st_mtime;
    return true;
}

static std::vector<Object> folder_objects(const std::string &path)
{
    std::vector<Object> v;
    if (path == "::system") {
        v.push_back({ "Drives", I_DRIVES, A_FOLDER, "::drives" });
        v.push_back({ "Command Prompt", I_PROMPT, A_PROMPT, "" });
        v.push_back({ "Programs", I_PROGRAMS, A_FOLDER, "::programs" });
        v.push_back({ "Window List", I_WINLIST, A_WINLIST, "" });
        v.push_back({ "Startup", I_FOLDER, A_FOLDER, "::startup" });
        v.push_back({ "Assistance Center", I_HELP, A_HELP, "" });
        v.push_back({ "Shut Down", I_SHUTDOWN, A_SHUTDOWN, "" });
        return v;
    }
    if (path == "::programs") return program_objects();
    if (path == "::startup") return S.startup;
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
        Object o;
        if (!classify(p, n, &o)) continue;
        (o.action == A_FOLDER ? dirs : files).push_back(o);
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
    if (kind == FOLDER) return name + (view == TREE ? " - Tree View" : view == DETAILS ? " - Details View" : " - Icon View");
    if (kind == WINLIST) return "Window List";
    if (kind == INFO || kind == NOTEBOOK || kind == HELP) return info_title;
    if (top && top->title && *top->title) return top->title;
    if (top && top->app_id && *top->app_id) return top->app_id;
    return "(untitled)";
}

/* ── drawing the desktop and the WarpCenter ─────────────────────────── */
static void draw_label(pixman_image_t *im, int cx, int y, const std::string &s, bool sel, bool on_desk,
                       bool shadow = false)
{
    int w = std::min(text_width(TEXT_SMALL, s.c_str()), 86), h = text_height(TEXT_SMALL);
    int x = cx - w / 2;
    if (sel) fill(im, x - 2, y, w + 4, h + 2, C_SEL);
    if (on_desk && !sel) text_draw(im, TEXT_SMALL, x + 1, y + 1 + text_ascent(TEXT_SMALL), s.c_str(), C_DARK, x + 87);
    uint32_t c = sel ? C_WHITE : on_desk ? (shadow ? 0xFFB8E0FF : C_WHITE) : (shadow ? 0xFF1C3CB4 : C_TEXT);
    text_draw(im, TEXT_SMALL, x, y + text_ascent(TEXT_SMALL), s.c_str(), c, x + 86);
}

/* ── the desktop's Settings notebook: Background, Window ──────────────── */
static const uint32_t palette[] = {
    0xFF2C4DA3, 0xFF000080, 0xFF6E9ED6, 0xFF006A6A, 0xFF2E6B3A, 0xFF45556B,
    0xFF4B2E83, 0xFF6E1E1E, 0xFF6B5432, 0xFF808080, 0xFF000000, 0xFFD8D8D8,
};
static const int NPAL = sizeof palette / sizeof palette[0];
struct Scheme { const char *name; uint32_t t1, t2; };
static const Scheme schemes[] = {
    { "Warp 4", 0xFF1C3C9C, 0xFF5878C8 }, { "Warp 3", 0xFF000080, 0xFF000080 },
    { "Teal", 0xFF005A5A, 0xFF3A9A9A }, { "Graphite", 0xFF303030, 0xFF8A8A8A },
    { "Plum", 0xFF4A1E5E, 0xFF9A6AB0 },
};
static const int NSCHEMES = sizeof schemes / sizeof schemes[0];
static const char *nb_tabs[] = { "Background", "Window" };
static const int NB_TABS = 2, NB_TAB_W = 96, NB_TAB_H = 26;

/* a desktop object's name in WPS.INI: a shadow's is its original's path */
static std::string desk_key(const Object &o) { return o.shadow ? "@" + o.path : o.label; }

static uint32_t darker(uint32_t c, int pct);
static void draw_desktop()
{
    PixBuf *pb = pixbuf_new(S.W, S.H);
    pixman_image_t *im = pb->img;
    fill(im, 0, 0, S.W, S.H, S.desk_color);
    uint32_t c2 = darker(S.desk_color, 14);
    for (int y = 0; y < S.H; y += 2)                      /* a quiet texture */
        for (int x = (y / 2) % 4; S.pattern && x < S.W; x += 4) pb->px[y * S.W + x] = c2;
    S.desk_pos.clear();
    for (size_t i = 0; i < S.desk_objs.size(); i++) {
        int x, y;
        auto pl = S.desk_place.find(desk_key(S.desk_objs[i]));
        if (pl != S.desk_place.end()) {
            x = std::clamp(pl->second.first, 0, S.W - 56);
            y = std::clamp(pl->second.second, BAR, S.H - 54);
        }
        else if (S.desk_objs[i].action == A_SHREDDER) { x = S.W - 70; y = S.H - 80; }
        else if (S.desk_objs[i].action == A_SHUTDOWN) { x = S.W - 140; y = S.H - 80; }
        else if (S.desk_objs[i].shadow) {
            int k = (int)(i - S.desk_builtin.size());
            x = 90 + 72 * (k / 8); y = BAR + 16 + (k % 8) * 72;
        }
        else { x = 18; y = BAR + 16 + (int)i * 72; }
        S.desk_pos.push_back({ x, y });
        draw_icon(im, S.desk_objs[i].icon, x + 11, y, 32);
        draw_label(im, x + 27, y + 36, S.desk_objs[i].label, (int)i == S.desk_sel, true, S.desk_objs[i].shadow);
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
    if (w->kind == Window::FOLDER) return w->path == "::programs" ? I_PROGRAMS : w->path == "::drives" ? I_DRIVES :
                                          w->path == "::startup" ? I_FOLDER :
                                          w->path.rfind("::", 0) == 0 ? I_SYSTEM : (w->path == "/" || w->path == "/h") ? I_DRIVE : I_FOLDER;
    if (w->kind == Window::WINLIST) return I_WINLIST;
    if (w->kind == Window::NOTEBOOK) return I_SYSTEM;
    if (w->kind == Window::HELP) return I_HELP;
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
static void draw_info_content(Window *w, pixman_image_t *im, int ox, int oy);
static void draw_notebook_content(Window *w, pixman_image_t *im, int ox, int oy);
static void draw_help_content(Window *w, pixman_image_t *im, int ox, int oy);

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
    if (on) gradient_h(im, B, B, W - 2 * B, T - 1, schemes[S.scheme].t1, schemes[S.scheme].t2);
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
    else if (w->kind == Window::INFO) draw_info_content(w, im, B, B + T);
    else if (w->kind == Window::NOTEBOOK) draw_notebook_content(w, im, B, B + T);
    else if (w->kind == Window::HELP) draw_help_content(w, im, B, B + T);
    else fill(im, B, B + T, w->cw, w->ch, C_WHITE);       /* under the client */
    w->frame.show(w->tree, pb, &w->tag);
    wlr_scene_node_lower_to_bottom(&w->frame.node->node);
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

/* ── folders: a menu bar, then the view (Icon, Tree or Details) and a
 *    scroll bar; coordinates here are inside the window's content ───── */
static const int CELL_W = 88, CELL_H = 66, SB = 16, TROW = 20;
static const char *menubar_labels[] = { "Folder", "Edit", "View", "Selected", "Help" };
static const int MENUBAR_N = 5;

static int menubar_x(int i)
{
    int x = 8;
    for (int k = 0; k < i; k++) x += text_width(TEXT_UI, menubar_labels[k]) + 18;
    return x;
}

static int menubar_at(int cx)
{
    for (int i = 0; i < MENUBAR_N; i++) {
        int x = menubar_x(i);
        if (cx >= x - 7 && cx < x + text_width(TEXT_UI, menubar_labels[i]) + 9) return i;
    }
    return -1;
}

static int view_w(const Window *w) { return w->cw - SB; }
static int view_h(const Window *w) { return w->ch - MENUBAR; }
static int folder_cols(const Window *w) { return std::max(1, (view_w(w) - 8) / CELL_W); }
static int row_h(const Window *w) { return w->view == Window::ICON ? CELL_H : TROW; }
static int head_h(const Window *w) { return w->view == Window::DETAILS ? TROW : 0; }
static int item_count(const Window *w) { return (int)(w->view == Window::TREE ? w->nodes.size() : w->objects.size()); }
static int item_row(const Window *w, int i) { return w->view == Window::ICON ? i / folder_cols(w) : i; }
static int rows_total(const Window *w)
{
    int n = item_count(w);
    return w->view == Window::ICON ? (n + folder_cols(w) - 1) / folder_cols(w) : n;
}
static int rows_visible(const Window *w) { return std::max(1, (view_h(w) - head_h(w) - 6) / row_h(w)); }
static int scroll_max(const Window *w) { return std::max(0, rows_total(w) - rows_visible(w)); }
static void clamp_scroll(Window *w) { w->scroll = std::clamp(w->scroll, 0, scroll_max(w)); }

static void ensure_visible(Window *w, int i)
{
    if (i < 0) return;
    int r = item_row(w, i);
    if (r < w->scroll) w->scroll = r;
    if (r >= w->scroll + rows_visible(w)) w->scroll = r - rows_visible(w) + 1;
    clamp_scroll(w);
}

/* the scroll bar's trough and thumb (content y) */
static void sb_geometry(const Window *w, int *ty, int *th, int *my, int *mh)
{
    *ty = MENUBAR + SB;
    *th = std::max(1, view_h(w) - 2 * SB);
    int tot = rows_total(w), vis = rows_visible(w);
    if (tot <= vis) { *my = *ty; *mh = *th; return; }
    *mh = std::max(12, *th * vis / tot);
    *my = *ty + (*th - *mh) * w->scroll / (tot - vis);
}

static void arrow(pixman_image_t *im, int cx, int cy, bool up, uint32_t c)
{
    for (int k = 0; k < 4; k++) fill(im, cx - k, up ? cy - 2 + k : cy + 1 - k, 2 * k + 1, 1, c);
}

static void draw_scrollbar(Window *w, pixman_image_t *im, int ox, int oy)
{
    int x = ox + view_w(w), y = oy + MENUBAR, h = view_h(w);
    bool live = rows_total(w) > rows_visible(w);
    uint32_t ac = live ? C_DARK : C_SHADOW;
    fill(im, x, y, SB, h, 0xFFE4E4E4);
    fill(im, x, y, SB, SB, C_FACE);
    bevel(im, x, y, SB, SB, true, 1);
    arrow(im, x + SB / 2, y + SB / 2, true, ac);
    fill(im, x, y + h - SB, SB, SB, C_FACE);
    bevel(im, x, y + h - SB, SB, SB, true, 1);
    arrow(im, x + SB / 2, y + h - SB / 2, false, ac);
    if (live) {
        int ty, th, my, mh;
        sb_geometry(w, &ty, &th, &my, &mh);
        fill(im, x, oy + my, SB, mh, C_FACE);
        bevel(im, x, oy + my, SB, mh, true, 1);
    }
}

static void draw_menubar(Window *w, pixman_image_t *im, int ox, int oy)
{
    fill(im, ox, oy, w->cw, MENUBAR, C_FACE);
    int open = S.menu && S.menu->owner == w ? S.menu->bar : -1;
    for (int i = 0; i < MENUBAR_N; i++) {
        int x = ox + menubar_x(i), tw = text_width(TEXT_UI, menubar_labels[i]);
        if (i == open) fill(im, x - 6, oy + 1, tw + 12, MENUBAR - 3, C_SEL);
        text_draw(im, TEXT_UI, x, oy + MENUBAR / 2 + text_ascent(TEXT_UI) / 2 - 1, menubar_labels[i],
                  i == open ? C_WHITE : C_TEXT, 0);
    }
    fill(im, ox, oy + MENUBAR - 1, w->cw, 1, C_SHADOW);
}

static bool selected(const Window *w, int i) { return w->all || i == w->sel; }

static void draw_icon_view(Window *w, pixman_image_t *im, int ox, int oy)
{
    int cols = folder_cols(w);
    for (int i = 0; i < (int)w->objects.size(); i++) {
        int r = i / cols - w->scroll, c = i % cols;
        if (r < 0) continue;
        int x = ox + 4 + c * CELL_W, y = oy + 6 + r * CELL_H;
        if (y > oy + view_h(w)) break;
        draw_icon(im, w->objects[i].icon, x + (CELL_W - 32) / 2, y, 32);
        draw_label(im, x + CELL_W / 2, y + 36, w->objects[i].label, selected(w, i), false, w->objects[i].shadow);
    }
}

/* the tree: a +/- box for folders with folders in them, dotted lines */
static int tree_x(int depth) { return 8 + depth * 20; }
static bool later_sibling(const Window *w, int i)
{
    for (size_t j = i + 1; j < w->nodes.size(); j++) {
        if (w->nodes[j].depth < w->nodes[i].depth) return false;
        if (w->nodes[j].depth == w->nodes[i].depth) return true;
    }
    return false;
}
static void dots_h(pixman_image_t *im, int x0, int x1, int y) { for (int x = x0; x < x1; x += 2) fill(im, x, y, 1, 1, C_SHADOW); }
static void dots_v(pixman_image_t *im, int x, int y0, int y1) { for (int y = y0 + (y0 & 1); y < y1; y += 2) fill(im, x, y, 1, 1, C_SHADOW); }

static void draw_tree_view(Window *w, pixman_image_t *im, int ox, int oy)
{
    for (int i = w->scroll; i < (int)w->nodes.size(); i++) {
        const Node &n = w->nodes[i];
        int y = oy + 3 + (i - w->scroll) * TROW;
        if (y > oy + view_h(w)) break;
        int mid = y + TROW / 2, bx = ox + tree_x(n.depth);
        if (n.depth > 0) {
            int px = ox + tree_x(n.depth - 1) + 4;               /* the parent's column */
            dots_v(im, px, y, later_sibling(w, i) ? y + TROW : mid);
            dots_h(im, px, bx + 12, mid);
            for (int a = i - 1, d = n.depth - 1; a >= 0 && d > 0; a--)   /* the ancestors' columns */
                if (w->nodes[a].depth == d) {
                    if (later_sibling(w, a)) dots_v(im, ox + tree_x(d - 1) + 4, y, y + TROW);
                    d--;
                }
        }
        if (n.kids) {
            fill(im, bx, mid - 4, 9, 9, C_DARK);
            fill(im, bx + 1, mid - 3, 7, 7, C_WHITE);
            fill(im, bx + 2, mid, 5, 1, C_DARK);
            if (!n.open) fill(im, bx + 4, mid - 2, 1, 5, C_DARK);
        }
        int icon = n.path.rfind("::", 0) == 0 ? (n.path == "::drives" ? I_DRIVES : n.path == "::programs" ? I_PROGRAMS : I_SYSTEM)
                 : (n.path == "/" || n.path == "/h") ? I_DRIVE : I_FOLDER;
        draw_icon(im, icon, bx + 14, mid - 8, 16);
        int tx = bx + 34, tw = text_width(TEXT_UI, n.name.c_str());
        bool sel = i == w->sel;
        if (sel) fill(im, tx - 2, y + 1, tw + 4, TROW - 2, C_SEL);
        text_draw(im, TEXT_UI, tx, mid + text_ascent(TEXT_UI) / 2 - 1, n.name.c_str(), sel ? C_WHITE : C_TEXT,
                  ox + view_w(w) - 2);
    }
}

/* the Details view's columns: icon, title, size, last write */
static void details_cols(const Window *w, int *title, int *size, int *date)
{
    *date = std::max(150, view_w(w) - 112);
    *size = std::max(110, *date - 86);
    *title = 30;
}

static std::string when(time_t t)
{
    if (!t) return "";
    struct tm tm;
    localtime_r(&t, &tm);
    char b[32];
    strftime(b, sizeof b, "%m-%d-%y  %H:%M", &tm);
    return b;
}

static std::string bytes(long long n)
{
    if (n < 0) return "";
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

static void draw_details_view(Window *w, pixman_image_t *im, int ox, int oy)
{
    int tc, sc, dc;
    details_cols(w, &tc, &sc, &dc);
    int base = text_ascent(TEXT_UI) / 2 - 1 + TROW / 2;
    struct { int x, w; const char *t; } head[] = {
        { 0, tc, "Icon" }, { tc, sc - tc, "Title" }, { sc, dc - sc, "Size" }, { dc, view_w(w) - dc, "Last write" } };
    for (auto &h : head) {
        fill(im, ox + h.x, oy, h.w, TROW, C_FACE);
        bevel(im, ox + h.x, oy, h.w, TROW, true, 1);
        text_draw(im, TEXT_UI, ox + h.x + 5, oy + base, h.t, C_TEXT, ox + h.x + h.w - 2);
    }
    for (int i = w->scroll; i < (int)w->objects.size(); i++) {
        const Object &o = w->objects[i];
        int y = oy + TROW + 1 + (i - w->scroll) * TROW;
        if (y > oy + view_h(w)) break;
        bool sel = selected(w, i);
        if (sel) fill(im, ox + tc, y, view_w(w) - tc, TROW, C_SEL);
        uint32_t c = sel ? C_WHITE : C_TEXT;
        draw_icon(im, o.icon, ox + 7, y + 2, 16);
        text_draw(im, TEXT_UI, ox + tc + 5, y + base, o.label.c_str(), c, ox + sc - 4);
        std::string sz = bytes(o.size), dt = when(o.mtime);
        text_draw(im, TEXT_UI, ox + dc - 6 - text_width(TEXT_UI, sz.c_str()), y + base, sz.c_str(), c, 0);
        text_draw(im, TEXT_UI, ox + dc + 5, y + base, dt.c_str(), c, ox + view_w(w) - 2);
    }
}

static void draw_folder_content(Window *w, pixman_image_t *im, int ox, int oy)
{
    clamp_scroll(w);
    draw_menubar(w, im, ox, oy);
    int vy = oy + MENUBAR;
    fill(im, ox, vy, view_w(w), view_h(w), C_FOLDERBG);
    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, ox, vy, view_w(w), view_h(w));
    pixman_image_set_clip_region32(im, &clip);
    if (w->view == Window::TREE) draw_tree_view(w, im, ox, vy);
    else if (w->view == Window::DETAILS) draw_details_view(w, im, ox, vy);
    else draw_icon_view(w, im, ox, vy);
    if (item_count(w) == 0)
        text_draw(im, TEXT_UI, ox + 12, vy + head_h(w) + 24, "(empty)", C_SHADOW, 0);
    pixman_image_set_clip_region32(im, nullptr);
    pixman_region32_fini(&clip);
    draw_scrollbar(w, im, ox, oy);
}


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
    if (w->kind == Window::FOLDER) save_state();
}

/* internal windows: folders and the Window List */
static Window *new_internal(Window::Kind k, int cw, int ch, Window **slot = nullptr,
                            const std::function<void(Window *)> &init = nullptr)
{
    Window *w = new Window();
    if (slot) *slot = w;                         /* (known before it's first drawn) */
    w->kind = k;
    if (init) init(w);
    w->cw = cw; w->ch = ch;
    w->tree = wlr_scene_tree_create(S.t_wins);
    w->tree->node.data = &w->tag;
    w->mapped = true;
    S.windows.push_back(w);
    if (k != Window::INFO) place(w);              /* (dialogs: the caller centres them) */
    focus(w);
    return w;
}

/* the Tree view: the folder, and the folders in it as they're opened */
static bool has_subfolders(const std::string &path)
{
    if (path.rfind("::", 0) == 0) {
        for (const Object &o : folder_objects(path)) if (o.action == A_FOLDER) return true;
        return false;
    }
    DIR *d = opendir(path.c_str());
    if (!d) return false;
    bool root = path == "/" || path == "/h", found = false;
    while (dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.' || (root && hidden_at_root(n))) continue;
        if (e->d_type == DT_DIR) { found = true; break; }
        if (e->d_type != DT_UNKNOWN) continue;
        struct stat st;
        std::string p = (path == "/" ? "" : path) + "/" + n;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { found = true; break; }
    }
    closedir(d);
    return found;
}

static std::vector<Node> subfolders(const std::string &path, int depth)
{
    std::vector<Node> v;
    for (const Object &o : folder_objects(path))
        if (o.action == A_FOLDER) v.push_back({ o.path, o.label, depth, false, has_subfolders(o.path) });
    return v;
}

static void toggle_node(Window *w, int i)
{
    Node &n = w->nodes[i];
    if (n.open) {
        size_t j = i + 1;
        while (j < w->nodes.size() && w->nodes[j].depth > n.depth) j++;
        w->nodes.erase(w->nodes.begin() + i + 1, w->nodes.begin() + j);
        n.open = false;
    } else {
        auto kids = subfolders(n.path, n.depth + 1);
        w->nodes[i].open = true;
        w->nodes[i].kids = !kids.empty();
        w->nodes.insert(w->nodes.begin() + i + 1, kids.begin(), kids.end());
    }
}

static void load_folder(Window *w)
{
    w->sel = -1;
    w->all = false;
    w->scroll = 0;
    if (w->view == Window::TREE) {
        w->nodes = { { w->path, w->name, 0, false, true } };
        toggle_node(w, 0);
    } else {
        w->objects = folder_objects(w->path);
    }
}

static void set_view(Window *w, Window::View v)
{
    w->view = v;
    load_folder(w);
    draw_window(w);
    save_state();
    draw_bar();
    redraw_winlist();
}

static void open_folder(const std::string &path, const std::string &name, int view_)
{
    Window::View view = (Window::View)view_;
    for (Window *w : S.windows)
        if (w->kind == Window::FOLDER && w->path == path && w->view == view) { focus(w); return; }
    Window *w = new Window();
    w->kind = Window::FOLDER;
    w->path = path;
    w->name = name;
    w->view = view;
    load_folder(w);
    w->cw = view == Window::TREE ? 300 : view == Window::DETAILS ? 520 : 470;
    w->ch = view == Window::TREE ? 330 : 290;
    w->tree = wlr_scene_tree_create(S.t_wins);
    w->tree->node.data = &w->tag;
    w->mapped = true;
    S.windows.push_back(w);
    place(w);
    focus(w);
    save_state();
}

/* a folder's parent: the drive's parent is Drives */
static void open_parent(Window *w)
{
    std::string p = w->path;
    if (p.rfind("::", 0) == 0 || p == "/" || p == "/h") { open_folder("::drives", "Drives", w->view); return; }
    size_t s = p.rfind('/');
    std::string up = s == 0 ? "/" : p.substr(0, s);
    std::string nm = up == "/" ? "Drive C" : up == "/h" ? "Drive D" : up.substr(up.rfind('/') + 1);
    open_folder(up, nm, w->view);
}

/* /h/x/y is D:\x\y, /x is C:\x */
static std::string dos_path(const std::string &p)
{
    if (p.rfind("::", 0) == 0) return "(on the desktop)";
    std::string d = p == "/h" || p.rfind("/h/", 0) == 0 ? "D:" + p.substr(2) : "C:" + p;
    for (char &c : d) if (c == '/') c = '\\';
    if (d.size() == 2) d += "\\";
    return d;
}

/* Properties, About: a window of lines */
static uint32_t darker(uint32_t c, int pct)
{
    auto ch = [&](int sh) { return (uint32_t)(((c >> sh) & 0xFF) * (100 - pct) / 100) << sh; };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

/* geometry, in content coordinates */
static int nb_page_w(const Window *w) { return w->cw - NB_TAB_W - 18; }
static void nb_swatch(int i, int *x, int *y) { *x = 24 + (i % 6) * 46; *y = 46 + (i / 6) * 36; }
static const int NB_SW = 40, NB_SH = 28, NB_CHECK_Y = 128, NB_SCHEME_Y = 46, NB_SCHEME_ROW = 30;

static void nb_check(pixman_image_t *im, int x, int y, bool on)
{
    fill(im, x, y, 13, 13, C_WHITE);
    bevel(im, x, y, 13, 13, false, 1);
    if (on) for (int k = 0; k < 7; k++) fill(im, x + 3 + k, y + 6 + (k < 3 ? k - 1 : 4 - k), 1, 2, C_DARK);
}

static void draw_notebook_content(Window *w, pixman_image_t *im, int ox, int oy)
{
    fill(im, ox, oy, w->cw, w->ch, C_FACE);
    int pw = nb_page_w(w);
    /* the tabs, on the right as in Warp 4 */
    for (int i = 0; i < NB_TABS; i++) {
        int tx = ox + pw + 12, ty = oy + 12 + i * (NB_TAB_H + 2);
        bool on = i == w->tab;
        fill(im, tx, ty, NB_TAB_W, NB_TAB_H, on ? C_FACE : 0xFFB0B0B0);
        bevel(im, tx, ty, NB_TAB_W, NB_TAB_H, true, 1);
        if (on) fill(im, tx - 2, ty + 1, 3, NB_TAB_H - 2, C_FACE);    /* joined to the page */
        text_draw(im, on ? TEXT_BOLD : TEXT_UI, tx + 10, ty + NB_TAB_H / 2 + text_ascent(TEXT_UI) / 2 - 1,
                  nb_tabs[i], C_TEXT, 0);
    }
    fill(im, ox + 8, oy + 8, pw + 4, w->ch - 16, C_FACE);
    bevel(im, ox + 8, oy + 8, pw + 4, w->ch - 16, true, 1);
    int base = text_ascent(TEXT_BOLD);
    if (w->tab == 0) {
        text_draw(im, TEXT_BOLD, ox + 24, oy + 26 + base / 2, "Color", C_TEXT, 0);
        for (int i = 0; i < NPAL; i++) {
            int x, y;
            nb_swatch(i, &x, &y);
            bool sel = palette[i] == S.desk_color;
            fill(im, ox + x - 3, oy + y - 3, NB_SW + 6, NB_SH + 6, sel ? C_DARK : C_FACE);
            if (sel) fill(im, ox + x - 1, oy + y - 1, NB_SW + 2, NB_SH + 2, C_WHITE);
            fill(im, ox + x, oy + y, NB_SW, NB_SH, palette[i]);
            bevel(im, ox + x, oy + y, NB_SW, NB_SH, false, 1);
        }
        nb_check(im, ox + 24, oy + NB_CHECK_Y, S.pattern);
        text_draw(im, TEXT_UI, ox + 44, oy + NB_CHECK_Y + 11, "Pattern", C_TEXT, 0);
        /* what it looks like */
        int px = ox + 24, py = oy + NB_CHECK_Y + 28, pwid = std::min(240, pw - 40), ph = w->ch - (NB_CHECK_Y + 28) - 22;
        fill(im, px, py, pwid, ph, S.desk_color);
        if (S.pattern)
            for (int y = 0; y < ph; y += 2)
                for (int x = (y / 2) % 4; x < pwid; x += 4) fill(im, px + x, py + y, 1, 1, darker(S.desk_color, 14));
        bevel(im, px - 1, py - 1, pwid + 2, ph + 2, false, 1);
    } else {
        text_draw(im, TEXT_BOLD, ox + 24, oy + 26 + base / 2, "Title bars", C_TEXT, 0);
        for (int i = 0; i < NSCHEMES; i++) {
            int y = oy + NB_SCHEME_Y + i * NB_SCHEME_ROW;
            disc(im, ox + 30, y + 8, 6, C_SHADOW);
            disc(im, ox + 30, y + 8, 5, C_WHITE);
            if (i == S.scheme) disc(im, ox + 30, y + 8, 2, C_DARK);
            text_draw(im, TEXT_UI, ox + 44, y + 12, schemes[i].name, C_TEXT, 0);
            gradient_h(im, ox + 120, y, std::min(160, pw - 120), 17, schemes[i].t1, schemes[i].t2);
            text_draw(im, TEXT_BOLD, ox + 128, y + 13, "Title", C_WHITE, 0);
        }
    }
}

static void apply_settings()
{
    draw_desktop();
    for (Window *w : S.windows) draw_window(w);
    save_state();
}

static void notebook_click(Window *w, int cx, int cy)
{
    int pw = nb_page_w(w);
    for (int i = 0; i < NB_TABS; i++) {
        int tx = pw + 12, ty = 12 + i * (NB_TAB_H + 2);
        if (cx >= tx && cx < tx + NB_TAB_W && cy >= ty && cy < ty + NB_TAB_H) { w->tab = i; draw_window(w); return; }
    }
    if (w->tab == 0) {
        for (int i = 0; i < NPAL; i++) {
            int x, y;
            nb_swatch(i, &x, &y);
            if (cx >= x && cx < x + NB_SW && cy >= y && cy < y + NB_SH) { S.desk_color = palette[i]; apply_settings(); return; }
        }
        if (cx >= 20 && cx < 100 && cy >= NB_CHECK_Y - 2 && cy < NB_CHECK_Y + 16) { S.pattern = !S.pattern; apply_settings(); }
    } else {
        for (int i = 0; i < NSCHEMES; i++) {
            int y = NB_SCHEME_Y + i * NB_SCHEME_ROW;
            if (cy >= y - 4 && cy < y + 22 && cx >= 20 && cx < pw) { S.scheme = i; apply_settings(); return; }
        }
    }
}

static void open_desktop_settings()
{
    for (Window *w : S.windows) if (w->kind == Window::NOTEBOOK) { focus(w); return; }
    Window *w = new_internal(Window::NOTEBOOK, 440, 300, nullptr, [](Window *x) { x->info_title = "Desktop - Properties"; });
    w->x = (S.W - w->fw()) / 2;
    w->y = (S.H - w->fh()) / 3;
    wlr_scene_node_set_position(&w->tree->node, w->x, w->y);
}

/* ── the Assistance Center: help topics from \OS2\HELP\*.txt ─────────────
 * Each file's first line is its title; then paragraphs, separated by
 * blank lines, wrapped to the window; lines starting with two spaces are
 * shown as they are (examples, tables of keys). */
static const int HELP_LIST_W = 170;

static void help_load_topics(Window *w)
{
    w->topics.clear();
    std::string dir = sys("help");
    DIR *d = opendir(dir.c_str());
    if (d) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() < 5 || strcasecmp(n.c_str() + n.size() - 4, ".txt")) continue;
            FILE *f = fopen((dir + "/" + n).c_str(), "r");
            if (!f) continue;
            char line[256] = "";
            if (fgets(line, sizeof line, f)) line[strcspn(line, "\r\n")] = 0;
            fclose(f);
            w->topics.push_back({ line[0] ? line : n, dir + "/" + n });
        }
        closedir(d);
    }
    std::sort(w->topics.begin(), w->topics.end(), [](const std::pair<std::string, std::string> &a,
                                                     const std::pair<std::string, std::string> &b) {
        return a.second < b.second; });
}

/* the topic's text, wrapped to the window's width */
static void help_wrap(Window *w)
{
    w->lines.clear();
    if (w->sel < 0 || w->sel >= (int)w->topics.size()) return;
    FILE *f = fopen(w->topics[w->sel].second.c_str(), "r");
    if (!f) { w->lines.push_back("(this topic can't be read)"); return; }
    int width = w->cw - HELP_LIST_W - 40 - SB;
    char buf[2048];
    bool first = true;
    while (fgets(buf, sizeof buf, f)) {
        std::string l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (first) { first = false; continue; }                       /* (the title) */
        if (l.empty() || l.rfind("  ", 0) == 0) { w->lines.push_back(l); continue; }
        std::string cur;
        size_t i = 0;
        while (i < l.size()) {
            size_t j = l.find(' ', i);
            std::string word = l.substr(i, j == std::string::npos ? std::string::npos : j - i);
            std::string next = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() && text_width(TEXT_UI, next.c_str()) > width) { w->lines.push_back(cur); cur = word; }
            else cur = next;
            if (j == std::string::npos) break;
            i = j + 1;
        }
        w->lines.push_back(cur);
    }
    fclose(f);
}

static int help_rows(const Window *w) { return std::max(1, (w->ch - 44) / ROW); }
static int help_scroll_max(const Window *w) { return std::max(0, (int)w->lines.size() - help_rows(w)); }

static void draw_help_content(Window *w, pixman_image_t *im, int ox, int oy)
{
    fill(im, ox, oy, w->cw, w->ch, C_FACE);
    /* the topics */
    int lx = ox + 6, ly = oy + 6, lh = w->ch - 12;
    fill(im, lx, ly, HELP_LIST_W - 8, lh, C_WHITE);
    bevel(im, lx, ly, HELP_LIST_W - 8, lh, false, 2);
    for (int i = 0; i < (int)w->topics.size(); i++) {
        int y = ly + 4 + i * (ROW + 2);
        bool sel = i == w->sel;
        if (sel) fill(im, lx + 3, y, HELP_LIST_W - 14, ROW + 1, C_SEL);
        draw_icon(im, I_HELP, lx + 6, y + 1, 16);
        text_draw(im, TEXT_UI, lx + 26, y + ROW / 2 + text_ascent(TEXT_UI) / 2, w->topics[i].first.c_str(),
                  sel ? C_WHITE : C_TEXT, lx + HELP_LIST_W - 12);
    }
    /* the text */
    int tx = ox + HELP_LIST_W, ty = oy + 6, tw = w->cw - HELP_LIST_W - 6, th = w->ch - 12;
    fill(im, tx, ty, tw, th, C_WHITE);
    bevel(im, tx, ty, tw, th, false, 2);
    if (w->sel >= 0 && w->sel < (int)w->topics.size())
        text_draw(im, TEXT_BOLD, tx + 12, ty + 12 + text_ascent(TEXT_BOLD), w->topics[w->sel].first.c_str(),
                  0xFF1C3C9C, tx + tw - SB - 6);
    w->scroll = std::clamp(w->scroll, 0, help_scroll_max(w));
    int y = ty + 34;
    for (int i = w->scroll; i < (int)w->lines.size() && y + ROW <= ty + th; i++, y += ROW)
        text_draw(im, TEXT_UI, tx + 12, y + text_ascent(TEXT_UI), w->lines[i].c_str(), C_TEXT, tx + tw - SB - 6);
    /* where in it */
    int sx = tx + tw - SB - 2, sy = ty + 2, sh = th - 4;
    fill(im, sx, sy, SB, sh, 0xFFE4E4E4);
    int tot = (int)w->lines.size(), vis = help_rows(w);
    if (tot > vis) {
        int mh = std::max(14, sh * vis / tot), my = sy + (sh - mh) * w->scroll / std::max(1, tot - vis);
        fill(im, sx, my, SB, mh, C_FACE);
        bevel(im, sx, my, SB, mh, true, 1);
    }
}

static void help_select(Window *w, int topic)
{
    if (topic < 0 || topic >= (int)w->topics.size()) return;
    w->sel = topic;
    w->scroll = 0;
    help_wrap(w);
    draw_window(w);
}

static void open_help(int topic)
{
    for (Window *w : S.windows)
        if (w->kind == Window::HELP) { focus(w); help_select(w, topic); return; }
    Window *w = new_internal(Window::HELP, 640, 400, nullptr, [topic](Window *x) {
        x->info_title = "Assistance Center";
        help_load_topics(x);
        x->sel = x->topics.empty() ? -1 : std::clamp(topic, 0, (int)x->topics.size() - 1);
    });
    help_wrap(w);                                     /* (now that it has its size) */
    draw_window(w);
}

static void help_click(Window *w, int cx, int cy)
{
    if (cx >= 6 && cx < HELP_LIST_W - 2) {
        int i = (cy - 10) / (ROW + 2);
        if (cy >= 10 && i < (int)w->topics.size()) help_select(w, i);
    }
}

static bool help_key(Window *w, xkb_keysym_t sym)
{
    int page = help_rows(w) - 1;
    switch (sym) {
    case XKB_KEY_Escape: close_window(w); return true;
    case XKB_KEY_Up: w->scroll--; break;
    case XKB_KEY_Down: w->scroll++; break;
    case XKB_KEY_Prior: w->scroll -= page; break;
    case XKB_KEY_Next: case XKB_KEY_space: w->scroll += page; break;
    case XKB_KEY_Home: w->scroll = 0; break;
    case XKB_KEY_End: w->scroll = help_scroll_max(w); break;
    case XKB_KEY_Tab: help_select(w, (w->sel + 1) % std::max(1, (int)w->topics.size())); return true;
    case XKB_KEY_ISO_Left_Tab:
        help_select(w, (w->sel + (int)w->topics.size() - 1) % std::max(1, (int)w->topics.size())); return true;
    default: return false;
    }
    draw_window(w);
    return true;
}

/* ── dialogs: lines of text, maybe an entry field, buttons ───────────── */
static const int DLG_BTN_H = 24, DLG_EDIT_H = 22;

static int dialog_button_w(const Window::Button &b) { return std::max(76, text_width(TEXT_UI, b.label.c_str()) + 24); }

/* button i's rectangle, in content coordinates */
static void dialog_button_rect(const Window *w, int i, int *x, int *y, int *bw)
{
    *x = 14;
    for (int k = 0; k < i; k++) *x += dialog_button_w(w->buttons[k]) + 8;
    *y = w->ch - DLG_BTN_H - 10;
    *bw = dialog_button_w(w->buttons[i]);
}

static int dialog_button_at(const Window *w, int cx, int cy)
{
    for (int i = 0; i < (int)w->buttons.size(); i++) {
        int x, y, bw;
        dialog_button_rect(w, i, &x, &y, &bw);
        if (cx >= x && cx < x + bw && cy >= y && cy < y + DLG_BTN_H) return i;
    }
    return -1;
}

static void draw_info_content(Window *w, pixman_image_t *im, int ox, int oy)
{
    fill(im, ox, oy, w->cw, w->ch, C_FACE);
    int y = oy + 12;
    for (size_t i = 0; i < w->lines.size(); i++, y += ROW)
        text_draw(im, i == 0 ? TEXT_BOLD : TEXT_UI, ox + 14, y + text_ascent(TEXT_UI), w->lines[i].c_str(), C_TEXT,
                  ox + w->cw - 6);
    if (w->editing) {
        int fx = ox + 14, fy = y + 4, fw = w->cw - 28;
        fill(im, fx, fy, fw, DLG_EDIT_H, C_WHITE);
        bevel(im, fx, fy, fw, DLG_EDIT_H, false, 2);
        int base = fy + DLG_EDIT_H / 2 + text_ascent(TEXT_UI) / 2 - 1;
        int tw = text_width(TEXT_UI, w->edit.c_str());
        if (w->edit_all && !w->edit.empty()) fill(im, fx + 4, fy + 3, std::min(tw + 2, fw - 8), DLG_EDIT_H - 6, C_SEL);
        text_draw(im, TEXT_UI, fx + 5, base, w->edit.c_str(), w->edit_all ? C_WHITE : C_TEXT, fx + fw - 4);
        int cx = std::min(fx + fw - 4, fx + 5 + tw + 1);
        fill(im, cx, fy + 4, 1, DLG_EDIT_H - 8, C_DARK);                 /* the caret */
    }
    for (int i = 0; i < (int)w->buttons.size(); i++) {
        int x, by, bw;
        dialog_button_rect(w, i, &x, &by, &bw);
        x += ox; by += oy;
        if (i == 0) { fill(im, x - 1, by - 1, bw + 2, DLG_BTN_H + 2, C_DARK); }   /* the default */
        fill(im, x, by, bw, DLG_BTN_H, C_FACE);
        bevel(im, x, by, bw, DLG_BTN_H, true, 2);
        const char *l = w->buttons[i].label.c_str();
        text_draw(im, TEXT_UI, x + (bw - text_width(TEXT_UI, l)) / 2, by + DLG_BTN_H / 2 + text_ascent(TEXT_UI) / 2 - 1,
                  l, C_TEXT, 0);
    }
}

/* runs button i (Enter: the first), then closes the dialog */
static void dialog_press(Window *w, int i)
{
    if (i >= 0 && i < (int)w->buttons.size() && w->buttons[i].run) {
        auto run = w->buttons[i].run;
        run(w);
    }
    close_window(w);
}

static Window *open_dialog(const std::string &title, std::vector<std::string> lines,
                           std::vector<Window::Button> buttons, const std::string *edit = nullptr)
{
    if (buttons.empty()) buttons.push_back({ "OK", nullptr });
    int w = 220, bw = 14;
    for (size_t i = 0; i < lines.size(); i++)
        w = std::max(w, text_width(i == 0 ? TEXT_BOLD : TEXT_UI, lines[i].c_str()) + 32);
    for (auto &b : buttons) bw += dialog_button_w(b) + 8;
    w = std::max(w, bw + 6);
    if (edit) w = std::max(w, 320);
    int h = (int)lines.size() * ROW + 12 + (edit ? DLG_EDIT_H + 10 : 0) + DLG_BTN_H + 22;
    Window *d = new_internal(Window::INFO, std::min(w, S.W - 40), h, nullptr, [&](Window *x) {
        x->info_title = title;
        x->lines = std::move(lines);
        x->buttons = std::move(buttons);
        if (edit) { x->editing = true; x->edit = *edit; x->edit_all = true; }
    });
    d->x = (S.W - d->fw()) / 2;
    d->y = (S.H - d->fh()) / 3;
    wlr_scene_node_set_position(&d->tree->node, d->x, d->y);
    return d;
}

static void open_info(const std::string &title, std::vector<std::string> lines)
{
    open_dialog(title, std::move(lines), {});
}

static void error_box(const std::string &what, const std::string &why)
{
    open_dialog("Workplace Shell", { "Error", what, why }, {});
}

/* ── file operations ────────────────────────────────────────────────── */
static std::string parent_of(const std::string &p)
{
    size_t s = p.rfind('/');
    return s == 0 || s == std::string::npos ? "/" : p.substr(0, s);
}
static std::string leaf_of(const std::string &p) { return p.substr(p.rfind('/') + 1); }
static std::string join(const std::string &dir, const std::string &name) { return (dir == "/" ? "" : dir) + "/" + name; }
static bool real_dir(const std::string &p) { return !p.empty() && p.rfind("::", 0) != 0; }
/* the drive a path is on: D: (the stick, /h) or C: */
static char drive_of(const std::string &p) { return p == "/h" || p.rfind("/h/", 0) == 0 ? 'D' : 'C'; }
/* can it be moved, copied, renamed, deleted? (not the drives, nor the desktop's own objects) */
static bool movable(const Object &o) { return !o.shadow && real_dir(o.path) && o.path != "/" && o.path != "/h"; }

static bool copy_tree(const std::string &from, const std::string &to)
{
    struct stat st;
    if (lstat(from.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to.c_str(), (st.st_mode & 0777) | 0700) != 0) return false;
        DIR *d = opendir(from.c_str());
        if (!d) return false;
        bool ok = true;
        while (dirent *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            ok = copy_tree(join(from, e->d_name), join(to, e->d_name)) && ok;
        }
        closedir(d);
        return ok;
    }
    if (S_ISLNK(st.st_mode)) {
        char t[1024];
        ssize_t n = readlink(from.c_str(), t, sizeof t - 1);
        if (n < 0) return false;
        t[n] = 0;
        return symlink(t, to.c_str()) == 0;
    }
    int in = open(from.c_str(), O_RDONLY);
    if (in < 0) return false;
    int out = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 0777);
    if (out < 0) { close(in); return false; }
    static char buf[65536];
    bool ok = true;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = n == 0; break; }
        for (ssize_t o = 0; o < n;) {
            ssize_t m = write(out, buf + o, n - o);
            if (m < 0 && errno == EINTR) continue;
            if (m <= 0) { ok = false; break; }
            o += m;
        }
        if (!ok) break;
    }
    close(in);
    if (close(out) != 0) ok = false;
    return ok;
}

static bool remove_tree(const std::string &p)
{
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(p.c_str()) == 0;
    DIR *d = opendir(p.c_str());
    if (!d) return false;
    bool ok = true;
    while (dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        ok = remove_tree(join(p, e->d_name)) && ok;
    }
    closedir(d);
    return rmdir(p.c_str()) == 0 && ok;
}

/* every folder window showing dir (and every Tree view) sees the change */
static void refresh_dir(const std::string &dir)
{
    for (Window *w : S.windows)
        if (w->kind == Window::FOLDER && (w->path == dir || w->view == Window::TREE)) {
            int sel = w->sel, scroll = w->scroll;
            if (w->view == Window::TREE) {
                /* keep what's open: reopen the same folders */
                std::vector<std::string> open;
                for (auto &n : w->nodes) if (n.open) open.push_back(n.path);
                load_folder(w);
                for (size_t i = 0; i < w->nodes.size(); i++)
                    if (!w->nodes[i].open && w->nodes[i].kids &&
                        std::find(open.begin(), open.end(), w->nodes[i].path) != open.end())
                        toggle_node(w, (int)i);
            } else {
                load_folder(w);
            }
            w->sel = sel < item_count(w) ? sel : -1;
            w->scroll = scroll;
            draw_window(w);
        }
}

/* moves or copies src into the folder dir: OS/2's rule - the same drive
 * moves, another drive copies; Shift forces a move, Ctrl a copy */
static void transfer(const std::string &src, const std::string &dir, bool copy)
{
    std::string name = leaf_of(src), dst = join(dir, name);
    if (parent_of(src) == dir && !copy) return;                       /* already there */
    if (dir == src || dir.rfind(src + "/", 0) == 0) {
        error_box("Can't put " + name + " inside itself.", "");
        return;
    }
    struct stat st;
    if (lstat(dst.c_str(), &st) == 0) {
        error_box(name + " is already in " + dos_path(dir) + ".", "Rename one of them first.");
        return;
    }
    bool ok;
    if (copy) ok = copy_tree(src, dst);
    else {
        ok = rename(src.c_str(), dst.c_str()) == 0;
        if (!ok && errno == EXDEV) ok = copy_tree(src, dst) && remove_tree(src);
    }
    if (!ok) error_box(std::string(copy ? "Can't copy " : "Can't move ") + name + " to " + dos_path(dir) + ":",
                       strerror(errno));
    refresh_dir(dir);
    if (!copy) refresh_dir(parent_of(src));
}

static void confirm_delete(const std::string &path)
{
    std::string name = leaf_of(path);
    struct stat st;
    bool dir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    open_dialog("Confirm delete", {
            name, std::string("Are you sure you want to delete this ") + (dir ? "folder and everything in it?" : "object?"),
            dos_path(path) },
        { { "Delete", [path, name](Window *) {
              if (!remove_tree(path)) error_box("Can't delete " + name + ":", strerror(errno));
              refresh_dir(parent_of(path));
          } },
          { "Cancel", nullptr } });
}

static void rename_dialog(const std::string &path)
{
    std::string name = leaf_of(path);
    open_dialog(name + " - Rename", { "Rename", "New name for " + name + ":" },
        { { "Rename", [path](Window *d) {
              std::string nn = d->edit;
              if (nn.empty() || nn == leaf_of(path)) return;
              if (nn.find('/') != std::string::npos || nn.find('\\') != std::string::npos || nn == "." || nn == "..") {
                  error_box("A name can't contain \\ or /.", ""); return;
              }
              std::string to = join(parent_of(path), nn);
              struct stat st;
              if (lstat(to.c_str(), &st) == 0) { error_box(nn + " already exists.", ""); return; }
              if (rename(path.c_str(), to.c_str()) != 0) error_box("Can't rename " + leaf_of(path) + ":", strerror(errno));
              refresh_dir(parent_of(path));
          } },
          { "Cancel", nullptr } }, &name);
}

static void create_folder(Window *w)
{
    if (!real_dir(w->path)) { error_box("Folders can't be made here.", "Open a drive first."); return; }
    std::string base = "New Folder", name = base;
    struct stat st;
    for (int i = 1; lstat(join(w->path, name).c_str(), &st) == 0; i++) name = base + ":" + std::to_string(i);
    if (mkdir(join(w->path, name).c_str(), 0755) != 0) { error_box("Can't create a folder:", strerror(errno)); return; }
    refresh_dir(w->path);
    if (w->view != Window::TREE)
        for (int i = 0; i < (int)w->objects.size(); i++)
            if (w->objects[i].label == name) { w->sel = i; ensure_visible(w, i); draw_window(w); break; }
}

/* what's selected in a folder, as an object */
static bool selected_object(Window *w, Object *o)
{
    if (w->sel < 0 || w->all) return false;
    if (w->view == Window::TREE) {
        const Node &n = w->nodes[w->sel];
        *o = { n.name, I_FOLDER, A_FOLDER, n.path };
    } else {
        *o = w->objects[w->sel];
    }
    return true;
}

static void properties(Window *w)
{
    std::string name, path, type;
    long long size = -1;
    time_t mt = 0;
    if (w->view == Window::TREE) {
        if (w->sel < 0) return;
        name = w->nodes[w->sel].name; path = w->nodes[w->sel].path; type = "Folder";
    } else {
        if (w->sel < 0) return;
        const Object &o = w->objects[w->sel];
        name = o.label; path = o.path; size = o.size; mt = o.mtime;
        type = o.action == A_FOLDER ? "Folder" : o.action == A_RUN ? "Program" : o.action == A_OS2RUN ? "OS/2 program" : o.action == A_SCRIPT ? "Program (script)"
             : o.icon == I_TEXT ? "Text file" : o.path.empty() ? "Object" : "File";
    }
    std::vector<std::string> l = { name, "Type:  " + type };
    if (!path.empty()) l.push_back("Location:  " + dos_path(path));
    if (size >= 0) l.push_back("Size:  " + bytes(size) + " bytes");
    if (mt) l.push_back("Last write:  " + when(mt));
    open_info(name + " - Properties", l);
}

static void about()
{
    open_info("Product Information", {
        "Workplace Shell", "OS/2 Warp for ARM64",
        "A Wayland compositor (wlroots, C++) in the style of OS/2 Warp 4.",
        "", "Ctrl+Esc  Window List          Alt+Tab  next window",
        "Alt+F4  close                     Alt+O  Command Prompt",
        "Alt+P  Programs                   Alt+Shift+Q  quit to the console",
        "Folders: arrows, Enter, Backspace (parent), F5 (refresh)",
    });
}

/* opens what's selected (a double-click, Enter) */
static void activate(Window *w, int i)
{
    if (i < 0) return;
    if (w->view == Window::TREE) {
        open_folder(w->nodes[i].path, w->nodes[i].name, Window::ICON);
        return;
    }
    Object o = w->objects[i];                /* (the window may be redrawn meanwhile) */
    if (o.action == A_FOLDER) open_folder(o.path, o.label, w->view);
    else run_object(o);
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
    if (S.menu && S.menu->owner == w) { S.menu->owner = nullptr; close_menu(); }
    if (S.drag.src == w) S.drag.src = nullptr;
    if (S.active == w) { S.active = nullptr; focus_next_visible(w); }
    redraw_winlist();
    draw_bar();
}

static void close_window(Window *w)
{
    if (w->top) { wlr_xdg_toplevel_send_close(w->top); return; }
    bool folder = w->kind == Window::FOLDER;
    forget(w);
    w->frame.destroy();
    wlr_scene_node_destroy(&w->tree->node);
    delete w;
    if (folder) save_state();
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
    Window *owner = S.menu->owner;
    S.menu->pic.destroy();
    delete S.menu;
    S.menu = nullptr;
    if (owner) draw_window(owner);                /* its menu bar */
}

static void draw_menu()
{
    Menu *m = S.menu;
    PixBuf *pb = pixbuf_new(m->w, m->h);
    pixman_image_t *im = pb->img;
    fill(im, 0, 0, m->w, m->h, C_FACE);
    bevel(im, 0, 0, m->w, m->h, true, 2);
    for (size_t i = 0; i < m->items.size(); i++) {
        const Menu::Item &it = m->items[i];
        int y = 3 + (int)i * ROW;
        if (it.label.empty()) { fill(im, 6, y + ROW / 2 - 1, m->w - 12, 1, C_SHADOW);
                                fill(im, 6, y + ROW / 2, m->w - 12, 1, C_LIGHT); continue; }
        bool h = (int)i == m->hover && it.enabled;
        if (h) fill(im, 3, y, m->w - 6, ROW, C_SEL);
        uint32_t c = !it.enabled ? C_SHADOW : h ? C_WHITE : C_TEXT;
        if (it.checked)                                        /* a check mark */
            for (int k = 0; k < 7; k++) {
                int cx = 6 + k, cy = y + ROW / 2 + (k < 3 ? k - 1 : 4 - k);
                fill(im, cx, cy, 1, 2, c);
            }
        text_draw(im, TEXT_UI, 18, y + text_ascent(TEXT_UI) + 2, it.label.c_str(), c, 0);
    }
    m->pic.show(S.t_menu, pb, &m->tag);
    wlr_scene_node_set_position(&m->pic.node->node, m->x, m->y);
}

static Menu *open_menu(int x, int y, std::vector<Menu::Item> items, Window *owner = nullptr, int bar = -1)
{
    close_menu();
    Menu *m = new Menu();
    m->items = std::move(items);
    m->owner = owner;
    m->bar = bar;
    int w = 0;
    for (auto &it : m->items) w = std::max(w, text_width(TEXT_UI, it.label.c_str()));
    m->w = w + 40;
    m->h = (int)m->items.size() * ROW + 6;
    m->x = std::max(0, std::min(x, S.W - m->w));
    m->y = std::max(0, std::min(y, S.H - m->h));
    S.menu = m;
    draw_menu();
    if (owner) draw_window(owner);
    return m;
}

/* runs item i, if it can be */
static void menu_run(int i)
{
    Menu *m = S.menu;
    if (!m || i < 0 || i >= (int)m->items.size() || !m->items[i].enabled || m->items[i].label.empty()) return;
    auto run = m->items[i].run;
    close_menu();
    if (run) run();
}

static void menu_key(xkb_keysym_t sym);

static void system_menu(int x, int y)
{
    open_menu(x, y, {
        { "Command Prompt", [] { spawn(terminal()); } },
        { "Programs...", [] { spawn({ sys("fuzzel"), "--config", sys("fuzzel.ini") }); } },
        { "Drives", [] { open_folder("::drives", "Drives"); } },
        { "OS/2 System", [] { open_folder("::system", "OS/2 System"); } },
        { "Window List", [] { toggle_winlist(); } },
        { "", nullptr },
        { "Properties", [] { open_desktop_settings(); } },
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

/* a folder's menu bar menus: Folder, Edit, View, Selected, Help */
static std::vector<Menu::Item> folder_menu(Window *w, int which)
{
    bool any = w->sel >= 0 && !w->all;
    bool folder = any && (w->view == Window::TREE || w->objects[w->sel].action == A_FOLDER);
    Object so;
    bool can_change = any && selected_object(w, &so) && movable(so);
    switch (which) {
    case 0: return {
        { "Open parent", [w] { open_parent(w); } },
        { "Create new folder", [w] { create_folder(w); }, real_dir(w->path) },
        { "Refresh now", [w] { load_folder(w); draw_window(w); } },
        { "", nullptr },
        { "Close", [w] { close_window(w); } } };
    case 1: return {
        { "Select all", [w] { w->all = true; draw_window(w); }, w->view != Window::TREE && !w->objects.empty() },
        { "Deselect all", [w] { w->all = false; w->sel = -1; draw_window(w); } } };
    case 2: {
        std::vector<Menu::Item> v = {
            { "Icon view", [w] { set_view(w, Window::ICON); } },
            { "Tree view", [w] { set_view(w, Window::TREE); } },
            { "Details view", [w] { set_view(w, Window::DETAILS); } } };
        v[w->view].checked = true;
        return v; }
    case 3: return {
        { "Open", [w] { activate(w, w->sel); }, any },
        { "Open as Tree view", [w] {
              if (w->view == Window::TREE) open_folder(w->nodes[w->sel].path, w->nodes[w->sel].name, Window::TREE);
              else open_folder(w->objects[w->sel].path, w->objects[w->sel].label, Window::TREE);
          }, folder },
        { "", nullptr },
        { "Rename...", [w] { Object o; if (selected_object(w, &o)) rename_dialog(o.path); }, can_change },
        { so.shadow ? "Delete shadow" : "Delete...", [w] {
              Object o;
              if (!selected_object(w, &o)) return;
              if (o.shadow) remove_shadow(w->path, o.path); else confirm_delete(o.path);
          }, can_change || (any && so.shadow) },
        { "", nullptr },
        { "Properties", [w] { properties(w); }, any } };
    default: return {
        { "Help index", [] { open_help(0); } },
        { "Using folders", [] { open_help(3); } },
        { "Keys", [] { open_help(2); } },
        { "", nullptr },
        { "Product information", [] { about(); } } };
    }
}

static void open_folder_menu(Window *w, int which)
{
    open_menu(w->x + B + menubar_x(which) - 7, w->y + B + T + MENUBAR - 1, folder_menu(w, which), w, which);
}

static void menu_key(xkb_keysym_t sym)
{
    Menu *m = S.menu;
    int n = (int)m->items.size();
    auto step = [&](int d) {
        int h = m->hover;
        for (int k = 0; k < n; k++) {
            h = ((h < 0 ? (d > 0 ? -1 : 0) : h) + d + n) % n;
            if (!m->items[h].label.empty() && m->items[h].enabled) break;
        }
        m->hover = h;
        draw_menu();
    };
    switch (sym) {
    case XKB_KEY_Escape: close_menu(); break;
    case XKB_KEY_Down: step(1); break;
    case XKB_KEY_Up: step(-1); break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: menu_run(m->hover); break;
    case XKB_KEY_Left: case XKB_KEY_Right:
        if (m->owner && m->bar >= 0) {
            Window *w = m->owner;
            int b = (m->bar + (sym == XKB_KEY_Right ? 1 : MENUBAR_N - 1)) % MENUBAR_N;
            open_folder_menu(w, b);
        }
        break;
    }
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

/* ── the desktop as it was left: \OS2\WPS.INI on the stick ───────────── *
 *   icon X Y Label                       a desktop object moved by hand
 *   folder VIEW X Y W H Path|Name        an open folder window, back to front */
static std::string state_file() { return sys("wps.ini"); }

static void save_state()
{
    if (S.restoring) return;
    std::string tmp = state_file() + ".new";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "# Workplace Shell: the desktop as it was left (written by wps)\n");
    for (auto &p : S.desk_place) fprintf(f, "icon %d %d %s\n", p.second.first, p.second.second, p.first.c_str());
    fprintf(f, "background %06X %d\nscheme %d\n", (unsigned)(S.desk_color & 0xFFFFFF), S.pattern ? 1 : 0, S.scheme);
    for (const Object &o : S.shadows) fprintf(f, "shadow %s|%s\n", o.path.c_str(), o.label.c_str());
    for (const Object &o : S.startup) fprintf(f, "startup %s|%s\n", o.path.c_str(), o.label.c_str());
    for (Window *w : S.windows)
        if (w->kind == Window::FOLDER && w->mapped)
            fprintf(f, "folder %d %d %d %d %d %s|%s\n", (int)w->view, w->x, w->y, w->cw, w->ch,
                    w->path.c_str(), w->name.c_str());
    if (fclose(f) == 0) rename(tmp.c_str(), state_file().c_str());
}

struct Saved { int view, x, y, w, h; std::string path, name; };
static std::vector<Saved> saved_folders;

static void load_state()
{
    FILE *f = fopen(state_file().c_str(), "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        int a, b, c, d, e, n = 0;
        unsigned rgb;
        if (sscanf(line, "icon %d %d %n", &a, &b, &n) == 2 && n) S.desk_place[line + n] = { a, b };
        else if (sscanf(line, "background %x %d", &rgb, &a) == 2) { S.desk_color = 0xFF000000u | rgb; S.pattern = a != 0; }
        else if (sscanf(line, "scheme %d", &a) == 1 && a >= 0 && a < NSCHEMES) S.scheme = a;
        else if (!strncmp(line, "shadow ", 7) || !strncmp(line, "startup ", 8)) {
            bool st = line[1] == 't';
            std::string rest = line + (st ? 8 : 7);
            size_t bar = rest.find('|');
            Object sh;
            if (bar != std::string::npos && resolve_shadow(rest.substr(0, bar), rest.substr(bar + 1), &sh))
                (st ? S.startup : S.shadows).push_back(sh);
        }
        else if (sscanf(line, "folder %d %d %d %d %d %n", &a, &b, &c, &d, &e, &n) == 5 && n) {
            std::string rest = line + n;
            size_t bar = rest.find('|');
            if (bar == std::string::npos || a < 0 || a > 2) continue;
            saved_folders.push_back({ a, b, c, d, e, rest.substr(0, bar), rest.substr(bar + 1) });
        }
    }
    fclose(f);
}

/* the folders that were open: where they were (if they're still there) */
static void restore_folders()
{
    S.restoring = true;
    for (const Saved &s : saved_folders) {
        struct stat st;
        if (real_dir(s.path) && (stat(s.path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))) continue;
        open_folder(s.path, s.name, s.view);
        Window *w = S.active;
        if (!w || w->kind != Window::FOLDER || w->path != s.path) continue;
        w->cw = std::clamp(s.w, 160, S.W);
        w->ch = std::clamp(s.h, 80, S.H);
        w->x = std::clamp(s.x, -w->fw() + 40, S.W - 40);
        w->y = std::clamp(s.y, BAR - T, S.H - 40);
        wlr_scene_node_set_position(&w->tree->node, w->x, w->y);
        draw_window(w);
    }
    S.restoring = false;
}

/* ── Programs: the launcher's entries (\OS2\applications\*.desktop) ──── */
static std::vector<Object> program_objects()
{
    std::vector<Object> v;
    std::string dir = sys("applications");
    DIR *d = opendir(dir.c_str());
    if (!d) return v;
    while (dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() < 9 || n.compare(n.size() - 8, 8, ".desktop")) continue;
        FILE *f = fopen((dir + "/" + n).c_str(), "r");
        if (!f) continue;
        std::string name, exec, cats;
        bool term = false, hidden = false;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "Name=", 5)) name = line + 5;
            else if (!strncmp(line, "Exec=", 5)) exec = line + 5;
            else if (!strncmp(line, "Categories=", 11)) cats = line + 11;
            else if (!strncmp(line, "Terminal=", 9)) term = !strcmp(line + 9, "true");
            else if (!strncmp(line, "NoDisplay=", 10)) hidden = !strcmp(line + 10, "true");
        }
        fclose(f);
        if (name.empty() || exec.empty() || hidden) continue;
        int icon = cats.find("TerminalEmulator") != std::string::npos ? I_PROMPT
                 : cats.find("TextEditor") != std::string::npos ? I_TEXT
                 : exec.find("SHUTDOWN") != std::string::npos ? I_SHUTDOWN
                 : cats.find("Monitor") != std::string::npos ? I_WINLIST : I_PROGRAM;
        Object o{ name, icon, A_APP, dir + "/" + n };
        o.exec = exec;
        o.term = term;
        v.push_back(o);
    }
    closedir(d);
    std::sort(v.begin(), v.end(), [](const Object &a, const Object &b) {
        return strcasecmp(a.label.c_str(), b.label.c_str()) < 0; });
    return v;
}

static void run_app(const Object &o)
{
    std::vector<std::string> args = o.term ? terminal() : std::vector<std::string>();
    std::string w;
    for (size_t i = 0; i <= o.exec.size(); i++) {
        char c = i < o.exec.size() ? o.exec[i] : ' ';
        if (c != ' ') { w += c; continue; }
        if (!w.empty() && w[0] != '%') args.push_back(w);      /* (not %f, %U, ...) */
        w.clear();
    }
    if (!args.empty()) spawn(args);
}

/* ── shadows: links to objects, on the desktop and in Startup ─────────── */
static void rebuild_desktop()
{
    S.desk_objs = S.desk_builtin;
    S.desk_objs.insert(S.desk_objs.end(), S.shadows.begin(), S.shadows.end());
    if (S.desk_sel >= (int)S.desk_objs.size()) S.desk_sel = -1;
    if (S.output) draw_desktop();
}

/* a shadow of orig (an object of a folder, or a program) */
static bool make_shadow(const Object &orig, Object *sh)
{
    if (orig.action == A_APP) *sh = orig;
    else if (!real_dir(orig.path) || !classify(orig.path, orig.label, sh)) return false;
    sh->shadow = true;
    return true;
}

/* the object a shadow saved as path|label stands for, if it's still there */
static bool resolve_shadow(const std::string &path, const std::string &label, Object *sh)
{
    if (path.rfind(sys("applications") + "/", 0) == 0) {
        for (const Object &o : program_objects())
            if (o.path == path) { *sh = o; sh->label = label; sh->shadow = true; return true; }
        return false;
    }
    if (!classify(path, label, sh)) return false;
    sh->shadow = true;
    return true;
}

static std::vector<Object> &shadow_list(const std::string &where) { return where == "::startup" ? S.startup : S.shadows; }

static void add_shadow(const std::string &where, const Object &orig, int x = -1, int y = -1)
{
    Object sh;
    if (!make_shadow(orig, &sh)) return;
    auto &v = shadow_list(where);
    for (const Object &o : v) if (o.path == sh.path) return;              /* one is enough */
    v.push_back(sh);
    if (where == "::desktop") {
        if (x >= 0) S.desk_place[desk_key(sh)] = { std::clamp(x / 8 * 8, 0, S.W - 56), std::clamp(y / 8 * 8, BAR, S.H - 54) };
        rebuild_desktop();
    } else {
        refresh_dir(where);
    }
    save_state();
}

static void remove_shadow(const std::string &where, const std::string &path)
{
    auto &v = shadow_list(where);
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].path == path) {
            if (where == "::desktop") S.desk_place.erase(desk_key(v[i]));
            v.erase(v.begin() + i);
            break;
        }
    if (where == "::desktop") rebuild_desktop(); else refresh_dir(where);
    save_state();
}

/* the folder holding a shadow's original, with it selected */
static void open_original_folder(const Object &sh)
{
    if (sh.action == A_APP) { open_folder("::programs", "Programs"); return; }
    std::string dir = parent_of(sh.path);
    std::string nm = dir == "/" ? "Drive C" : dir == "/h" ? "Drive D" : leaf_of(dir);
    open_folder(dir, nm);
    Window *w = S.active;
    if (!w || w->kind != Window::FOLDER || w->view == Window::TREE) return;
    for (int i = 0; i < (int)w->objects.size(); i++)
        if (w->objects[i].path == sh.path) { w->sel = i; ensure_visible(w, i); draw_window(w); }
}

static void desktop_object_menu(int i, int x, int y)
{
    Object o = S.desk_objs[i];
    std::vector<Menu::Item> items = { { "Open", [o] { run_object(o); } } };
    if (o.shadow) {
        items.push_back({ "Open folder of original", [o] { open_original_folder(o); } });
        items.push_back({ "", nullptr });
        items.push_back({ "Delete shadow", [o] { remove_shadow("::desktop", o.path); } });
    }
    open_menu(x, y, items);
}

/* ── drag and drop: an object onto a folder, a drive or the Shredder ── */
static void drag_arm(Window *src, const Object &o)
{
    if (!movable(o) && o.action != A_APP && !o.shadow) return;   /* (those: shadows made, or removed) */
    S.drag = Server::Drag();
    S.drag.armed = true;
    S.drag.src = src;
    S.drag.obj = o;
    S.drag.x0 = S.cursor->x;
    S.drag.y0 = S.cursor->y;
}

static void drag_cancel()
{
    S.drag.pic.destroy();
    S.drag = Server::Drag();
}

static void drag_motion()
{
    Server::Drag &d = S.drag;
    double lx = S.cursor->x, ly = S.cursor->y;
    if (!d.active) {
        if (std::abs(lx - d.x0) < 6 && std::abs(ly - d.y0) < 6) return;
        d.active = true;
        PixBuf *pb = pixbuf_new(32, 32);
        draw_icon(pb->img, d.obj.icon, 0, 0, 32);
        d.pic.show(S.t_menu, pb, nullptr);
    }
    wlr_scene_node_set_position(&d.pic.node->node, (int)lx - 16, (int)ly - 16);
}

static Hit hit_at(double lx, double ly);
static int folder_item_at(Window *w, int cx, int cy, bool *box);

/* where the object was dropped: a folder's path, "::shredder", or "" */
static std::string drop_target(double lx, double ly)
{
    wlr_scene_node_set_enabled(&S.drag.pic.node->node, false);
    Hit h = hit_at(lx, ly);
    if (!h.tag) return "";
    if (h.tag->k == Tag::DESK) {
        for (size_t k = 0; k < S.desk_pos.size(); k++) {
            int x = S.desk_pos[k].first, y = S.desk_pos[k].second;
            if (lx >= x && lx < x + 56 && ly >= y && ly < y + 54) {
                const Object &o = S.desk_objs[k];
                if (o.action == A_SHREDDER) return "::shredder";
                if (o.action == A_FOLDER && real_dir(o.path)) return o.path;
                return "";
            }
        }
        return "::desktop";                               /* the desktop itself: a shadow there */
    }
    if (h.tag->k != Tag::WIN || h.tag->w->kind != Window::FOLDER) return "";
    Window *w = h.tag->w;
    int cx = (int)lx - w->x - B, cy = (int)ly - w->y - B - T;
    bool box;
    int i = folder_item_at(w, cx, cy, &box);
    if (i >= 0) {
        std::string p = w->view == Window::TREE ? w->nodes[i].path
                      : w->objects[i].action == A_FOLDER ? w->objects[i].path : "";
        if (real_dir(p) || p == "::startup") return p;
    }
    return real_dir(w->path) || w->path == "::startup" ? w->path : "";
}

static void drag_drop()
{
    Server::Drag d = S.drag;
    if (d.desk >= 0) {                                   /* a desktop object: put where it's dropped */
        std::string target = d.active ? drop_target(S.cursor->x, S.cursor->y) : "";
        drag_cancel();
        if (!d.active) return;
        if (target == "::shredder") { if (d.obj.shadow) remove_shadow("::desktop", d.obj.path); return; }
        if (target == "::startup") { add_shadow("::startup", d.obj); return; }
        if (target != "::desktop") return;
        int x = (int)(S.cursor->x - d.dx), y = (int)(S.cursor->y - d.dy);
        x = std::clamp((x + 4) / 8 * 8, 0, S.W - 56);
        y = std::clamp((y + 4) / 8 * 8, BAR, S.H - 54);
        S.desk_place[desk_key(d.obj)] = { x, y };
        draw_desktop();
        save_state();
        return;
    }
    std::string target = d.active ? drop_target(S.cursor->x, S.cursor->y) : "";
    bool was_active = d.active;
    drag_cancel();
    if (!was_active || target.empty()) return;
    if (target == "::shredder") {
        if (d.obj.shadow) remove_shadow("::startup", d.obj.path);       /* (the shadow, not its original) */
        else if (movable(d.obj)) confirm_delete(d.obj.path);
        return;
    }
    if (target == "::desktop") { add_shadow("::desktop", d.obj, (int)S.cursor->x - 28, (int)S.cursor->y - 16); return; }
    if (target == "::startup") { add_shadow("::startup", d.obj); return; }
    if (!movable(d.obj)) return;
    wlr_keyboard *kb = wlr_seat_get_keyboard(S.seat);
    uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
    bool copy = drive_of(d.obj.path) != drive_of(target);
    if (mods & WLR_MODIFIER_CTRL) copy = true;
    if (mods & WLR_MODIFIER_SHIFT) copy = false;
    transfer(d.obj.path, target, copy);
}

/* which item is at (cx, cy) in a folder's content; *box: on a Tree +/- box */
static int folder_item_at(Window *w, int cx, int cy, bool *box)
{
    *box = false;
    if (cx < 0 || cx >= view_w(w) || cy < MENUBAR) return -1;
    int vy = cy - MENUBAR;
    switch (w->view) {
    case Window::ICON: {
        int ty = vy - 6;
        if (ty < 0 || cx < 4) return -1;
        int c = (cx - 4) / CELL_W, r = ty / CELL_H + w->scroll;
        if (c >= folder_cols(w)) return -1;
        int i = r * folder_cols(w) + c;
        return i < (int)w->objects.size() ? i : -1;
    }
    case Window::TREE: {
        if (vy < 3) return -1;
        int i = (vy - 3) / TROW + w->scroll;
        if (i >= (int)w->nodes.size()) return -1;
        int bx = tree_x(w->nodes[i].depth);
        if (cx >= bx - 2 && cx < bx + 11) { *box = w->nodes[i].kids; return i; }
        int tw = text_width(TEXT_UI, w->nodes[i].name.c_str());
        return cx >= bx + 12 && cx < bx + 38 + tw ? i : -1;
    }
    case Window::DETAILS: {
        if (vy < TROW + 1) return -1;
        int i = (vy - TROW - 1) / TROW + w->scroll;
        return i < (int)w->objects.size() ? i : -1;
    }
    }
    return -1;
}

static void folder_click(Window *w, int cx, int cy, uint32_t time, bool right)
{
    if (cx >= view_w(w)) {                                   /* the scroll bar */
        int ty, th, my, mh, page = std::max(1, rows_visible(w) - (w->view == Window::ICON ? 0 : 1));
        sb_geometry(w, &ty, &th, &my, &mh);
        if (cy < ty) w->scroll--;
        else if (cy >= ty + th) w->scroll++;
        else if (cy < my) w->scroll -= page;
        else if (cy >= my + mh) w->scroll += page;
        else if (scroll_max(w) > 0) {                         /* the thumb: drag it */
            S.grab = w; S.mode = Server::THUMB;
            S.gy = S.cursor->y; S.gch = w->scroll;
        }
        clamp_scroll(w);
        draw_window(w);
        return;
    }
    bool box;
    int i = folder_item_at(w, cx, cy, &box);
    if (box) { toggle_node(w, i); w->sel = i; draw_window(w); return; }
    bool dbl = !right && double_click(time, w, i);
    w->sel = i;
    w->all = false;
    draw_window(w);
    Object o;
    if (!right && !dbl && selected_object(w, &o)) drag_arm(w, o);
    int sx = w->x + B + cx, sy = w->y + B + T + cy;
    if (right) open_menu(sx, sy, folder_menu(w, i >= 0 ? 3 : 2), w);
    else if (dbl) activate(w, i);
}

static void content_click(Window *w, int cx, int cy, uint32_t time, bool right)
{
    if (w->kind == Window::HELP) {
        if (!right) help_click(w, cx, cy);
    } else if (w->kind == Window::NOTEBOOK) {
        if (!right) notebook_click(w, cx, cy);
    } else if (w->kind == Window::INFO) {
        int b = dialog_button_at(w, cx, cy);
        if (b >= 0 && !right) dialog_press(w, b);
    } else if (w->kind == Window::FOLDER) {
        folder_click(w, cx, cy, time, right);
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
    if (S.drag.armed) { drag_motion(); return; }
    if (S.mode == Server::MOVE && S.grab) {
        Window *w = S.grab;
        w->x = S.gwx + (int)(lx - S.gx);
        w->y = std::max(BAR - T, S.gwy + (int)(ly - S.gy));
        wlr_scene_node_set_position(&w->tree->node, w->x, w->y);
        return;
    }
    if (S.mode == Server::THUMB && S.grab) {
        Window *w = S.grab;
        int ty, th, my, mh;
        sb_geometry(w, &ty, &th, &my, &mh);
        int room = std::max(1, th - mh);
        w->scroll = S.gch + (int)((ly - S.gy) * scroll_max(w) / room);
        clamp_scroll(w);
        draw_window(w);
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
        if (Window *w = S.menu->owner; w && S.menu->bar >= 0) {     /* along the menu bar */
            int fx = (int)lx - w->x - B, fy = (int)ly - w->y - B - T;
            int b = fy >= 0 && fy < MENUBAR ? menubar_at(fx) : -1;
            if (b >= 0 && b != S.menu->bar) open_folder_menu(w, b);
        }
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
        if (S.drag.armed) { drag_drop(); return; }
        if (S.mode != Server::NONE) {
            Window *w = S.grab;
            S.mode = Server::NONE; S.grab = nullptr;
            if (w && !w->top) draw_window(w);
            if (w && w->kind == Window::FOLDER) save_state();     /* (moved, resized) */
            return;
        }
        wlr_seat_pointer_notify_button(S.seat, e->time_msec, e->button, e->state);
        return;
    }
    bool right = e->button == 0x111;                    /* BTN_RIGHT */
    Window *menu_owner = nullptr;                       /* (a click on its label again: closed) */
    int menu_bar = -1;
    if (S.menu) {
        Menu *m = S.menu;
        int mx = (int)lx - m->x, my = (int)ly - m->y;
        if (mx >= 0 && mx < m->w && my >= 0 && my < m->h) {
            if (my >= 3) menu_run((my - 3) / ROW);
            return;
        }
        menu_owner = m->owner;
        menu_bar = m->bar;
        close_menu();
    }
    Hit h = hit_at(lx, ly);
    if (h.tag && h.tag->k == Tag::BAR) {
        int i = ((int)lx - 4) / BAR_BTN;
        if (lx >= 4 && i >= 0 && i < (int)(sizeof bar_buttons / sizeof bar_buttons[0])) {
            const BarButton &b = bar_buttons[i];
            if (b.icon == I_SYSTEM) system_menu(4 + i * BAR_BTN, BAR);
            else if (b.action == A_FOLDER) open_folder("::drives", "Drives");
            else if (b.action == A_PROGRAMS) {
                std::vector<Menu::Item> items;
                for (const Object &o : program_objects()) items.push_back({ o.label, [o] { run_app(o); } });
                items.push_back({ "", nullptr });
                items.push_back({ "Programs folder", [] { open_folder("::programs", "Programs"); } });
                open_menu(4 + i * BAR_BTN, BAR, items);
            }
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
        case HIT_MENUBAR: {
            int b = menubar_at(fx - B);
            if (b >= 0 && !(menu_owner == w && menu_bar == b)) open_folder_menu(w, b);
            break;
        }
        case HIT_CONTENT:
            content_click(w, fx - B, fy - B - T, e->time_msec, right);
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
        if (right) { desktop_object_menu(i, (int)lx, (int)ly); return; }
        bool dbl = double_click(e->time_msec, &S.desk_tag, i);
        if (i != S.desk_sel) { S.desk_sel = i; draw_desktop(); }
        if (dbl && i >= 0) run_object(S.desk_objs[i]);
        else if (i >= 0 && !right) {                     /* maybe moved somewhere else */
            S.drag = Server::Drag();
            S.drag.armed = true;
            S.drag.desk = i;
            S.drag.obj = S.desk_objs[i];
            S.drag.x0 = lx; S.drag.y0 = ly;
            S.drag.dx = lx - S.desk_pos[i].first; S.drag.dy = ly - S.desk_pos[i].second;
        }
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
    if (h.tag && h.tag->k == Tag::WIN && h.tag->w->kind == Window::HELP) {
        Window *w = h.tag->w;
        w->scroll += e->delta > 0 ? 3 : e->delta < 0 ? -3 : 0;
        draw_window(w);
        return;
    }
    if (h.tag && h.tag->k == Tag::WIN && h.tag->w->kind == Window::FOLDER) {
        Window *w = h.tag->w;
        w->scroll += e->delta > 0 ? 1 : e->delta < 0 ? -1 : 0;
        clamp_scroll(w);
        draw_window(w);
    }
}

/* ── keyboard ───────────────────────────────────────────────────────── */
struct Keyboard { wlr_keyboard *kb; Listener key, mods, destroy; };

/* keys for the compositor's own windows (folders, Window List, info) */
static bool internal_key(Window *w, xkb_keysym_t sym, const char *utf8)
{
    if (w->kind == Window::INFO) {
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { dialog_press(w, 0); return true; }
        if (sym == XKB_KEY_Escape) { close_window(w); return true; }
        if (!w->editing) return false;
        if (w->edit_all && (sym == XKB_KEY_BackSpace || sym == XKB_KEY_Delete ||
                            (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7F))) {
            w->edit.clear();                                     /* typing replaces the selection */
            w->edit_all = false;
            if (sym == XKB_KEY_BackSpace || sym == XKB_KEY_Delete) { draw_window(w); return true; }
        }
        if (sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_End || sym == XKB_KEY_Home) {
            w->edit_all = false;                                 /* (the caret stays at the end) */
        } else if (sym == XKB_KEY_BackSpace) {
            while (!w->edit.empty() && (w->edit.back() & 0xC0) == 0x80) w->edit.pop_back();   /* a whole UTF-8 character */
            if (!w->edit.empty()) w->edit.pop_back();
        } else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7F && w->edit.size() < 200) {
            w->edit += utf8;
        } else {
            return false;
        }
        draw_window(w);
        return true;
    }
    if (w->kind == Window::HELP) return help_key(w, sym);
    if (w->kind == Window::NOTEBOOK) {
        if (sym == XKB_KEY_Escape) { close_window(w); return true; }
        if (sym == XKB_KEY_Next || sym == XKB_KEY_Prior) { w->tab = (w->tab + 1) % NB_TABS; draw_window(w); return true; }
        return false;
    }
    if (w->kind == Window::WINLIST) {
        auto list = listed_windows();
        int n = (int)list.size();
        switch (sym) {
        case XKB_KEY_Up: w->lsel = std::max(0, w->lsel - 1); break;
        case XKB_KEY_Down: w->lsel = std::min(n - 1, w->lsel + 1); break;
        case XKB_KEY_Return: case XKB_KEY_KP_Enter:
            if (w->lsel >= 0 && w->lsel < n) focus(list[w->lsel]);
            return true;
        case XKB_KEY_Escape: close_window(w); return true;
        default: return false;
        }
        draw_window(w);
        return true;
    }
    if (w->kind != Window::FOLDER) return false;
    int n = item_count(w), cols = w->view == Window::ICON ? folder_cols(w) : 1;
    int page = cols * std::max(1, rows_visible(w) - 1), s = w->sel;
    bool tree = w->view == Window::TREE;
    switch (sym) {
    case XKB_KEY_F10: open_folder_menu(w, 0); return true;
    case XKB_KEY_F5: load_folder(w); draw_window(w); return true;
    case XKB_KEY_BackSpace: open_parent(w); return true;
    case XKB_KEY_Delete: case XKB_KEY_F2: {
        Object o;
        if (selected_object(w, &o) && o.shadow && sym == XKB_KEY_Delete) remove_shadow(w->path, o.path);
        else if (selected_object(w, &o) && movable(o)) {
            if (sym == XKB_KEY_Delete) confirm_delete(o.path);
            else rename_dialog(o.path);
        }
        return true;
    }
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: activate(w, w->sel); return true;
    case XKB_KEY_Up: s = s < 0 ? 0 : s - cols; break;
    case XKB_KEY_Down: s = s < 0 ? 0 : s + cols; break;
    case XKB_KEY_Home: s = 0; break;
    case XKB_KEY_End: s = n - 1; break;
    case XKB_KEY_Prior: s = s < 0 ? 0 : s - page; break;
    case XKB_KEY_Next: s = s < 0 ? 0 : s + page; break;
    case XKB_KEY_Left: case XKB_KEY_minus: case XKB_KEY_KP_Subtract:
        if (!tree) { s = s < 0 ? 0 : s - 1; break; }
        if (s >= 0 && w->nodes[s].open) { toggle_node(w, s); break; }
        while (s > 0 && w->nodes[s - 1].depth >= w->nodes[s].depth) s--;     /* to the parent */
        if (s > 0) s--;
        break;
    case XKB_KEY_Right: case XKB_KEY_plus: case XKB_KEY_KP_Add:
        if (!tree) { s = s < 0 ? 0 : s + 1; break; }
        if (s >= 0 && w->nodes[s].kids && !w->nodes[s].open) toggle_node(w, s);
        break;
    default: return false;
    }
    if (n == 0) return true;
    n = item_count(w);
    w->sel = std::clamp(s, 0, n - 1);
    w->all = false;
    ensure_visible(w, w->sel);
    draw_window(w);
    return true;
}

static bool binding(uint32_t mods, xkb_keysym_t sym)
{
    bool alt = mods & WLR_MODIFIER_ALT, ctrl = mods & WLR_MODIFIER_CTRL, shift = mods & WLR_MODIFIER_SHIFT;
    if (ctrl && sym == XKB_KEY_Escape) { toggle_winlist(); return true; }
    if (sym == XKB_KEY_F1 && !alt && !ctrl) {                 /* help: about what's in front */
        open_help(S.active && S.active->kind == Window::FOLDER ? 3 : 0);
        return true;
    }
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
            if (S.drag.armed && n && syms[0] == XKB_KEY_Escape) { drag_cancel(); return; }
            if (S.menu && n) { menu_key(syms[0]); return; }
            char utf8[16] = "";
            xkb_state_key_get_utf8(k->kb->xkb_state, e->keycode + 8, utf8, sizeof utf8);
            if (S.active && !S.active->top && n && internal_key(S.active, syms[0], utf8)) return;
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
        restore_folders();
        for (const Object &o : S.startup) run_object(o);     /* the Startup folder */
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

    S.desk_builtin = {
        { "OS/2 System", I_SYSTEM, A_FOLDER, "::system" },
        { "Assistance Center", I_HELP, A_HELP, "" },
        { "Drives", I_DRIVES, A_FOLDER, "::drives" },
        { "Command Prompt", I_PROMPT, A_PROMPT, "" },
        { "Drive D", I_DRIVE, A_FOLDER, "/h" },
        { "Programs", I_PROGRAMS, A_FOLDER, "::programs" },
        { "Window List", I_WINLIST, A_WINLIST, "" },
        { "Shut Down", I_SHUTDOWN, A_SHUTDOWN, "" },
        { "Shredder", I_SHREDDER, A_SHREDDER, "" },
    };
    load_state();
    rebuild_desktop();

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
