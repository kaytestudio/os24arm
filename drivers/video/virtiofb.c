/**
 * @file virtiofb.c
 * @brief VirtIO GPU Framebuffer Driver for OS/2 Warp ARM64
 * 
 * Provides a simple framebuffer interface for QEMU virt machine: 32-bit
 * XRGB, double buffering, at the size the host's display has (QEMU:
 * -device virtio-gpu-device,xres=1920,yres=1080) and following it when
 * it changes (QEMU's window resized: fb_check_resize). The memory is
 * reserved once for the largest size (FB_MAX_*), so what programs map of
 * it (/dev/fb0) stays good whatever the size; the image is always packed
 * tightly in it (stride = width * 4). ramfb: 1024x768 only.
 */

#include "types.h"
#include "uart.h"
#include "kprintf.h"
#include "gui/ramfb.h"
#include "gui/virtio_gpu.h"

/* Forward declare memory functions */
extern void *mem_alloc(size_t size);
extern void mem_free(void *ptr);
/* Page frames (kernel/src/mmu.c, QEMU virt): the front buffer comes from
 * there when it can, page aligned and contiguous, so Linux programs can
 * map it (/dev/fb0, kernel/src/linux/linux.c). */
extern void *mmu_frames_alloc(unsigned long n) __attribute__((weak));

/* ── Framebuffer Configuration ────────────────────────────────────────────── */
#define FB_WIDTH        1024    /* without a size from the host (ramfb, ...) */
#define FB_HEIGHT       768
#define FB_MAX_W        2560    /* the most there's memory for (16 MB) */
#define FB_MAX_H        1600
#define FB_MIN_W        640
#define FB_MIN_H        480
#define FB_BPP          32      /* Bits per pixel (RGBA8888) */
#define FB_STRIDE       (FB_WIDTH * (FB_BPP / 8))
#define FB_SIZE         (FB_MAX_W * FB_MAX_H * (FB_BPP / 8))

/* ── Framebuffer State ────────────────────────────────────────────────────── */
typedef struct {
    int cap_w, cap_h;       /* what the memory has room for */
    uint32_t *pixels;       /* Front buffer (displayed) */
    uint32_t *backbuffer;   /* Back buffer (drawn to) */
    int width;
    int height;
    int stride;             /* Bytes per row */
    bool initialized;
    bool double_buffer;
} framebuffer_t;

static framebuffer_t fb = {0};

/* A size the memory has room for, and no smaller than usable */
static void fit_size(uint32_t *w, uint32_t *h)
{
    uint32_t cw = fb.cap_w ? (uint32_t)fb.cap_w : FB_MAX_W, ch = fb.cap_h ? (uint32_t)fb.cap_h : FB_MAX_H;
    if (*w > cw) *w = cw;
    if (*h > ch) *h = ch;
    if (*w < FB_MIN_W) *w = FB_MIN_W;
    if (*h < FB_MIN_H) *h = FB_MIN_H;
    *w &= ~7u;                                         /* (whole groups of 8: tidy rows) */
}

/**
 * @brief Initialize framebuffer driver
 *
 * Allocates front and back buffers, then brings up a display path for
 * them: a real virtio-gpu 2D scanout (see gui/virtio_gpu.h) if
 * "-device virtio-gpu-device" is attached, otherwise ramfb.
 */
