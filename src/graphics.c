#include "graphics.h"


#include <stddef.h>

/* Multiboot2 tag accessors.  The bootloader hands us a physical address for
 * the information structure and (via tag type 8) the framebuffer; the 64-bit
 * kernel identity-maps all physical memory below 4 GiB (see src/boot.S). */
struct multiboot2_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bits_per_pixel;
    uint8_t framebuffer_type;
    uint8_t reserved;
    uint8_t padding; /* color_info is 4-byte aligned (starts at offset 32). */
    uint8_t red_position;
    uint8_t red_size;
    uint8_t green_position;
    uint8_t green_size;
    uint8_t blue_position;
    uint8_t blue_size;
} __attribute__((packed));

_Static_assert(offsetof(struct multiboot2_framebuffer, address) == 8,
               "Incorrect Multiboot2 framebuffer offset");
_Static_assert(offsetof(struct multiboot2_framebuffer, red_position) == 32,
               "Incorrect Multiboot2 RGB color offset");

static struct {
    volatile uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bytes_per_pixel;
    uint8_t red_position, green_position, blue_position;
    uint8_t red_shift, green_shift, blue_shift;
    bool native_rgb;
} framebuffer;

/* A complete frame is composed in ordinary RAM before touching video memory.
 * This lives in .bss, not on the 16 KiB kernel stack. */
static uint32_t screen_buffer[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];
static uint32_t *backbuffer = screen_buffer;
static uint32_t saved_width, saved_height;
void gfx_surface_begin(uint32_t *pixels, uint32_t width, uint32_t height)
{ saved_width=framebuffer.width; saved_height=framebuffer.height; backbuffer=pixels; framebuffer.width=width;framebuffer.height=height; }
void gfx_surface_end(void)
{ backbuffer=screen_buffer;framebuffer.width=saved_width;framebuffer.height=saved_height; }

static bool valid_channel(uint8_t position, uint8_t size, uint8_t bits_per_pixel)
{
    return size > 0 && size <= 8 && position < bits_per_pixel &&
           (unsigned int)position + size <= bits_per_pixel;
}

bool gfx_init(uint32_t multiboot_info_address)
{
    framebuffer.pixels = NULL;
    framebuffer.width = 0;
    framebuffer.height = 0;
    if (multiboot_info_address == 0)
        return false;

    /* Walk the Multiboot2 tags (each padded to 8 bytes) for framebuffer
     * information (type 8). */
    const uint8_t *tag = (const uint8_t *)(uintptr_t)multiboot_info_address + 8;
    uint32_t total_size = ((const uint32_t *)(uintptr_t)multiboot_info_address)[0];
    const uint8_t *end = (const uint8_t *)(uintptr_t)multiboot_info_address + total_size;
    const struct multiboot2_framebuffer *info = NULL;
    while (tag + 8 <= end) {
        uint32_t type = ((const uint32_t *)tag)[0];
        uint32_t size = ((const uint32_t *)tag)[1];
        if (size < 8)
            return false;
        if (type == 0)
            break;
        if (type == 8) {
            info = (const struct multiboot2_framebuffer *)tag;
            break;
        }
        tag += (size + 7U) & ~7U;
    }
    if (!info || info->framebuffer_type != 1 ||
        (info->bits_per_pixel != 24 && info->bits_per_pixel != 32) ||
        info->address == 0 || info->address > 0xFFFFFFFFULL ||
        info->width < 640 || info->width > GFX_MAX_WIDTH ||
        info->height < 480 || info->height > GFX_MAX_HEIGHT)
        return false;

    uint32_t bytes_per_pixel = info->bits_per_pixel / 8;
    if (info->pitch < info->width * bytes_per_pixel ||
        info->pitch > UINT32_MAX / info->height)
        return false;
    uint64_t byte_count = (uint64_t)info->pitch * info->height;
    if (info->address + byte_count > 0x100000000ULL)
        return false;

    if (!valid_channel(info->red_position, info->red_size, info->bits_per_pixel) ||
        !valid_channel(info->green_position, info->green_size, info->bits_per_pixel) ||
        !valid_channel(info->blue_position, info->blue_size, info->bits_per_pixel))
        return false;

    uint32_t red_mask = ((1U << info->red_size) - 1U) << info->red_position;
    uint32_t green_mask = ((1U << info->green_size) - 1U) << info->green_position;
    uint32_t blue_mask = ((1U << info->blue_size) - 1U) << info->blue_position;
    if ((red_mask & green_mask) || (red_mask & blue_mask) || (green_mask & blue_mask))
        return false;

    framebuffer.pixels = (volatile uint8_t *)(uintptr_t)info->address;
    framebuffer.width = info->width;
    framebuffer.height = info->height;
    framebuffer.pitch = info->pitch;
    framebuffer.bytes_per_pixel = (uint8_t)bytes_per_pixel;
    framebuffer.red_position = info->red_position;
    framebuffer.green_position = info->green_position;
    framebuffer.blue_position = info->blue_position;
    framebuffer.red_shift = 8 - info->red_size;
    framebuffer.green_shift = 8 - info->green_size;
    framebuffer.blue_shift = 8 - info->blue_size;
    framebuffer.native_rgb = info->red_position == 16 && info->red_size == 8 &&
        info->green_position == 8 && info->green_size == 8 &&
        info->blue_position == 0 && info->blue_size == 8;
    return true;
}

