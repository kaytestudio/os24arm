#!/bin/bash
# userland/build-compositor.sh - build the stage 5/6 programs:
#
#   build/userland/bin/tinywl    wlroots' reference compositor (from the
#                                wlroots source tree), patched at build time
#                                to use the os2 backend (userland/wlr-os2),
#                                the built-in keymap when xkeyboard-config
#                                isn't installed, and to start its -s client
#                                directly (there is no /bin/sh)
#   build/userland/bin/wlclient  a small xdg-shell client (userland/wlr-os2)
#   build/userland/bin/cage      the cage kiosk compositor 0.2.1, patched the
#                                same way (and without VT switching)
#
# Needs userland/build-libs.sh first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/build/userland"
SR="$W/sysroot"
CC="$W/toolchain/aarch64-linux-musl-cc"
SCANNER="$W/host/bin/wayland-scanner"
SRC="$W/src/wlroots-0.19.2"
OUT="$W/bin"
GEN="$W/gen-compositor"
[ -x "$CC" ] && [ -d "$SRC" ] || "$ROOT/userland/build-libs.sh"
mkdir -p "$OUT" "$GEN"

XDG="$SR/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
"$SCANNER" server-header "$XDG" "$GEN/xdg-shell-protocol.h"
"$SCANNER" client-header "$XDG" "$GEN/xdg-shell-client-protocol.h"
"$SCANNER" private-code "$XDG" "$GEN/xdg-shell-protocol.c"
python3 "$ROOT/userland/wlr-os2/gen-keymap.py" "$GEN/keymap_us.h"
cmp -s "$GEN/keymap_us.h" "$ROOT/userland/wlr-os2/keymap_us.h" ||
    { echo "userland/wlr-os2/keymap_us.h is stale: regenerate it with gen-keymap.py" >&2; exit 1; }

# tinywl, patched
python3 - "$SRC/tinywl/tinywl.c" "$GEN/tinywl.c" <<'PY'
import sys
s = open(sys.argv[1]).read()
def rep(a, b):
    global s
    assert s.count(a) == 1, "tinywl.c changed upstream: " + a[:60]
    s = s.replace(a, b)
rep('#include <xkbcommon/xkbcommon.h>\n',
    '#include <xkbcommon/xkbcommon.h>\n#include <spawn.h>\n#include <string.h>\n#include "os2_backend.h"\n')
rep('''	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
''', '''	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) { /* os2: no xkeyboard-config installed */
		keymap = xkb_keymap_new_from_string(context, wlr_os2_fallback_keymap(),
			XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
''')
rep('''	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
''', '''	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) { /* os2: no /usr/share/X11/xkb to include from */
		context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
	}
''')
rep('''	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), NULL);''',
    '''	server.backend = wlr_os2_backend_create(wl_display_get_event_loop(server.wl_display),
		NULL, NULL, NULL);
	if (server.backend == NULL) {
		server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), NULL);
	}''')
rep('''		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}''', '''		/* os2: no shell - split on spaces and spawn the program itself */
		char *argv_s[16];
		int n = 0;
		for (char *t = strtok(startup_cmd, " "); t && n < 15; t = strtok(NULL, " ")) {
			argv_s[n++] = t;
		}
		argv_s[n] = NULL;
		extern char **environ;
		pid_t pid;
		if (n && posix_spawn(&pid, argv_s[0], NULL, NULL, argv_s, environ) != 0) {
			wlr_log(WLR_ERROR, "can't start %s", argv_s[0]);
		}''')
open(sys.argv[2], "w").write(s)
PY

PKGS="wlroots-0.19 wayland-server xkbcommon pixman-1 libdrm"
FLAGS=(-O2 -Wall -Wno-unused-parameter -DWLR_USE_UNSTABLE -I"$GEN" -I"$ROOT/userland/wlr-os2")
# shellcheck disable=SC2046,SC2086
"$CC" "${FLAGS[@]}" -o "$OUT/tinywl" "$GEN/tinywl.c" "$ROOT/userland/wlr-os2/os2_backend.c" \
    "$GEN/xdg-shell-protocol.c" \
    $(PKG_CONFIG_LIBDIR="$SR/lib/pkgconfig:$SR/share/pkgconfig" pkg-config --static --cflags --libs $PKGS)