bool fb_init_driver(void)
{
    if (fb.initialized) {
        uart_puts("[FB] Already initialized\r\n");
        return true;
    }

    uart_puts("[FB] Initializing framebuffer...\r\n");

    /* Front buffer: page frames (room for the largest size), else the
     * heap (1024x768 then) */
    int cap_w = FB_MAX_W, cap_h = FB_MAX_H;
    fb.pixels = mmu_frames_alloc ? (uint32_t *)mmu_frames_alloc((FB_SIZE + 4095) / 4096) : 0;
    if (!fb.pixels) { cap_w = FB_WIDTH; cap_h = FB_HEIGHT; fb.pixels = (uint32_t *)mem_alloc((size_t)cap_w * cap_h * 4); }
    if (!fb.pixels) {
        uart_puts("[FB] ERROR: Failed to allocate front buffer\r\n");
        return false;
    }
    fb.cap_w = cap_w;
    fb.cap_h = cap_h;

    /* Allocate back buffer for double buffering (the kernel's own GUI) */
    fb.backbuffer = mmu_frames_alloc && cap_w == FB_MAX_W ? (uint32_t *)mmu_frames_alloc((FB_SIZE + 4095) / 4096)
                                                         : (uint32_t *)mem_alloc((size_t)cap_w * cap_h * 4);
    if (!fb.backbuffer) {
        uart_puts("[FB] WARNING: No double buffering (low memory)\r\n");
        fb.backbuffer = fb.pixels;  /* Use single buffer */
        fb.double_buffer = false;
    } else {
        fb.double_buffer = true;
    }

    /* The size: the host's display, if it has one (virtio-gpu) */
    uint32_t hw = FB_WIDTH, hh = FB_HEIGHT;
    int gpu = virtio_gpu_probe();
    if (gpu && virtio_gpu_host_size(&hw, &hh)) fit_size(&hw, &hh);
    else { hw = FB_WIDTH; hh = FB_HEIGHT; }
    fb.width = (int)hw;
    fb.height = (int)hh;
    fb.stride = fb.width * 4;
    fb.initialized = true;

    /* Clear screen to black */
    for (int i = 0; i < fb.cap_w * fb.cap_h; i++) {
        fb.pixels[i] = 0xFF000000;  /* Black (ARGB) */
        if (fb.double_buffer) {
            fb.backbuffer[i] = 0xFF000000;
        }
    }

    kprintf("[FB] Framebuffer ready: %dx%d @ %d-bit (room for %dx%d)\r\n",
            fb.width, fb.height, FB_BPP, fb.cap_w, fb.cap_h);
    kprintf("[FB] Front buffer: 0x%p\r\n", fb.pixels);
    kprintf("[FB] Back buffer:  0x%p\r\n", fb.backbuffer);
    kprintf("[FB] Double buffering: %s\r\n",
            fb.double_buffer ? "enabled" : "disabled");

    /* Prefer a real virtio-gpu 2D scanout - it's the only path that can
     * later negotiate VIRTIO_GPU_F_VIRGL for 3D (see gui/virtio_gpu.h).
     * Fall back to ramfb (if attached via -device ramfb) when no
     * virtio-gpu-device is present; ramfb needs no ongoing fb_present()
     * traffic since QEMU just polls this RAM continuously. */
    if (!gpu || !virtio_gpu_set_mode(fb.pixels, (uint32_t)fb.width, (uint32_t)fb.height)) {
        fb.width = FB_WIDTH; fb.height = FB_HEIGHT; fb.stride = FB_STRIDE;
        ramfb_init(fb.pixels, (uint32_t)FB_WIDTH, (uint32_t)FB_HEIGHT, (uint32_t)FB_STRIDE);
    } else {
        uart_puts("[GPU] virtio-gpu 2D scanout ready\r\n");
    }

    return true;
}

/**
 * @brief Get framebuffer info
 */
void fb_get_info(int *width, int *height, int *bpp)
{
    if (width) *width = fb.width;
    if (height) *height = fb.height;
    if (bpp) *bpp = FB_BPP;
}

/* The displayed buffer (what /dev/fb0 maps), and telling the display
 * that a rectangle of it changed (virtio-gpu; ramfb needs nothing). */
uint32_t *fb_front(void) { return fb.initialized ? fb.pixels : 0; }

/* The memory behind the front buffer, in bytes (all of it can be mapped) */
unsigned long fb_capacity(void) { return fb.initialized ? (unsigned long)fb.cap_w * fb.cap_h * 4 : 0; }

/* Has the host's display changed size? Then the image is shown at the new
 * size (cleared: what was there is laid out for the old width) - 1 if so. */
