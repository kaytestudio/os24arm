/**
 * @file virtio_gpu.h
 * @brief Minimal virtio-gpu 2D driver (real control-queue protocol)
 *
 * Unlike drivers/video/virtiofb.c's old virtio_gpu_init() - which only
 * does the MMIO status-register handshake and then relies entirely on
 * gui/ramfb.h to actually reach the display - this speaks the real
 * virtio-gpu command protocol: GET_DISPLAY_INFO, RESOURCE_CREATE_2D,
 * RESOURCE_ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D,
 * RESOURCE_FLUSH. ramfb already works and needs none of this for plain
 * 2D output; the reason to have a real virtio-gpu driver at all is that
 * it's the only path that can later negotiate VIRTIO_GPU_F_VIRGL for 3D -
 * ramfb is a dead end for that.
 *
 * Requires the VM to be started with "-device virtio-gpu-device" (the
 * virtio-mmio transport, matching gui/virtio_input.c's slot-scan - NOT
 * "virtio-gpu-pci", which lives on a different bus this driver doesn't
 * probe). Safely does nothing (returns false) if that device isn't
 * attached, leaving ramfb as the display path.
 */

#ifndef GUI_VIRTIO_GPU_H
#define GUI_VIRTIO_GPU_H

#include "types.h"

/* Probe the QEMU "virt" machine's virtio-mmio slots for a virtio-gpu
 * device and, if found, bring up its control virtqueue and one 2D
 * scanout resource backed directly by `fb_addr` (no extra copy) -
 * `width`x`height` 32-bit XRGB pixels (matching FB_COLOR's byte layout,
 * same as gui/ramfb.h expects), `stride` bytes per row, tightly packed
 * (stride must equal width*4 - virtio-gpu's TRANSFER_TO_HOST_2D has no
 * separate stride field, see virtio_gpu.c). Returns false, changing
 * nothing, if no virtio-gpu-device is attached. */
bool virtio_gpu_init(void *fb_addr, uint32_t width, uint32_t height, uint32_t stride);

/* True once virtio_gpu_init() has found and successfully brought up a
 * device. Lets callers (drivers/video/virtiofb.c) decide whether to also
 * rely on ramfb. */
bool virtio_gpu_available(void);

/* Sync the given rectangle of the backing buffer (already written by the
 * caller, e.g. after a page flip) to the actual display:
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. No-op if virtio_gpu_init() didn't
 * find a device. */
void virtio_gpu_flush(int x, int y, int w, int h);

/* The host's display size (QEMU: its window), a change of it (resized:
 * the event taken, the new size), and showing a buffer at a size. */
bool virtio_gpu_probe(void);
bool virtio_gpu_host_size(uint32_t *w, uint32_t *h);
bool virtio_gpu_resized(uint32_t *w, uint32_t *h);
bool virtio_gpu_set_mode(void *fb_addr, uint32_t width, uint32_t height);
void virtio_gpu_shutdown(void);                /* before powering off: the display let go of */

#endif /* GUI_VIRTIO_GPU_H */