uint32_t gfx_width(void)
{
    return framebuffer.width;
}

uint32_t gfx_height(void)
{
    return framebuffer.height;
}

static uint32_t native_color(uint32_t rgb)
{
    return (((rgb >> 16) & 255U) >> framebuffer.red_shift) << framebuffer.red_position |
           (((rgb >> 8) & 255U) >> framebuffer.green_shift) << framebuffer.green_position |
           ((rgb & 255U) >> framebuffer.blue_shift) << framebuffer.blue_position;
}

void gfx_capture(uint32_t *destination)
{
    if (!destination || !framebuffer.pixels) return;
    for (uint32_t i = 0; i < framebuffer.width * framebuffer.height; ++i)
        destination[i] = backbuffer[i];
}

void gfx_present(void)
{
    if (framebuffer.pixels == NULL)
        return;
    for (uint32_t y = 0; y < framebuffer.height; ++y) {
        const uint32_t *source = backbuffer + y * framebuffer.width;
        volatile uint8_t *destination = framebuffer.pixels + y * framebuffer.pitch;
        if (framebuffer.bytes_per_pixel == 4 && framebuffer.native_rgb) {
            volatile uint32_t *pixels = (volatile uint32_t *)destination;
            for (uint32_t x = 0; x < framebuffer.width; ++x)
                pixels[x] = source[x];
        } else if (framebuffer.bytes_per_pixel == 4) {
            volatile uint32_t *pixels = (volatile uint32_t *)destination;
            for (uint32_t x = 0; x < framebuffer.width; ++x)
                pixels[x] = native_color(source[x]);
        } else {
            for (uint32_t x = 0; x < framebuffer.width; ++x) {
                uint32_t color = framebuffer.native_rgb ? source[x] : native_color(source[x]);
                destination[0] = (uint8_t)color;
                destination[1] = (uint8_t)(color >> 8);
                destination[2] = (uint8_t)(color >> 16);
                destination += 3;
            }
        }
    }
}

void gfx_rect(int x, int y, int width, int height, uint32_t rgb)
{
    if (framebuffer.pixels == NULL || width <= 0 || height <= 0)
        return;
    if (x < 0) {
        if (x <= -width)
            return;
        width += x;
        x = 0;
    }
    if (y < 0) {
        if (y <= -height)
            return;
        height += y;
        y = 0;
    }
    if (x >= (int)framebuffer.width || y >= (int)framebuffer.height)
        return;
    if (width > (int)framebuffer.width - x)
        width = (int)framebuffer.width - x;
    if (height > (int)framebuffer.height - y)
        height = (int)framebuffer.height - y;

    uint32_t color = rgb & 0xFFFFFFU;
    for (int line = 0; line < height; ++line) {
        uint32_t *destination = backbuffer + (uint32_t)(y + line) * framebuffer.width + x;
        for (int column = 0; column < width; ++column)
            destination[column] = color;
    }
}

