/**
 * @file virtiofb.c
 * @brief VirtIO GPU Framebuffer Driver for OS/2 Warp ARM64
 * 
 * Provides a software framebuffer for the QEMU virt machine.
 * Supports: 1024x768 @ 32-bit RGBA, double buffering, page flipping
 */

#include "types.h"
#include "uart.h"
#include "kprintf.h"

/* Forward declare memory functions */
extern void *mem_alloc(size_t size);
extern void mem_free(void *ptr);

/* VirtIO MMIO Registers */
#define VIRTIO_GPU_BASE     0x0A000000UL  /* QEMU virt: virtio-gpu-device */

#define VIRTIO_MMIO_MAGIC            0x000  /* Magic value 'virt' */
#define VIRTIO_MMIO_VERSION          0x004  /* Device version */
#define VIRTIO_MMIO_DEVICE_ID        0x008  /* Virtio Subsystem Device ID */
#define VIRTIO_MMIO_VENDOR_ID        0x00C  /* Virtio Subsystem Vendor ID */
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010  /* Flags representing features */
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020  /* Driver feature bits */
#define VIRTIO_MMIO_STATUS           0x070  /* Device status */

/* Device IDs */
#define VIRTIO_ID_GPU       16

/* Status bits */
#define VIRTIO_CONFIG_S_ACKNOWLEDGE  1
#define VIRTIO_CONFIG_S_DRIVER       2
#define VIRTIO_CONFIG_S_DRIVER_OK    4
#define VIRTIO_CONFIG_S_FEATURES_OK  8
#define VIRTIO_CONFIG_S_FAILED       128

/* Framebuffer Configuration */
#define FB_WIDTH        1024
#define FB_HEIGHT       768
#define FB_BPP          32      /* Bits per pixel (RGBA8888) */
#define FB_STRIDE       (FB_WIDTH * (FB_BPP / 8))
#define FB_SIZE         (FB_STRIDE * FB_HEIGHT)

/* Framebuffer State */
typedef struct {
    uint32_t *pixels;       /* Front buffer (displayed) */
    uint32_t *backbuffer;   /* Back buffer (drawn to) */
    int width;
    int height;
    int stride;             /* Bytes per row */
    bool initialized;
    bool double_buffer;
} framebuffer_t;

static framebuffer_t fb = {0};

/* VirtIO GPU Helpers */
static inline uint32_t virtio_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(VIRTIO_GPU_BASE + offset);
}

static inline void virtio_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(VIRTIO_GPU_BASE + offset) = value;
}

/* Check that a VirtIO GPU is present and mark the device ready. */
static bool virtio_gpu_init(void)
{
    uint32_t magic = virtio_read32(VIRTIO_MMIO_MAGIC);
    if (magic != 0x74726976) {  /* 'virt' in little-endian */
        uart_puts("[FB] VirtIO GPU not found\r\n");
        return false;
    }
    
    uint32_t version = virtio_read32(VIRTIO_MMIO_VERSION);
    if (version != 2) {
        kprintf("[FB] Unsupported VirtIO version: %u\r\n", version);
        return false;
    }
    
    uint32_t device_id = virtio_read32(VIRTIO_MMIO_DEVICE_ID);
    if (device_id != VIRTIO_ID_GPU) {
        kprintf("[FB] Not a GPU device: %u\r\n", device_id);
        return false;
    }
    
    uart_puts("[FB] VirtIO GPU detected\r\n");
    
    /* Reset device */
    virtio_write32(VIRTIO_MMIO_STATUS, 0);
    
    /* Acknowledge */
    virtio_write32(VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    
    /* Driver ready */
    virtio_write32(VIRTIO_MMIO_STATUS, 
                   VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);
    
    /* Features OK */
    virtio_write32(VIRTIO_MMIO_STATUS, 
                   VIRTIO_CONFIG_S_ACKNOWLEDGE | 
                   VIRTIO_CONFIG_S_DRIVER | 
                   VIRTIO_CONFIG_S_FEATURES_OK);
    
    /* Driver OK */
    virtio_write32(VIRTIO_MMIO_STATUS, 
                   VIRTIO_CONFIG_S_ACKNOWLEDGE | 
                   VIRTIO_CONFIG_S_DRIVER | 
                   VIRTIO_CONFIG_S_FEATURES_OK | 
                   VIRTIO_CONFIG_S_DRIVER_OK);
    
    uart_puts("[FB] VirtIO GPU initialized\r\n");
    return true;
}

/* Allocate the framebuffer and its optional back buffer. */
bool fb_init(void)
{
    if (fb.initialized) {
        uart_puts("[FB] Already initialized\r\n");
        return true;
    }
    
    uart_puts("[FB] Initializing framebuffer...\r\n");
    
    /* Initialize VirtIO GPU (optional - can work without it) */
    virtio_gpu_init();
    
    /* Allocate the front buffer (FB_SIZE bytes). */
    fb.pixels = (uint32_t *)mem_alloc(FB_SIZE);
    if (!fb.pixels) {
        uart_puts("[FB] ERROR: Failed to allocate front buffer\r\n");
        return false;
    }
    
    /* Allocate back buffer for double buffering */
    fb.backbuffer = (uint32_t *)mem_alloc(FB_SIZE);
    if (!fb.backbuffer) {
        uart_puts("[FB] WARNING: No double buffering (low memory)\r\n");
        fb.backbuffer = fb.pixels;  /* Use single buffer */
        fb.double_buffer = false;
    } else {
        fb.double_buffer = true;
    }
    
    fb.width = FB_WIDTH;
    fb.height = FB_HEIGHT;
    fb.stride = FB_STRIDE;
    fb.initialized = true;
    
    /* Clear screen to black */
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        fb.pixels[i] = 0xFF000000;  /* Black (ARGB) */
        if (fb.double_buffer) {
            fb.backbuffer[i] = 0xFF000000;
        }
    }
    
    kprintf("[FB] Framebuffer ready: %dx%d @ %d-bit\r\n", 
            FB_WIDTH, FB_HEIGHT, FB_BPP);
    kprintf("[FB] Front buffer: 0x%p\r\n", fb.pixels);
    kprintf("[FB] Back buffer:  0x%p\r\n", fb.backbuffer);
    kprintf("[FB] Double buffering: %s\r\n", 
            fb.double_buffer ? "enabled" : "disabled");
    
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
        for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
            fb.pixels[i] = fb.backbuffer[i];
        }
    }
    
    /* TODO: Submit a VirtIO GPU transfer and flush command. */
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
void fb_clear(uint32_t color)
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
