#include "login.h"
#include "graphics.h"
#include "input.h"
#include "system.h"
#include "terminal.h"


#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { PASSWORD_CAPACITY = 64 };

static char password[PASSWORD_CAPACITY];
static unsigned int password_length;
static bool failed;
static bool ready_logged;

/* Password storage: a salted FNV-1a hash in /NOVA.PASS on the FAT32 disk.
 * Demo-grade only (no rainbow table concerns for a local hobby OS), but the
 * file never contains the plaintext. */
static uint32_t hash_password(const char *text)
{
    static const char salt[] = "nova-os-0.4:";
    uint32_t hash = 2166136261u;
    for (unsigned int i = 0; salt[i]; ++i) {
        hash ^= (uint8_t)salt[i];
        hash *= 16777619u;
    }
    for (unsigned int i = 0; text[i]; ++i) {
        hash ^= (uint8_t)text[i];
        hash *= 16777619u;
    }
    return hash;
}

bool login_verify(const char *candidate)
{
    if (!candidate) return false;
    FILE *file = fopen("/NOVA.PASS", "rb");
    if (!file) {
        return strcmp(candidate, "nova") == 0; /* fresh disk default */
    }
    char buffer[64];
    size_t got = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[got] = 0;
    if (strncmp(buffer, "hash=", 5) != 0) return false;
    uint32_t stored = 0;
    for (unsigned int i = 5; buffer[i] >= '0' && buffer[i] <= '9'; ++i)
        stored = stored * 10u + (uint32_t)(buffer[i] - '0');
    return stored == hash_password(candidate);
}

bool login_set_password(const char *candidate)
{
    if (!candidate || !candidate[0]) return false;
    char buffer[32];
    int length = snprintf(buffer, sizeof(buffer), "hash=%u\n",
                          hash_password(candidate));
    if (length <= 0) return false;
    FILE *file = fopen("/NOVA.PASS", "wb");
    if (!file) return false;
    fwrite(buffer, 1, (size_t)length, file);
    fclose(file);
    terminal_log("PASSWORD_CHANGED: nova\n");
    return true;
}

static bool password_matches(void)
{
    return login_verify(password);
}

static void clear_password(void)
{
    for (unsigned int i = 0; i < PASSWORD_CAPACITY; ++i) password[i] = 0;
    password_length = 0;
}

static unsigned int text_length(const char *text)
{
    unsigned int length = 0;
    while (text[length]) ++length;
    return length;
}

static void dashed_text(int x, int y, const char *text, uint32_t foreground,
                        uint32_t background)
{
    for (unsigned int i = 0; text[i]; ++i) {
        if (text[i] >= 32 && text[i] <= 126)
            gfx_char(x + (int)i * 8, y, text[i], foreground, background);
    }
}

static void centered_text(const char *text, int y, uint32_t foreground, int scale)
{
    int width = (int)gfx_width();
    int text_width = (int)text_length(text) * 8 * scale;
    int x = (width - text_width) / 2;
    if (x < 0) x = 0;
    if (scale == 1)
        dashed_text(x, y, text, foreground, GFX_TRANSPARENT);
    else
        gfx_text_scaled(x, y, text, foreground, GFX_TRANSPARENT, scale);
}

/* Caelestia SDDM "locklike" layout: the wallpaper fills the screen, a huge
 * clock sits at the top, and the avatar/username/password pill float in the
 * center with circular power buttons at the bottom right. */

static uint32_t topo_dim_frame[1280 * 800];
static void draw_topo_backdrop(int width, int height, uint32_t line, uint32_t base)
{
    static bool built;
    static uint32_t last_line, last_base;
    if (!built || last_line != line || last_base != base) {
        for (int i = 0; i < 1280 * 800; ++i)
            topo_dim_frame[i] = gfx_mix(line, base, nova_topo_aa[i]);
        last_line = line; last_base = base; built = true;
    }
    if (width == 1280 && height == 800) gfx_blit(0, 0, width, height, topo_dim_frame);
    else gfx_mask_scaled(0, 0, width, height, nova_topo_aa, 1280, 800, line, base);
}

static void power_button(int cx, int cy, int kind, uint32_t foreground)
{
    gfx_circle(cx, cy, 24, 0x141C30);
    gfx_circle(cx, cy, 24, foreground);
    gfx_circle(cx, cy, 21, 0x141C30);
    if (kind == 0) { /* suspend: crescent moon */
        gfx_circle(cx, cy - 1, 10, foreground);
        gfx_circle(cx + 4, cy - 3, 9, 0x141C30);
    } else if (kind == 1) { /* restart: circular arrow */
        for (int i = 0; i < 8; ++i)
            gfx_rect(cx + (i % 4 == 0 ? 6 : 2) - 1, cy - 10 + i * 3, 8, 2, foreground);
        gfx_triangle(cx + 2, cy - 12, cx + 12, cy - 8, cx + 2, cy - 4, foreground);
    } else { /* shutdown: power glyph (arc points on a 11px radius). */
        static const int arc[9][2] = {
            { -8, -8 }, { -5, -10 }, { -2, -11 }, { 1, -11 }, { 4, -10 },
            { 7, -8 }, { 9, -5 }, { 10, -1 }, { 10, 2 }
        };
        gfx_rect(cx - 1, cy - 12, 3, 12, foreground);
        for (unsigned int i = 0; i < sizeof(arc) / sizeof(arc[0]); ++i)
            gfx_rect(cx + arc[i][0], cy + arc[i][1], 2, 2, foreground);
    }
}