void gfx_blit(int x, int y, int width, int height, const uint32_t *source)
{
    int start_x = 0, start_y = 0;
    int stride = width;
    if (framebuffer.pixels == NULL || width <= 0 || height <= 0 || source == NULL)
        return;
    if (x < 0) {
        if (x <= -width)
            return;
        start_x = -x;
        width += x;
        x = 0;
    }
    if (y < 0) {
        if (y <= -height)
            return;
        start_y = -y;
        height += y;
        y = 0;
    }
    if (x >= (int)framebuffer.width || y >= (int)framebuffer.height)
        return;
    if (width > (int)framebuffer.width - x)
        width = (int)framebuffer.width - x;
    if (height > (int)framebuffer.height - y)
        height = (int)framebuffer.height - y;

    for (int line = 0; line < height; ++line) {
        const uint32_t *row = source + (uint32_t)(start_y + line) * (uint32_t)stride
                            + (uint32_t)start_x;
        uint32_t *destination = backbuffer + (uint32_t)(y + line) * framebuffer.width + x;
        for (int column = 0; column < width; ++column)
            destination[column] = row[column] & 0xFFFFFFU;
    }
}

/* Coverage blending is shared by glyphs, artwork and shape edges. */
uint32_t gfx_mix(uint32_t fg, uint32_t bg, unsigned int a)
{
    if (!a) return bg & 0xFFFFFF;
    if (a >= 255) return fg & 0xFFFFFF;
    unsigned int b = 255 - a;
    return (((((fg >> 16) & 255) * a + ((bg >> 16) & 255) * b + 127) / 255) << 16) |
           (((((fg >> 8) & 255) * a + ((bg >> 8) & 255) * b + 127) / 255) << 8) |
           (((fg & 255) * a + (bg & 255) * b + 127) / 255);
}

static void coverage_pixel(int x, int y, uint32_t color, unsigned int alpha)
{
    if (!framebuffer.pixels || x < 0 || y < 0 ||
        x >= (int)framebuffer.width || y >= (int)framebuffer.height || !alpha) return;
    uint32_t *pixel = backbuffer + y * framebuffer.width + x;
    *pixel = gfx_mix(color, *pixel, alpha);
}

void gfx_mask_scaled(int x, int y, int width, int height, const uint8_t *mask,
                     int sw, int sh, uint32_t fg, uint32_t bg)
{
    if (!framebuffer.pixels || !mask || width <= 0 || height <= 0 || sw <= 0 || sh <= 0) return;
    int left = x < 0 ? -x : 0, top = y < 0 ? -y : 0;
    int right = width, bottom = height;
    if ((int64_t)x + right > framebuffer.width) right = (int)framebuffer.width - x;
    if ((int64_t)y + bottom > framebuffer.height) bottom = (int)framebuffer.height - y;
    if (width == sw && height == sh) {
        for (int row = top; row < bottom; ++row)
            for (int col = left; col < right; ++col)
                backbuffer[(y + row) * framebuffer.width + x + col] =
                    gfx_mix(fg, bg == GFX_TRANSPARENT ?
                        backbuffer[(y + row) * framebuffer.width + x + col] : bg,
                        mask[row * sw + col]);
        return;
    }
    for (int row = top; row < bottom; ++row) {
        int64_t v = ((int64_t)row * (sh - 1) * 256) / (height > 1 ? height - 1 : 1);
        int sy = v >> 8, fy = v & 255, ny = sy + 1 < sh ? sy + 1 : sy;
        for (int col = left; col < right; ++col) {
            int64_t u = ((int64_t)col * (sw - 1) * 256) / (width > 1 ? width - 1 : 1);
            int sx = u >> 8, fx = u & 255, nx = sx + 1 < sw ? sx + 1 : sx;
            unsigned a = (mask[sy * sw + sx] * (256-fx) + mask[sy * sw + nx] * fx) * (256-fy) +
                         (mask[ny * sw + sx] * (256-fx) + mask[ny * sw + nx] * fx) * fy;
            backbuffer[(y + row) * framebuffer.width + x + col] = gfx_mix(fg,
                bg == GFX_TRANSPARENT ? backbuffer[(y + row) * framebuffer.width + x + col] : bg,
                (a + 32768) >> 16);
        }
    }
}

static void span(int64_t left, int64_t right, int y, uint32_t rgb)
{
    if (framebuffer.pixels == NULL || y < 0 || y >= (int)framebuffer.height ||
        left > right || right < 0 || left >= (int64_t)framebuffer.width)
        return;
    if (left < 0) left = 0;
    if (right >= (int64_t)framebuffer.width) right = framebuffer.width - 1;
    uint32_t *destination = backbuffer + (uint32_t)y * framebuffer.width;
    uint32_t color = rgb & 0xFFFFFFU;
    for (int x = (int)left; x <= (int)right; ++x)
        destination[x] = color;
}

