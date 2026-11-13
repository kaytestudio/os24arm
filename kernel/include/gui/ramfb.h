/**
 * @file ramfb.h
 * @brief QEMU "ramfb" boot framebuffer configuration (fw_cfg DMA)
 *
 * ramfb is QEMU's simplest display device: point it at a plain RGB buffer
 * already sitting in guest RAM (via the "etc/ramfb" fw_cfg file, written
 * over the fw_cfg DMA interface) and QEMU continuously displays that
 * memory region directly - no virtqueues, no GPU command submission,
 * no ongoing driver traffic after the one-time setup. Requires the VM to
 * be started with "-device ramfb" (see the run-gui CMake target).
 */

#ifndef GUI_RAMFB_H
#define GUI_RAMFB_H

#include "types.h"

/* Point QEMU's ramfb device at an existing RGB framebuffer already
 * allocated in guest RAM (e.g. drivers/video/virtiofb.c's fb.pixels).
 * fb_addr/width/height/stride describe that buffer; pixels must already
 * be (or will be) written as 32-bit XRGB (0x00RRGGBB per pixel, alpha
 * byte ignored) - matching what this project's framebuffer code already
 * writes. Returns false if "-device ramfb" wasn't attached to the VM (no
 * "etc/ramfb" fw_cfg file found) or the DMA interface didn't respond.
 */
bool ramfb_init(void *fb_addr, uint32_t width, uint32_t height, uint32_t stride);
/* Did ramfb_init() find QEMU's ramfb (a display someone can see)? */
bool ramfb_available(void);

#endif /* GUI_RAMFB_H */
