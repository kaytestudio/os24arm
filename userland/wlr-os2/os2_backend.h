/*
 * userland/wlr-os2/os2_backend.h - a wlroots backend for a Linux framebuffer
 * device and evdev input devices, without DRM, libinput or a session.
 *
 * Output: /dev/fb0 (fbdev: FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO, mmap),
 * one fixed mode; each committed frame's damage is copied into the mapping
 * and presented with the kernel's damage ioctl (or FBIOPAN_DISPLAY). The
 * cursor is drawn in software.
 * Input: /dev/input/event0 (keyboard) and /dev/input/event1 (pointer:
 * relative or absolute), read as struct input_event.
 *
 * Written for os2warp's Linux-ABI user mode (userland/README.md), but only
 * standard Linux interfaces are used.
 */
#ifndef OS2_BACKEND_H
#define OS2_BACKEND_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>

/* fb: framebuffer device path (NULL: $WLR_OS2_FB or /dev/fb0);
 * keyboard / pointer: evdev paths (NULL: $WLR_OS2_KBD, $WLR_OS2_PTR or
 * /dev/input/event0 / event1; missing devices are skipped). */
struct wlr_backend *wlr_os2_backend_create(struct wl_event_loop *loop, const char *fb,
                                           const char *keyboard, const char *pointer);
bool wlr_backend_is_os2(struct wlr_backend *backend);

/* A US keymap in XKB text format, for when xkeyboard-config isn't
 * installed (xkb_keymap_new_from_names fails). */
const char *wlr_os2_fallback_keymap(void);

#endif