/* Division used only when clipping shapes with very large coordinates.
 * Keeping it here avoids relying on the compiler emitting 64-bit division
 * helpers on every target. */
static uint32_t divide_wide(uint64_t numerator, uint32_t denominator)
{
    if (numerator <= UINT32_MAX)
        return (uint32_t)numerator / denominator;
    uint64_t quotient = 0, remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        remainder = (remainder << 1) | ((numerator >> bit) & 1U);
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient |= (uint64_t)1U << bit;
        }
    }
    return (uint32_t)quotient;
}

static uint32_t magnitude(int64_t value)
{
    return (uint32_t)(value < 0 ? -value : value);
}

/* Interpolate between integer endpoints without signed-product overflow. */
static int intersection(int start, int end, int base_start, int base_end, int boundary)
{
    int64_t delta = (int64_t)end - start;
    int64_t distance = (int64_t)boundary - base_start;
    int64_t denominator = (int64_t)base_end - base_start;
    if (denominator == 0)
        return start;
    uint64_t numerator = (uint64_t)magnitude(delta) * magnitude(distance);
    uint32_t fraction = divide_wide(numerator, magnitude(denominator));
    bool negative = (delta < 0) != (distance < 0);
    negative = negative != (denominator < 0);
    return (int)((int64_t)start + (negative ? -(int64_t)fraction : fraction));
}

static unsigned int outside(int x, int y)
{
    unsigned int result = 0;
    if (x < 0) result |= 1;
    else if (x >= (int)framebuffer.width) result |= 2;
    if (y < 0) result |= 4;
    else if (y >= (int)framebuffer.height) result |= 8;
    return result;
}

void gfx_line(int x1, int y1, int x2, int y2, uint32_t rgb)
{
    if (framebuffer.pixels == NULL)
        return;
    /* Clip before Bresenham so off-screen endpoints cannot cause long loops. */
    for (unsigned int attempt = 0; attempt < 16; ++attempt) {
        unsigned int first = outside(x1, y1), second = outside(x2, y2);
        if (!(first | second)) break;
        if (first & second) return;
        unsigned int code = first ? first : second;
        int x, y;
        if (code & 8) {
            y = (int)framebuffer.height - 1;
            x = intersection(x1, x2, y1, y2, y);
        } else if (code & 4) {
            y = 0;
            x = intersection(x1, x2, y1, y2, y);
        } else if (code & 2) {
            x = (int)framebuffer.width - 1;
            y = intersection(y1, y2, x1, x2, x);
        } else {
            x = 0;
            y = intersection(y1, y2, x1, x2, x);
        }
        if (code == first) { x1 = x; y1 = y; }
        else { x2 = x; y2 = y; }
    }
    if (outside(x1, y1) || outside(x2, y2)) return;
    int dx = x2 - x1, dy = y2 - y1;
    bool steep = (dy < 0 ? -dy : dy) > (dx < 0 ? -dx : dx);
    if (steep) { int t = x1; x1 = y1; y1 = t; t = x2; x2 = y2; y2 = t; }
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    dx = x2 - x1; dy = y2 - y1;
    for (int x = x1; x <= x2; ++x) {
        int64_t yf = (int64_t)y1 * 65536 + (dx ? (int64_t)dy * (x - x1) * 65536 / dx : 0);
        int y = (int)(yf >> 16), alpha = (int)((yf & 65535) * 255 / 65536);
        coverage_pixel(steep ? y : x, steep ? x : y, rgb, 255 - alpha);
        coverage_pixel(steep ? y + 1 : x, steep ? x : y + 1, rgb, alpha);
    }
}

static uint32_t square_root(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = (uint64_t)1U << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)result;
}