echo "  tinywl"
# shellcheck disable=SC2046
"$CC" "${FLAGS[@]}" -o "$OUT/wlclient" "$ROOT/userland/wlr-os2/wlclient.c" "$GEN/xdg-shell-protocol.c" \
    $(PKG_CONFIG_LIBDIR="$SR/lib/pkgconfig" pkg-config --static --cflags --libs wayland-client)
echo "  wlclient"

# ── cage (kiosk compositor), patched like tinywl ────────────────────────
CAGE_VER=0.2.1
CAGE_SHA256=fc1238e3aa5b82787a95d49cb3e1bac0671e4d3a40090087848f43f3e1f63a98
CAGE_URL="https://github.com/cage-kiosk/cage/releases/download/v$CAGE_VER/cage-$CAGE_VER.tar.gz"
CAGE_TGZ="$W/dl/cage-$CAGE_VER.tar.gz"
CAGE_SRC="$W/src/cage-$CAGE_VER"
if [ ! -f "$CAGE_TGZ" ]; then
    echo "Downloading $CAGE_URL"
    curl -sSfL -o "$CAGE_TGZ.part" "$CAGE_URL" && mv "$CAGE_TGZ.part" "$CAGE_TGZ"
fi
echo "$CAGE_SHA256  $CAGE_TGZ" | shasum -a 256 -c - >/dev/null \
    || { echo "SHA-256 mismatch for $CAGE_TGZ" >&2; rm -f "$CAGE_TGZ"; exit 1; }
if [ ! -d "$CAGE_SRC" ]; then
    mkdir -p "$CAGE_SRC.tmp" && tar xzf "$CAGE_TGZ" -C "$CAGE_SRC.tmp" --strip-components=1
    mv "$CAGE_SRC.tmp" "$CAGE_SRC"
fi
CG="$GEN/cage"
rm -rf "$CG"; mkdir -p "$CG"
cp "$CAGE_SRC"/*.c "$CAGE_SRC"/*.h "$CG"/
rm -f "$CG/xwayland.c" "$CG/xwayland.h"            # no XWayland
printf '#ifndef CG_CONFIG_H\n#define CG_CONFIG_H\n#define CAGE_HAS_XWAYLAND 0\n#define CAGE_VERSION "%s (os2warp)"\n#endif\n' \
    "$CAGE_VER" > "$CG/config.h"
python3 - "$CG" <<'PY'
import sys
d = sys.argv[1]
def patch(f, pairs):
    p = d + "/" + f
    s = open(p).read()
    for a, b in pairs:
        assert s.count(a) == 1, "cage %s changed upstream: %s" % (f, a[:60])
        s = s.replace(a, b)
    open(p, "w").write(s)
patch("cage.c", [
    ('#include "seat.h"\n', '#include "seat.h"\n#include "os2_backend.h"\n'),
    ('''	server.backend = wlr_backend_autocreate(event_loop, &server.session);''',
     '''	server.backend = wlr_os2_backend_create(event_loop, NULL, NULL, NULL);
	if (!server.backend) {
		server.backend = wlr_backend_autocreate(event_loop, &server.session);
	}'''),
])
patch("seat.c", [
    ('#include <wlr/backend/session.h>\n', '#include "os2_backend.h"\n'),   # no sessions here
    ('''				wlr_session_change_vt(server->session, vt);''', '''				(void)vt; /* os2: no VT switching */'''),
    ('''	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
''', '''	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) { /* os2: no /usr/share/X11/xkb */
		context = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
	}
'''),
    ('''	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
''', '''	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) { /* os2: no xkeyboard-config: the built-in US keymap */
		keymap = xkb_keymap_new_from_string(context, wlr_os2_fallback_keymap(),
			XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
'''),
])
PY
# shellcheck disable=SC2046,SC2086
"$CC" "${FLAGS[@]}" -std=c11 -Wno-sign-compare -I"$CG" -o "$OUT/cage" "$CG"/*.c \
    "$ROOT/userland/wlr-os2/os2_backend.c" "$GEN/xdg-shell-protocol.c" \
    $(PKG_CONFIG_LIBDIR="$SR/lib/pkgconfig:$SR/share/pkgconfig" pkg-config --static --cflags --libs $PKGS)
echo "  cage"
