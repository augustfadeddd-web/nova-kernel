#ifndef TINY_OS_GRAPHICS_H
#define TINY_OS_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>

enum {
    GFX_CHAR_WIDTH = 8,
    GFX_CHAR_HEIGHT = 16,
    GFX_MAX_WIDTH = 1280,
    GFX_MAX_HEIGHT = 1024
};

/* Initialize from Multiboot 1's RGB framebuffer information. */
bool gfx_init(uint32_t multiboot_info_address);
uint32_t gfx_width(void);
uint32_t gfx_height(void);

#define GFX_TRANSPARENT 0x01000000U

/* Colors are 0xRRGGBB. Text/masks accept GFX_TRANSPARENT as background. Drawing is clipped into an off-screen buffer.
 * Call gfx_present once after completing a frame to display it. */
void gfx_present(void);
void gfx_surface_begin(uint32_t *pixels,uint32_t width,uint32_t height);
void gfx_surface_end(void);
/* Destination holds gfx_width() * gfx_height() RGB pixels. */
void gfx_capture(uint32_t *destination);
uint32_t gfx_mix(uint32_t foreground, uint32_t background, unsigned int alpha);
void gfx_mask_scaled(int x, int y, int width, int height, const uint8_t *mask,
                     int source_width, int source_height, uint32_t foreground, uint32_t background);
extern const uint8_t nova_topo_aa[], nova_logo_aa[], nova_font_aa[];
void gfx_rect(int x, int y, int width, int height, uint32_t rgb);
/* Copies a width*height block of 0xRRGGBB pixels (row stride = width) into
 * the frame buffer, clipped to the screen. Used for full-frame game blits. */
void gfx_blit(int x, int y, int width, int height, const uint32_t *source);
void gfx_line(int x1, int y1, int x2, int y2, uint32_t rgb);
void gfx_circle(int center_x, int center_y, int radius, uint32_t rgb);
void gfx_triangle(int x1, int y1, int x2, int y2, int x3, int y3, uint32_t rgb);
void gfx_round_rect(int x, int y, int width, int height, int radius, uint32_t rgb);
void gfx_char(int x, int y, char character, uint32_t foreground, uint32_t background);
void gfx_text(int x, int y, const char *text, uint32_t foreground, uint32_t background);
void gfx_text_scaled(int x, int y, const char *text, uint32_t foreground,
                     uint32_t background, int scale);

#endif