void gfx_circle(int center_x, int center_y, int radius, uint32_t rgb)
{
    if (framebuffer.pixels == NULL || radius < 0)
        return;
    int64_t top = (int64_t)center_y - radius;
    int64_t bottom = (int64_t)center_y + radius;
    if (bottom < 0 || top >= (int64_t)framebuffer.height) return;
    if (top < 0) top = 0;
    if (bottom >= (int64_t)framebuffer.height) bottom = framebuffer.height - 1;
    uint64_t squared_radius = (uint64_t)(uint32_t)radius * (uint32_t)radius;
    for (int y = (int)top; y <= (int)bottom; ++y) {
        uint32_t distance = magnitude((int64_t)y - center_y);
        uint64_t remaining = squared_radius - (uint64_t)distance * distance;
        if (radius > 1000000) { /* Keep extreme coordinates overflow-safe. */
            uint32_t extent = square_root(remaining);
            span((int64_t)center_x - extent, (int64_t)center_x + extent, y, rgb);
        } else {
            uint32_t extent256 = square_root(remaining * 65536);
            int extent = (int)(extent256 >> 8);
            span((int64_t)center_x - extent, (int64_t)center_x + extent, y, rgb);
            coverage_pixel(center_x - extent - 1, y, rgb, extent256 & 255);
            coverage_pixel(center_x + extent + 1, y, rgb, extent256 & 255);
        }
    }
}

static void swap(int *first, int *second)
{
    int temporary = *first;
    *first = *second;
    *second = temporary;
}

static void triangle_legacy(int x1, int y1, int x2, int y2, int x3, int y3, uint32_t rgb)
{
    if (framebuffer.pixels == NULL) return;
    if (y1 > y2) { swap(&y1, &y2); swap(&x1, &x2); }
    if (y2 > y3) { swap(&y2, &y3); swap(&x2, &x3); }
    if (y1 > y2) { swap(&y1, &y2); swap(&x1, &x2); }
    if (y3 < 0 || y1 >= (int)framebuffer.height) return;
    if (y1 == y3) {
        int left = x1 < x2 ? x1 : x2;
        int right = x1 > x2 ? x1 : x2;
        if (x3 < left) left = x3;
        if (x3 > right) right = x3;
        span(left, right, y1, rgb);
        return;
    }
    int top = y1 < 0 ? 0 : y1;
    int bottom = y3 >= (int)framebuffer.height ? (int)framebuffer.height - 1 : y3;
    for (int y = top; y <= bottom; ++y) {
        int left = intersection(x1, x3, y1, y3, y);
        int right;
        if (y < y2) right = intersection(x1, x2, y1, y2, y);
        else right = y2 == y3 ? x2 : intersection(x2, x3, y2, y3, y);
        if (left > right) swap(&left, &right);
        span(left, right, y, rgb);
    }
}

/* Four subpixel scanlines per row: coverage only, no larger framebuffer. */
void gfx_triangle(int x1, int y1, int x2, int y2, int x3, int y3, uint32_t rgb)
{
    if (!framebuffer.pixels) return;
    if (x1 < -1000000 || x1 > 1000000 || y1 < -1000000 || y1 > 1000000 ||
        x2 < -1000000 || x2 > 1000000 || y2 < -1000000 || y2 > 1000000 ||
        x3 < -1000000 || x3 > 1000000 || y3 < -1000000 || y3 > 1000000) {
        triangle_legacy(x1, y1, x2, y2, x3, y3, rgb); return;
    }
    if (y1 > y2) { swap(&y1, &y2); swap(&x1, &x2); }
    if (y2 > y3) { swap(&y2, &y3); swap(&x2, &x3); }
    if (y1 > y2) { swap(&y1, &y2); swap(&x1, &x2); }
    if (y1 == y3) { triangle_legacy(x1, y1, x2, y2, x3, y3, rgb); return; }
    int top = y1 < 0 ? 0 : y1;
    int bottom = y3 >= (int)framebuffer.height ? (int)framebuffer.height - 1 : y3;
    int min_x = x1 < x2 ? x1 : x2, max_x = x1 > x2 ? x1 : x2;
    if (x3 < min_x) min_x = x3;
    if (x3 > max_x) max_x = x3;
    if (min_x < 0) min_x = 0;
    if (max_x >= (int)framebuffer.width) max_x = (int)framebuffer.width - 1;
    if (min_x > max_x) return;
    uint16_t coverage[GFX_MAX_WIDTH];
    for (int y = top; y <= bottom; ++y) {
        for (int x = min_x; x <= max_x; ++x) coverage[x] = 0;
        for (int sample = 0; sample < 4; ++sample) {
            int sy = y * 8 + sample * 2 + 1;
            if (sy < y1 * 8 || sy >= y3 * 8) continue;
            int left = intersection(x1 * 256, x3 * 256, y1 * 8, y3 * 8, sy);
            int right = sy < y2 * 8 ?
                intersection(x1 * 256, x2 * 256, y1 * 8, y2 * 8, sy) :
                intersection(x2 * 256, x3 * 256, y2 * 8, y3 * 8, sy);
            if (left > right) swap(&left, &right);
            int first = left / 256, last = right / 256;
            if (first < min_x) first = min_x;
            if (last > max_x) last = max_x;
            for (int x = first; x <= last; ++x) {
                int l = left > x * 256 ? left : x * 256;
                int r = right < (x + 1) * 256 ? right : (x + 1) * 256;
                if (r > l) coverage[x] += (uint16_t)(r - l);
            }
        }
        for (int x = min_x; x <= max_x; ++x)
            coverage_pixel(x, y, rgb, (coverage[x] * 255U + 512) / 1024);
    }
}