int fb_check_resize(void)
{
    uint32_t w, h;
    if (!fb.initialized || !virtio_gpu_resized(&w, &h)) return 0;
    fit_size(&w, &h);
    if ((int)w == fb.width && (int)h == fb.height) return 0;
    for (int i = 0; i < (int)(w * h); i++) fb.pixels[i] = 0xFF000000;
    if (!virtio_gpu_set_mode(fb.pixels, w, h)) return 0;
    fb.width = (int)w;
    fb.height = (int)h;
    fb.stride = fb.width * 4;
    kprintf("[FB] display resized: %dx%d\r\n", fb.width, fb.height);
    return 1;
}

void fb_flush(int x, int y, int w, int h)
{
    if (!fb.initialized) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb.width) w = fb.width - x;
    if (y + h > fb.height) h = fb.height - y;
    if (w > 0 && h > 0) virtio_gpu_flush(x, y, w, h);
}

/**
 * @brief Get back buffer for drawing
 */
uint32_t *fb_get_buffer(void)
{
    return fb.backbuffer;
}

/**
 * @brief Present back buffer to screen (page flip)
 *
 * Copies back buffer to front buffer if double buffering is enabled.
 */
void fb_present(void)
{
    if (!fb.initialized) return;

    if (fb.double_buffer) {
        /* Copy back buffer to front buffer */
        for (int i = 0; i < fb.width * fb.height; i++) {
            fb.pixels[i] = fb.backbuffer[i];
        }
    }

    /* ramfb needs nothing here - QEMU polls that RAM continuously. A
     * real virtio-gpu scanout does need an explicit sync, since the host
     * only re-reads the backing buffer on TRANSFER_TO_HOST_2D. */
    virtio_gpu_flush(0, 0, fb.width, fb.height);
}

/**
 * @brief Set pixel in back buffer
 */
void fb_set_pixel(int x, int y, uint32_t color)
{
    if (x < 0 || x >= fb.width || y < 0 || y >= fb.height) return;
    fb.backbuffer[y * fb.width + x] = color;
}

/**
 * @brief Get pixel from back buffer
 */
uint32_t fb_get_pixel(int x, int y)
{
    if (x < 0 || x >= fb.width || y < 0 || y >= fb.height) return 0;
    return fb.backbuffer[y * fb.width + x];
}

/**
 * @brief Fill rectangle
 */
void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            fb_set_pixel(x + dx, y + dy, color);
        }
    }
}

/**
 * @brief Clear screen
 */
void fb_clear_screen(uint32_t color)
{
    fb_fill_rect(0, 0, fb.width, fb.height, color);
}

/**
 * @brief Draw horizontal line
 */
void fb_draw_hline(int x, int y, int w, uint32_t color)
{
    for (int i = 0; i < w; i++) {
        fb_set_pixel(x + i, y, color);
    }
}

/**
 * @brief Draw vertical line
 */
void fb_draw_vline(int x, int y, int h, uint32_t color)
{
    for (int i = 0; i < h; i++) {
        fb_set_pixel(x, y + i, color);
    }
}

/**
 * @brief Draw rectangle outline
 */
void fb_draw_rect(int x, int y, int w, int h, uint32_t color)
{
    fb_draw_hline(x, y, w, color);              /* Top */
    fb_draw_hline(x, y + h - 1, w, color);      /* Bottom */
    fb_draw_vline(x, y, h, color);              /* Left */
    fb_draw_vline(x + w - 1, y, h, color);      /* Right */
}

/**
 * @brief Draw line (Bresenham's algorithm)
 */
void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0;
    int dy = y1 - y0;

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (1) {
        fb_set_pixel(x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/**
 * @brief Blit (copy) image data to framebuffer
 */
void fb_blit(int x, int y, int w, int h, const uint32_t *data)
{
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            fb_set_pixel(x + dx, y + dy, data[dy * w + dx]);
        }
    }
}

/**
 * @brief Get framebuffer width
 */
int fb_width(void)
{
    return fb.width;
}

/**
 * @brief Get framebuffer height
 */
int fb_height(void)
{
    return fb.height;
}