static void draw_greeter(void)
{
    int width = (int)gfx_width();
    int height = (int)gfx_height();
    int center_x = width / 2;
    /* Dimmed topo wallpaper (like a blurred lock screen). */
    draw_topo_backdrop(width, height, 0x3A3F4E, 0x050505);

    /* Huge clock and date at the top. */
    char time[6];
    system_time(time);
    centered_text(time, 54, 0xECF4FF, 6);
    char date[24];
    system_date(date);
    centered_text(date, 54 + 96 + 12, 0x90A6C4, 1);

    /* Avatar: large soft disc with an accent ring and a star. */
    int avatar_y = height / 2 - 86;
    gfx_circle(center_x, avatar_y, 52, 0x89B4FA);
    gfx_circle(center_x, avatar_y, 49, 0x0B1220);
    gfx_circle(center_x, avatar_y, 44, 0x1B2946);
    gfx_triangle(center_x, avatar_y - 20, center_x - 7, avatar_y + 6,
                 center_x + 7, avatar_y + 6, 0x8BE9F9);
    gfx_triangle(center_x, avatar_y + 20, center_x - 7, avatar_y - 6,
                 center_x + 7, avatar_y - 6, 0x6B8DFF);

    centered_text("nova", avatar_y + 60, 0xECF4FF, 2);

    /* Password pill (dots) and Sign in button. */
    int field_width = 300;
    int field_x = center_x - field_width / 2;
    int field_y = avatar_y + 120;
    gfx_round_rect(field_x, field_y, field_width, 46, 23, 0x141C30);
    gfx_round_rect(field_x + 1, field_y + 1, field_width - 2, 44, 22, 0x1B2946);
    for (unsigned int i = 0; i < password_length && i < 20; ++i) {
        int dot_x = field_x + 18 + (int)i * 14;
        gfx_circle(dot_x + 4, field_y + 23, 5, 0xD5DFEF);
    }
    if (password_length == 0)
        dashed_text(field_x + 18, field_y + 15, "Password", 0x7A8FAD, 0x1B2946);
    if ((system_ms() / 500) % 2 == 0) {
        int caret_x = field_x + 18 + (int)password_length * 14;
        if (caret_x < field_x + field_width - 12)
            gfx_rect(caret_x, field_y + 12, 2, 22, 0x89B4FA);
    }

    centered_text("Press Enter to sign in", field_y + 58, 0x7088AB, 1);
    if (failed)
        centered_text("Incorrect password — try again", field_y + 82, 0xF38BA8, 1);

    /* Circular power buttons, bottom right (Caelestia lockscreen style). */
    int button_y = height - 44;
    power_button(width - 132, button_y, 0, 0xCDD6F4);
    power_button(width - 80, button_y, 1, 0x89B4FA);
    power_button(width - 28, button_y, 2, 0xF38BA8);

    centered_text("User: nova   Password: nova", height - 34, 0x7088AB, 1);
    gfx_present();
}

void login_run(void)
{
    if (!ready_logged) {
        terminal_log("LOGIN_READY\n");
        ready_logged = true;
    }
    clear_password();
    failed = false;
    for (;;) {
        draw_greeter();
        InputEvent event;
        while (input_poll(&event)) {
            if (event.type == INPUT_KEY) {
                if (event.pressed) {
                    if (event.key == '\n' || event.key == '\r') {
                        if (password_matches()) {
                            terminal_log("LOGIN_SUCCESS: nova\n");
                            clear_password();
                            return;
                        }
                        terminal_log("LOGIN_FAIL\n");
                        failed = true;
                        clear_password();
                    } else if (event.key == '\b' || event.key == 127) {
                        if (password_length) {
                            password[--password_length] = 0;
                        }
                    } else if (event.key >= 32 && event.key <= 126 &&
                               password_length + 1 < PASSWORD_CAPACITY) {
                        password[password_length++] = (char)event.key;
                        password[password_length] = 0;
                        failed = false;
                    }
                }
            } else if (event.type == INPUT_MOUSE) {
                int width = (int)gfx_width();
                int height = (int)gfx_height();
                /* Power buttons, bottom right. */
                int button_y = height - 44;
                if (event.buttons & 1) {
                    int hit = -1;
                    for (int i = 0; i < 3; ++i) {
                        int cx = width - 132 + i * 52;
                        int dx = event.x - cx, dy = event.y - button_y;
                        if (dx * dx + dy * dy <= 24 * 24) hit = i;
                    }
                    if (hit == 0) terminal_log("LOGIN_POWER: sleep\n");
                    else if (hit == 1) terminal_log("LOGIN_POWER: restart\n");
                    else if (hit == 2) terminal_log("LOGIN_POWER: shutdown\n");
                }
            }
        }
        system_wait(16);
    }
}