void gfx_round_rect(int x, int y, int width, int height, int radius, uint32_t rgb)
{
    if (framebuffer.pixels == NULL || width <= 0 || height <= 0) return;
    if (radius < 0) radius = 0;
    int maximum_radius = (width < height ? width : height) / 2;
    if (radius > maximum_radius) radius = maximum_radius;
    if (radius == 0) { gfx_rect(x, y, width, height, rgb); return; }
    int64_t top = y, bottom = (int64_t)y + height - 1;
    if (bottom < 0 || top >= (int64_t)framebuffer.height) return;
    if (top < 0) top = 0;
    if (bottom >= (int64_t)framebuffer.height) bottom = framebuffer.height - 1;
    uint64_t squared_radius = (uint64_t)(uint32_t)radius * (uint32_t)radius;
    for (int line = (int)top; line <= (int)bottom; ++line) {
        int64_t offset = (int64_t)line - y;
        uint32_t inset = 0, edge = 0;
        if (offset < radius || offset >= (int64_t)height - radius) {
            uint32_t distance = offset < radius ? (uint32_t)(radius - offset) :
                (uint32_t)(offset - ((int64_t)height - radius - 1));
            uint64_t remaining = squared_radius - (uint64_t)distance * distance;
            uint32_t extent256 = square_root(remaining * 65536);
            inset = (uint32_t)radius - (extent256 >> 8);
            edge = extent256 & 255;
        }
        span((int64_t)x + inset, (int64_t)x + width - inset - 1, line, rgb);
        if (edge) {
            coverage_pixel(x + (int)inset - 1, line, rgb, edge);
            coverage_pixel(x + width - (int)inset, line, rgb, edge);
        }
    }
}

static void scaled_character(int x, int y, char character, uint32_t foreground,
                             uint32_t background, int scale)
{
    unsigned char glyph = (unsigned char)character;
    if (glyph < 32 || glyph > 126) glyph = '?';
    int atlas_scale = scale > 6 ? 6 : scale;
    unsigned int offset = 0;
    for (int size = 1; size < atlas_scale; ++size) offset += 95 * 128 * size * size;
    offset += (glyph - 32) * 128 * atlas_scale * atlas_scale;
    gfx_mask_scaled(x, y, 8 * scale, 16 * scale, nova_font_aa + offset,
                    8 * atlas_scale, 16 * atlas_scale, foreground, background);
}

void gfx_char(int x, int y, char character, uint32_t foreground, uint32_t background)
{
    scaled_character(x, y, character, foreground, background, 1);
}

void gfx_text(int x, int y, const char *text, uint32_t foreground, uint32_t background)
{
    gfx_text_scaled(x, y, text, foreground, background, 1);
}

void gfx_text_scaled(int x, int y, const char *text, uint32_t foreground,
                     uint32_t background, int scale)
{
    if (scale < 1 || scale > 32)
        return;
    if (text == NULL)
        return;
    int origin_x = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\n') {
            x = origin_x;
            if (y >= (int)framebuffer.height)
                return;
            y += GFX_CHAR_HEIGHT * scale;
        } else if (text[i] == '\r') {
            x = origin_x;
        } else {
            scaled_character(x, y, text[i], foreground, background, scale);
            if (x > INT32_MAX - GFX_CHAR_WIDTH * scale)
                return;
            x += GFX_CHAR_WIDTH * scale;
        }
    }
}
