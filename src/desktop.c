#include "desktop.h"
#include "apps.h"
#include "process.h"
#include "doom_nova.h"
#include "graphics.h"
#include "network.h"
#include "system.h"
#include "terminal.h"

#include <stddef.h>
#include <stdio.h>

/* The desktop keeps one instance of each app; all state lives in the kernel. */
enum { TITLE_HEIGHT = 38, PANEL_HEIGHT = 56, PANEL_MARGIN = 12, TOP_BAR = 30 };
typedef struct {
    bool open, minimized, maximized;
    UiRect rectangle, restore;
} NovaWindow;

static NovaWindow windows[APP_COUNT];
static AppId stacking[APP_COUNT];
static AppId focused = APP_COUNT;
/* A Catppuccin-flavoured palette in the spirit of riced GNOME shells. */
static UiTheme theme = { 0x1B1B2E, 0x27273A, 0x32324A, 0xCDD6F4,
                         0x9399B2, 0x45475A, 0x89B4FA, true };
static int screen_width, screen_height;
static int mouse_x, mouse_y;
static uint8_t mouse_buttons;
static int dragging = -1, drag_x, drag_y;
static bool painting;
/* Set while DOOM's mouse-fire key is held down (content click -> release). */
static bool doom_firing;
static bool dirty, launcher;
static int launcher_selection, launcher_page;
static int pointer_capture = -1;
static int key_owner[512];
static bool snow;
static unsigned int snow_seed;
static void snow_reset(void);
static void settings_save(void);
static void settings_load(void);
/* Display preferences managed from Settings. */
enum { WALLPAPER_AURORA, WALLPAPER_ALPINE, WALLPAPER_SKYLINE, WALLPAPER_SUNSET,
       WALLPAPER_OCEAN, WALLPAPER_GALAXY, WALLPAPER_TOPO, WALLPAPER_COUNT };
static const char *const wallpaper_names[WALLPAPER_COUNT] = {
    "Aurora", "Alpine", "Skyline", "Sunset", "Ocean", "Galaxy", "Topo"
};
static int wallpaper_id;
static bool widgets_clock = true, widgets_battery = true;
static bool icons_visible = true;
static bool clock_24h = true;
static char shown_clock[16];
static int shown_battery = -1;

enum { SNOW_FLAKES = 170 };
struct snow_flake { int x, y, speed, size, sway; unsigned int phase; };
static struct snow_flake flakes[SNOW_FLAKES];

static int smaller(int a, int b) { return a < b ? a : b; }
static int clamp(int value, int lower, int upper)
{
    if (upper < lower) return lower;
    return value < lower ? lower : value > upper ? upper : value;
}

static bool contains(UiRect rectangle, int x, int y)
{
    return x >= rectangle.x && y >= rectangle.y &&
           x < rectangle.x + rectangle.width && y < rectangle.y + rectangle.height;
}

static void decimal(char *buffer, int number)
{
    char reversed[12];
    unsigned int count = 0, used = 0;
    if (number < 0) { buffer[used++] = '-'; number = -number; }
    do { reversed[count++] = (char)('0' + number % 10); number /= 10; } while (number);
    while (count) buffer[used++] = reversed[--count];
    buffer[used] = 0;
}

static void log_window(const char *action, AppId app)
{
    terminal_log(action);
    terminal_log(apps_title(app));
    terminal_log("\n");
}

static void log_geometry(AppId app)
{
    UiRect r = windows[app].rectangle;
    char digits[12];
    terminal_log("WINDOW_GEOMETRY: "); terminal_log(apps_title(app)); terminal_log(" ");
    decimal(digits, r.x); terminal_log(digits); terminal_log(",");
    decimal(digits, r.y); terminal_log(digits); terminal_log(",");
    decimal(digits, r.width); terminal_log(digits); terminal_log(",");
    decimal(digits, r.height); terminal_log(digits); terminal_log("\n");
}

void desktop_mark_dirty(void) { dirty = true; }
const UiTheme *desktop_theme(void) { return &theme; }

void desktop_set_dark(bool dark)
{
    uint32_t accent = theme.accent ? theme.accent : 0x89B4FA;
    theme = dark ? (UiTheme){ 0x1B1B2E, 0x27273A, 0x32324A, 0xCDD6F4,
                             0x9399B2, 0x45475A, accent, true }
                 : (UiTheme){ 0xC9DDF0, 0xFBFCFE, 0xEDF0F6, 0x1E2530,
                             0x65707F, 0xCFD6E2, accent, false };
    dirty = true;
    terminal_log(dark ? "THEME: dark\n" : "THEME: light\n");
    settings_save();
}

void desktop_set_accent(uint32_t accent)
{
    theme.accent = accent & 0xFFFFFF;
    dirty = true;
    terminal_log("THEME_ACCENT: changed\n");
    settings_save();
}

void desktop_set_snow(bool on)
{
    snow = on;
    dirty = true;
    terminal_log(on ? "SNOW: on\n" : "SNOW: off\n");
    settings_save();
}

bool desktop_snow(void) { return snow; }

static void text_copy(char *to, const char *from)
{
    do { *to++ = *from; } while (*from++);
}

static bool text_equal(const char *left, const char *right)
{
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

/* The dock, top bar and clock widget share one formatted time string. */
static void clock_text(char *buffer)
{
    char time_value[6];
    system_time(time_value);
    int hour = (time_value[0] - '0') * 10 + time_value[1] - '0';
    if (clock_24h) {
        text_copy(buffer, time_value);
        return;
    }
    int twelve = hour % 12;
    if (twelve == 0) twelve = 12;
    int used = 0;
    if (twelve >= 10) buffer[used++] = (char)('0' + twelve / 10);
    buffer[used++] = (char)('0' + twelve % 10);
    buffer[used++] = ':';
    buffer[used++] = time_value[3];
    buffer[used++] = time_value[4];
    buffer[used++] = ' ';
    buffer[used++] = hour < 12 ? 'A' : 'P';
    buffer[used++] = 'M';
    buffer[used] = 0;
}

int desktop_wallpaper(void) { return wallpaper_id; }
int desktop_wallpaper_count(void) { return WALLPAPER_COUNT; }

const char *desktop_wallpaper_name(int wallpaper)
{
    return wallpaper >= 0 && wallpaper < WALLPAPER_COUNT ?
           wallpaper_names[wallpaper] : "";
}

void desktop_set_wallpaper(int wallpaper)
{
    if (wallpaper < 0 || wallpaper >= WALLPAPER_COUNT || wallpaper == wallpaper_id)
        return;
    wallpaper_id = wallpaper;
    dirty = true;
    terminal_log("WALLPAPER: ");
    terminal_log(wallpaper_names[wallpaper]);
    terminal_log("\n");
    settings_save();
}

bool desktop_clock_widget(void) { return widgets_clock; }

void desktop_set_clock_widget(bool visible)
{
    widgets_clock = visible;
    dirty = true;
    terminal_log(visible ? "WIDGET: clock on\n" : "WIDGET: clock off\n");
    settings_save();
}

bool desktop_battery_widget(void) { return widgets_battery; }

void desktop_set_battery_widget(bool visible)
{
    widgets_battery = visible;
    dirty = true;
    terminal_log(visible ? "WIDGET: battery on\n" : "WIDGET: battery off\n");
    settings_save();
}

bool desktop_icons_visible(void) { return icons_visible; }

void desktop_set_icons(bool visible)
{
    icons_visible = visible;
    dirty = true;
    terminal_log(visible ? "ICONS: on\n" : "ICONS: off\n");
    settings_save();
}

bool desktop_clock_24h(void) { return clock_24h; }

void desktop_set_clock_24h(bool use_24h)
{
    clock_24h = use_24h;
    dirty = true;
    terminal_log(use_24h ? "CLOCK: 24h\n" : "CLOCK: 12h\n");
    settings_save();
}

/* ------------------------------------------------- persistent preferences -- */

static void settings_save(void)
{
    char buffer[192];
    int length = snprintf(buffer, sizeof(buffer),
        "wallpaper=%d\nsnow=%d\ndark=%d\naccent=%06X\nclock=%d\nbattery=%d\nicons=%d\nclock24=%d\n",
        wallpaper_id, snow ? 1 : 0, theme.dark ? 1 : 0,
        theme.accent & 0xFFFFFFu, widgets_clock ? 1 : 0,
        widgets_battery ? 1 : 0, icons_visible ? 1 : 0, clock_24h ? 1 : 0);
    if (length <= 0) return;
    FILE *file = fopen("/NOVA.CFG", "wb");
    if (!file) return;
    fwrite(buffer, 1, (size_t)length, file);
    fclose(file);
}

static bool starts_with(const char *text, const char *prefix)
{
    while (*prefix) {
        if (*text != *prefix) return false;
        ++text;
        ++prefix;
    }
    return true;
}

static bool parse_key_int(const char *text, const char *key, int *out)
{
    size_t key_length = 0;
    while (key[key_length]) ++key_length;
    const char *cursor = text;
    while (*cursor) {
        if (starts_with(cursor, key) && cursor[key_length] == '=') {
            const char *digits = cursor + key_length + 1;
            int value = 0, sign = 1;
            bool any = false;
            if (*digits == '-') { sign = -1; ++digits; }
            while (*digits >= '0' && *digits <= '9') {
                value = value * 10 + (*digits - '0');
                any = true;
                ++digits;
            }
            if (!any) return false;
            *out = sign * value;
            return true;
        }
        while (*cursor && *cursor != '\n') ++cursor;
        if (*cursor == '\n') ++cursor;
    }
    return false;
}

static bool parse_key_hex(const char *text, const char *key, uint32_t *out)
{
    size_t key_length = 0;
    while (key[key_length]) ++key_length;
    const char *cursor = text;
    while (*cursor) {
        if (starts_with(cursor, key) && cursor[key_length] == '=') {
            const char *digits = cursor + key_length + 1;
            uint32_t value = 0;
            bool any = false;
            while ((*digits >= '0' && *digits <= '9') ||
                   (*digits >= 'A' && *digits <= 'F') ||
                   (*digits >= 'a' && *digits <= 'f')) {
                uint32_t digit;
                if (*digits >= '0' && *digits <= '9') digit = (uint32_t)(*digits - '0');
                else if (*digits >= 'A' && *digits <= 'F') digit = (uint32_t)(*digits - 'A' + 10);
                else digit = (uint32_t)(*digits - 'a' + 10);
                value = value * 16 + digit;
                any = true;
                ++digits;
            }
            if (!any) return false;
            *out = value & 0xFFFFFFu;
            return true;
        }
        while (*cursor && *cursor != '\n') ++cursor;
        if (*cursor == '\n') ++cursor;
    }
    return false;
}

static void settings_load(void)
{
    char buffer[512];
    FILE *file = fopen("/NOVA.CFG", "rb");
    if (file) {
        size_t got = fread(buffer, 1, sizeof(buffer) - 1, file);
        fclose(file);
        buffer[got] = 0;
    } else {
        buffer[0] = 0;
    }
    int value;
    if (parse_key_int(buffer, "wallpaper", &value) &&
        value >= 0 && value < WALLPAPER_COUNT)
        wallpaper_id = value;
    if (parse_key_int(buffer, "snow", &value)) snow = value != 0;
    if (parse_key_int(buffer, "clock", &value)) widgets_clock = value != 0;
    if (parse_key_int(buffer, "battery", &value)) widgets_battery = value != 0;
    if (parse_key_int(buffer, "icons", &value)) icons_visible = value != 0;
    if (parse_key_int(buffer, "clock24", &value)) clock_24h = value != 0;
    uint32_t accent = theme.accent ? theme.accent : 0x89B4FA;
    parse_key_hex(buffer, "accent", &accent);
    if (parse_key_int(buffer, "dark", &value)) {
        bool dark = value != 0;
        theme = dark ? (UiTheme){ 0x1B1B2E, 0x27273A, 0x32324A, 0xCDD6F4,
                                  0x9399B2, 0x45475A, accent, true }
                     : (UiTheme){ 0xC9DDF0, 0xFBFCFE, 0xEDF0F6, 0x1E2530,
                                  0x65707F, 0xCFD6E2, accent, false };
    } else if (accent != 0x89B4FA) {
        theme.accent = accent;
    }
    char digits[12];
    terminal_log("PERSIST_SETTINGS: wallpaper=");
    decimal(digits, wallpaper_id); terminal_log(digits);
    terminal_log(" snow="); decimal(digits, snow ? 1 : 0); terminal_log(digits);
    terminal_log(" dark="); decimal(digits, theme.dark ? 1 : 0); terminal_log(digits);
    terminal_log("\n");
}

static void raise_window(AppId app)
{
    int position = 0;
    while (position < APP_COUNT && stacking[position] != app) ++position;
    for (int i = position; i < APP_COUNT - 1; ++i) stacking[i] = stacking[i + 1];
    stacking[APP_COUNT - 1] = app;
    if (focused != app) {
        process_app_focus(focused,false);
        focused = app;
        process_app_focus(app,true);
        log_window("WINDOW_FOCUS: ", app);
    }
    dirty = true;
}

static void focus_available(void)
{
    process_app_focus(focused,false);
    focused = APP_COUNT;
    for (int i = APP_COUNT - 1; i >= 0; --i) {
        AppId app = stacking[i];
        if (windows[app].open && !windows[app].minimized) {
            focused = app;
            process_app_focus(app,true);
            log_window("WINDOW_FOCUS: ", app);
            break;
        }
    }
}

void desktop_open(AppId app)
{
    if (app < 0 || app >= APP_COUNT) return;
    NovaWindow *window = &windows[app];
    process_app_ensure(app,window->rectangle.width-2,window->rectangle.height-TITLE_HEIGHT-2);
    if (!window->open) {
        window->open = true;
        window->minimized = false;
        log_window("WINDOW_OPEN: ", app);
        log_geometry(app);
    } else if (window->minimized) {
        window->minimized = false;
        log_window("WINDOW_RESTORE: ", app);
        log_geometry(app);
    }
    launcher = false;
    raise_window(app);
}

static void minimize_window(AppId app)
{
    windows[app].minimized = true;
    if (focused == app) focus_available();
    log_window("WINDOW_MINIMIZE: ", app);
    dirty = true;
}

static void close_window(AppId app)
{
    process_app_close(app);
    windows[app].open = false;
    windows[app].minimized = false;
    if (focused == app) focus_available();
    log_window("WINDOW_CLOSE: ", app);
    dirty = true;
}

static void maximize_window(AppId app)
{
    NovaWindow *window = &windows[app];
    if (window->maximized) {
        window->rectangle = window->restore;
        window->maximized = false;
        log_window("WINDOW_RESTORE: ", app);
    } else {
        window->restore = window->rectangle;
        /* Maximized windows sit just below the GNOME-style top bar. */
        window->rectangle = (UiRect){ 8, TOP_BAR + 6, screen_width - 16,
                                      screen_height - TOP_BAR - 76 };
        window->maximized = true;
        log_window("WINDOW_MAXIMIZE: ", app);
    }
    log_geometry(app);
    process_app_resize(app,window->rectangle.width-2,window->rectangle.height-TITLE_HEIGHT-2);
    dirty = true;
}

bool desktop_process_focused(AppId app){return app>=0 && app<APP_COUNT && focused==app;}
bool desktop_process_visible(AppId app){return app>=0 && app<APP_COUNT && windows[app].open && !windows[app].minimized;}
void desktop_process_exited(AppId app){if(app<0 || app>=APP_COUNT)return;windows[app].open=false;windows[app].minimized=false;if(focused==app)focus_available();dirty=true;}
void desktop_external_rectangle(AppId app,int width,int height){if(app<APP_BUILTIN_COUNT || app>=APP_COUNT)return;width=clamp(width,200,screen_width-40);height=clamp(height,150,screen_height-140);windows[app].rectangle=(UiRect){100+(app%4)*35,90+(app%3)*30,width+2,height+40};}

static UiRect initial_rectangle(int x, int y, int width, int height)
{
    width = smaller(width, screen_width - 40);
    height = smaller(height, screen_height - 100);
    x = clamp(x, 12, screen_width - width - 12);
    y = clamp(y, TOP_BAR + 6, screen_height - height - 82);
    return (UiRect){ x, y, width, height };
}

void desktop_init(void)
{
    screen_width = (int)gfx_width();
    screen_height = (int)gfx_height();
    mouse_x = screen_width / 2;
    mouse_y = screen_height / 2;
    mouse_buttons = 0;
    dragging = -1;
    painting = false;
    launcher = false;
    launcher_selection = launcher_page = 0;
    for(int i=0;i<512;++i)key_owner[i]=-1;
    snow = false;
    focused = APP_COUNT;
    for (int i = 0; i < APP_COUNT; ++i) {
        windows[i] = (NovaWindow){ 0 };
        stacking[i] = (AppId)i;
        windows[i].rectangle=initial_rectangle(100+(i%4)*35,90+(i%3)*30,642,440);
    }
    windows[APP_TERMINAL].rectangle = initial_rectangle(84, 64, 720, 540);
    windows[APP_NOTES].rectangle = initial_rectangle(screen_width - 430, 130, 400, 370);
    windows[APP_CALCULATOR].rectangle = initial_rectangle(screen_width - 330,
                                                        screen_height - 440, 302, 350);
    windows[APP_FILES].rectangle = initial_rectangle(150, 112, 620, 430);
    windows[APP_ABOUT].rectangle = initial_rectangle(280, 180, 470, 330);
    windows[APP_BROWSER].rectangle = initial_rectangle(120, 72, 780, 560);
    windows[APP_NETWORK].rectangle = initial_rectangle(screen_width - 470, 160, 440, 330);
    windows[APP_CLOCK].rectangle = initial_rectangle(screen_width - 350, 200, 320, 300);
    windows[APP_PAINT].rectangle = initial_rectangle(260, 170, 560, 400);
    windows[APP_SNAKE].rectangle = initial_rectangle(screen_width - 500, 96, 460, 420);
    windows[APP_TICTAC].rectangle = initial_rectangle(screen_width - 370, 110, 340, 400);
    windows[APP_MINES].rectangle = initial_rectangle(screen_width - 390,
                                                    screen_height - 490, 360, 420);
    windows[APP_SYSMON].rectangle = initial_rectangle(200, 210, 540, 380);
    windows[APP_CAR].rectangle = initial_rectangle(screen_width - 470, 96, 430, 470);
    /* 962x640 puts DOOM's 960x600 view at (160, 95) on a 1280x800 screen. */
    windows[APP_DOOM].rectangle = initial_rectangle(screen_width / 2 - 481, 56, 962, 640);
    windows[APP_SETTINGS].rectangle = initial_rectangle(330, 130, 640, 640);
    wallpaper_id = WALLPAPER_TOPO;
    widgets_clock = true;
    widgets_battery = true;
    icons_visible = true;
    clock_24h = true;
    shown_clock[0] = 0;
    shown_battery = -1;
    snow_reset();
    /* Load persisted preferences over the defaults, then announce the theme
     * (the setter also saves it back once, which is harmless). */
    settings_load();
    desktop_set_dark(theme.dark);
    {
        char digits[12];
        decimal(digits, (int)system_battery_percent());
        terminal_log("BATTERY: ");
        terminal_log(digits);
        terminal_log("%\n");
    }
    desktop_open(APP_TERMINAL);
    dirty = true;
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned int amount)
{
    unsigned int inverse = 256 - amount;
    return ((((a >> 16) & 255) * inverse + ((b >> 16) & 255) * amount) >> 8) << 16 |
           ((((a >> 8) & 255) * inverse + ((b >> 8) & 255) * amount) >> 8) << 8 |
           (((a & 255) * inverse + (b & 255) * amount) >> 8);
}

static void star(int x, int y, int radius, uint32_t color)
{
    int middle = radius / 3;
    if (middle < 1) middle = 1;
    gfx_triangle(x, y - radius, x - middle, y, x + middle, y, color);
    gfx_triangle(x, y + radius, x - middle, y, x + middle, y, color);
    gfx_triangle(x - radius, y, x, y - middle, x, y + middle, color);
    gfx_triangle(x + radius, y, x, y - middle, x, y + middle, color);
}

static void icon(AppId app, int x, int y, int size, uint32_t color, uint32_t background)
{
    int half = size / 2;
    if (app == APP_TERMINAL) {
        gfx_round_rect(x - half, y - half + 2, size, size - 4, 4, 0x152232);
        gfx_line(x - half + 6, y - 5, x - half + 11, y, color);
        gfx_line(x - half + 11, y, x - half + 6, y + 5, color);
        gfx_rect(x, y + 5, half - 5, 2, color);
    } else if (app == APP_NOTES) {
        gfx_round_rect(x - half + 3, y - half, size - 6, size, 3, 0xF2D78C);
        for (int i = 0; i < 3; ++i)
            gfx_rect(x - half + 8, y - 7 + i * 6, size - 16, 1, 0x8D773D);
    } else if (app == APP_CALCULATOR) {
        gfx_round_rect(x - half + 3, y - half, size - 6, size, 4, 0x8DC3EF);
        gfx_rect(x - half + 7, y - half + 5, size - 14, 6, 0x244564);
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col)
                gfx_rect(x - half + 7 + col * 6, y + row * 6, 3, 3, 0x345A7A);
    } else if (app == APP_FILES) {
        gfx_round_rect(x - half, y - half + 5, size, size - 7, 4, 0x6DC5F4);
        gfx_rect(x - half + 2, y - half + 1, half - 1, 7, 0xA6E2FF);
        gfx_rect(x - half + 2, y - half + 10, size - 4, 2, 0xA6E2FF);
    } else if (app == APP_SETTINGS) {
        for (int i = -1; i <= 1; i += 2) {
            gfx_rect(x - 3, y + i * (half - 2) - 3, 6, 6, 0xC6B7F5);
            gfx_rect(x + i * (half - 2) - 3, y - 3, 6, 6, 0xC6B7F5);
        }
        gfx_circle(x, y, half - 3, 0xC6B7F5);
        gfx_circle(x, y, half - 9, background);
    } else if (app == APP_BROWSER) {
        gfx_circle(x, y, half - 1, 0x70C8F5);
        gfx_circle(x, y, half - 3, background);
        gfx_line(x - half + 3, y, x + half - 3, y, 0x70C8F5);
        gfx_line(x, y - half + 2, x, y + half - 2, 0x70C8F5);
        gfx_line(x - half / 3, y - half + 4, x - half / 3, y + half - 4, 0x70C8F5);
        gfx_line(x + half / 3, y - half + 4, x + half / 3, y + half - 4, 0x70C8F5);
    } else if (app == APP_NETWORK) {
        gfx_line(x, y - half + 4, x, y + 2, 0x8FE0BA);
        gfx_line(x - half + 4, y + 2, x + half - 4, y + 2, 0x8FE0BA);
        gfx_line(x - half + 4, y + 2, x - half + 4, y + half - 4, 0x8FE0BA);
        gfx_line(x + half - 4, y + 2, x + half - 4, y + half - 4, 0x8FE0BA);
        gfx_circle(x, y - half + 4, 3, 0x8FE0BA);
        gfx_circle(x - half + 4, y + half - 3, 3, 0x8FE0BA);
        gfx_circle(x + half - 4, y + half - 3, 3, 0x8FE0BA);
    } else if (app == APP_CLOCK) {
        gfx_circle(x, y, half - 1, 0xD8E5F6);
        gfx_circle(x, y, half - 3, 0x344B68);
        gfx_line(x, y, x, y - half + 5, 0xECF5FF);
        gfx_line(x, y, x + half - 5, y + 2, 0xECF5FF);
        gfx_circle(x, y, 1, 0xF2C278);
    } else if (app == APP_PAINT) {
        gfx_round_rect(x - half + 1, y - half + 1, size - 2, size - 2, 4, 0xE7EAF1);
        gfx_circle(x - half + 7, y - half + 7, 3, 0xE36C8B);
        gfx_circle(x + half - 7, y - half + 7, 3, 0x62B9E9);
        gfx_circle(x - half + 7, y + half - 7, 3, 0x81C890);
        gfx_line(x - 1, y + half - 5, x + half - 4, y - 1, 0x7A5674);
        gfx_line(x, y + half - 5, x + half - 3, y - 1, 0x7A5674);
    } else if (app == APP_SNAKE) {
        gfx_round_rect(x - half, y - half, size, size, 5, 0x1C2A1E);
        gfx_rect(x - half + 6, y - half + 7, 9, 5, 0x9ECD6B);
        gfx_rect(x - half + 10, y - half + 7, 5, 11, 0x9ECD6B);
        gfx_rect(x - half + 10, y + half - 11, 9, 5, 0x7FB555);
        gfx_circle(x + half - 7, y - half + 9, 3, 0xE9708F);
    } else if (app == APP_TICTAC) {
        gfx_round_rect(x - half, y - half, size, size, 5, 0x23324E);
        gfx_rect(x - 1, y - half + 4, 2, size - 8, 0x5B7BB5);
        gfx_rect(x - half + 4, y - 1, size - 8, 2, 0x5B7BB5);
        gfx_line(x - half + 5, y - half + 5, x - half + 11, y + half - 5, 0x89B4FA);
        gfx_line(x - half + 5, y + half - 5, x - half + 11, y - half + 5, 0x89B4FA);
        gfx_circle(x + half - 8, y - half + 8, 4, 0xF38BB9);
    } else if (app == APP_MINES) {
        gfx_round_rect(x - half, y - half, size, size, 5, 0x2A2A3A);
        gfx_line(x, y - half + 4, x, y + half - 4, 0xCDD6F4);
        gfx_line(x - half + 4, y, x + half - 4, y, 0xCDD6F4);
        gfx_circle(x, y, 5, 0x151521);
        gfx_circle(x - 1, y - 1, 2, 0xE5EDF8);
    } else if (app == APP_SYSMON) {
        gfx_round_rect(x - half, y - half, size, size, 5, 0x20304A);
        gfx_rect(x - half + 5, y + 1, 4, half - 4, 0x89B4FA);
        gfx_rect(x - half + 11, y - 6, 4, half + 1, 0x9ECD6B);
        gfx_rect(x - half + 17, y - 11, 4, half + 6, 0xF3C76D);
    } else if (app == APP_CAR) {
        /* A little red racer seen from the side. */
        gfx_round_rect(x - half, y - 2, size, size / 2, 4, 0xE06C75);
        gfx_round_rect(x - half + size / 6, y - size / 2 + 2, size / 2 + 2,
                       size / 3, 4, 0xF2B8B5);
        gfx_rect(x - half + size / 3, y - size / 4, size / 3, 2, 0x2E5F86);
        int wheel = size / 8 + 1;
        gfx_circle(x - half + size / 4, y + size / 4, wheel, 0x161620);
        gfx_circle(x + half - size / 4, y + size / 4, wheel, 0x161620);
        gfx_rect(x + half - 2, y + 1, 2, 3, 0xF7DC6F);
    } else if (app == APP_DOOM) {
        /* A red slab with demon eyes: the DOOM logo in miniature. */
        gfx_round_rect(x - half, y - half, size, size, 5, 0x8E1B1B);
        gfx_rect(x - half + 3, y - half + 4, size - 6, 3, 0xD43A2F);
        gfx_circle(x - half + size / 3, y, size / 6 + 1, 0xF0D9A0);
        gfx_circle(x + half - size / 3, y, size / 6 + 1, 0xF0D9A0);
        gfx_circle(x - half + size / 3, y, size / 12, 0x2A0A0A);
        gfx_circle(x + half - size / 3, y, size / 12, 0x2A0A0A);
        gfx_rect(x - half + 4, y + half - 6, size - 8, 3, 0x3A0D0D);
    } else {
        gfx_circle(x, y, half - 1, color);
        gfx_text(x - 4, y - 8, "i", background, color);
    }
}

/* ---- Six procedural wallpapers: no image decoder, pure drawing code ---- */

static unsigned int scene_seed;

/* xorshift32, reseeded per frame so scenes stay deterministic. */
static unsigned int scene_random(void)
{
    scene_seed ^= scene_seed << 13;
    scene_seed ^= scene_seed >> 17;
    scene_seed ^= scene_seed << 5;
    return scene_seed;
}

static int scene_chance(int bound)
{
    return (int)(scene_random() % (unsigned int)bound);
}

/* Vertical two-colour gradient across a band of rows. */
static void gradient_rows(int y0, int y1, uint32_t top, uint32_t bottom)
{
    int span = y1 - y0;
    if (span <= 0) return;
    for (int y = y0; y < y1; ++y)
        gfx_rect(0, y, screen_width, 1, mix(top, bottom,
                 (unsigned int)((y - y0) * 256 / span)));
}

/* Integer square root by bit shifts: no libgcc, no 64-bit division. */
static int scene_sqrt(int value)
{
    unsigned int n = (unsigned int)value, root = 0, bit = 1u << 30;
    if (value <= 0) return 0;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return (int)root;
}

/* Filled ellipse used by suns, planets and harbour water. */
static void ellipse_fill(int cx, int cy, int rx, int ry, uint32_t color)
{
    if (rx <= 0 || ry <= 0) return;
    for (int dy = -ry; dy <= ry; ++dy) {
        int extent = rx * scene_sqrt(ry * ry - dy * dy) / ry;
        gfx_rect(cx - extent, cy + dy, extent * 2 + 1, 1, color);
    }
}

static void pine(int x, int base, int height, uint32_t needle, uint32_t trunk)
{
    if (height < 12) return;
    gfx_rect(x - height / 16, base - height / 5, height / 8, height / 5, trunk);
    for (int tier = 0; tier < 3; ++tier) {
        int top = base - height * (3 - tier) / 3;
        int bottom = top + height * 55 / 100;
        if (bottom > base) bottom = base;
        int half = height / 8 + tier * height / 8;
        gfx_triangle(x, top, x - half, bottom, x + half, bottom, needle);
    }
}

static void cloud(int x, int y, int span, uint32_t color)
{
    gfx_rect(x - span / 2, y, span, span / 7 + 1, color);
    gfx_circle(x, y - span / 12, span / 5, color);
    gfx_circle(x + span / 3, y - span / 16, span / 7, color);
    gfx_circle(x - span / 3, y - span / 18, span / 8, color);
}

static void wallpaper_aurora(void)
{
    uint32_t sky_top = theme.dark ? 0x191926 : 0xAECBE4;
    uint32_t sky_bottom = theme.dark ? 0x2F3555 : 0xDFEAF7;
    for (int y = 0; y < screen_height; ++y)
        gfx_rect(0, y, screen_width, 1, mix(sky_top, sky_bottom,
                 (unsigned int)(y * 256 / screen_height)));
    int w = screen_width, h = screen_height;
    if (theme.dark) {
        /* Soft aurora ribbons drift across the night sky. */
        gfx_triangle(w / 10, -h / 10, w * 3 / 4, h / 3, w / 5, h / 2,
                     mix(0x89B4FA, sky_top, 56));
        gfx_triangle(w / 2, -h / 12, w + 20, h / 4, w * 2 / 3, h * 5 / 12,
                     mix(0xCBA6F7, sky_top, 64));
        /* A scatter of stars, then the layered mountain silhouettes. */
        static const int16_t stars[14] = {
            70, 60, 160, 130, 260, 48, 330, 96, 420, 54, 540, 120, 640, 76
        };
        for (int i = 0; i < 14; i += 2)
            gfx_rect(stars[i] % (w - 40) + 20, stars[i + 1] % (h / 3) + 34, 2, 2,
                     mix(0xE5EDF8, sky_top, 40));
        star(w - 74, 58, 13, mix(theme.accent, 0xFFFFFF, 96));
    }
    gfx_triangle(w * 47 / 100, -h / 8, w + w / 8, h * 53 / 100,
                 w * 34 / 100, h + 40, theme.dark ? 0x3A4266 : 0x93AEC9);
    gfx_triangle(w * 76 / 100, -40, w + 80, h * 72 / 100,
                 w * 59 / 100, h + 80, theme.dark ? 0x2E3554 : 0x7E9CBF);
    gfx_triangle(w + 40, h / 4, w * 68 / 100, h + 50,
                 w * 43 / 100, h + 50, theme.dark ? 0x333A63 : 0x8AA6C6);
    gfx_triangle(w * 34 / 100, h + 40, w * 61 / 100, h * 62 / 100,
                 w * 66 / 100, h + 40, theme.dark ? 0x242A44 : 0x6A88AC);
    if (!theme.dark)
        star(w - 74, 58, 13, mix(0xFFFFFF, 0xF3C76D, 96));
}

/* Alpine: sunlit peaks, snow caps and a pine forest. */
static void wallpaper_alpine(void)
{
    int w = screen_width, h = screen_height;
    int horizon = h * 2 / 3;
    gradient_rows(0, horizon, 0x7FB2E5, 0xDCEEFB);
    ellipse_fill(w * 3 / 4, h / 5, 34, 34, 0xFFF4CF);
    /* Far and near ranges, then snow caps on the high peaks. */
    gfx_triangle(-20, horizon, w / 3, h / 7, w * 2 / 3, horizon, 0x93AECB);
    gfx_triangle(w / 4, horizon, w * 2 / 3, h / 4, w + 20, horizon, 0x7E9CBE);
    gfx_triangle(w * 5 / 9, h / 7 + 18, w / 3 - 34, h / 7 + 84, w / 3 + 34,
                 h / 7 + 84, 0xF4FAFF);
    gfx_triangle(w * 2 / 3, h / 4 + 16, w * 2 / 3 - 30, h / 4 + 74,
                 w * 2 / 3 + 30, h / 4 + 74, 0xF4FAFF);
    gfx_rect(0, horizon, w, h - horizon, 0xEFF6FD);
    gfx_rect(0, horizon, w, 3, 0xFFFFFF);
    scene_seed = 0xA11CEu;
    for (int i = 0; i < 14; ++i) {
        int x = scene_chance(w + 60) - 30;
        int size = 40 + scene_chance(60);
        if (x < w / 3 - 60 && x > w / 3 - 140 && size > 70) size = 60;
        pine(x, horizon + 14 + scene_chance(h - horizon - 40), size,
             scene_chance(2) ? 0x2E6B4F : 0x255840, 0x5A4632);
    }
    gfx_rect(0, h - 26, w, 26, 0xE4EEF8);
}

/* Skyline: a dusk city with lit windows over a night street. */
static void wallpaper_skyline(void)
{
    int w = screen_width, h = screen_height;
    int street = h * 4 / 5;
    gradient_rows(0, h / 2, 0x0D1226, 0x2C3766);
    gradient_rows(h / 2, street, 0x2C3766, 0xD97B4F);
    gfx_circle(w * 5 / 6, h / 6, 24, 0xF7EFD4);
    gfx_circle(w * 5 / 6 - 7, h / 16, 5, mix(0xF7EFD4, 0x0D1226, 60));
    scene_seed = 0xC171E5u;
    int x = -10;
    while (x < w) {
        int width = 44 + scene_chance(46);
        int height = h / 6 + scene_chance(h / 3);
        int top = street - height;
        gfx_rect(x, top, width, height, 0x0A0E1A);
        for (int wy = top + 10; wy < street - 12; wy += 16)
            for (int wx = x + 7; wx < x + width - 9; wx += 14)
                if (scene_chance(10) < 4)
                    gfx_rect(wx, wy, 6, 8,
                             scene_chance(3) ? 0xF6D779 : 0x9FD7F5);
        x += width + 8;
    }
    gfx_rect(0, street, w, h - street, 0x151824);
    gfx_rect(0, street, w, 3, 0x2A2F42);
    for (int lane = 16; lane < w; lane += 90)
        gfx_rect(lane, h - h / 9, 44, 4, 0xE8C765);
}

/* Sunset: warm banded sky, a low sun and layered hill silhouettes. */
static void wallpaper_sunset(void)
{
    int w = screen_width, h = screen_height;
    gradient_rows(0, h / 3, 0x35275E, 0x9C4B77);
    gradient_rows(h / 3, h * 2 / 3, 0x9C4B77, 0xE57F52);
    gradient_rows(h * 2 / 3, h, 0xE57F52, 0xF6C97A);
    ellipse_fill(w / 2, h * 2 / 3 + 6, 58, 58, 0xFFE9A8);
    ellipse_fill(w / 2, h * 2 / 3 + 6, 44, 44, 0xFFF6CE);
    scene_seed = 0x5E77A1u;
    for (int i = 0; i < 7; ++i) {
        int cy = h / 6 + scene_chance(h / 2);
        int cx = scene_chance(w);
        int span = 120 + scene_chance(200);
        cloud(cx, cy, span, mix(0xF6C97A, 0xFFFFFF, (unsigned int)(60 + scene_chance(120))));
    }
    gfx_triangle(-40, h, w / 4, h * 2 / 3, w * 3 / 5, h, 0x4B2A55);
    gfx_triangle(w / 3, h, w * 3 / 4, h * 7 / 10, w + 40, h, 0x3C2347);
    gfx_rect(0, h - 10, w, 10, 0x2C1A38);
}

/* Ocean: a bright bay with a sailboat and rolling wave lines. */
static void wallpaper_ocean(void)
{
    int w = screen_width, h = screen_height;
    int sea = h / 2 + h / 8;
    gradient_rows(0, sea, 0x8FC7EE, 0xE3F3FC);
    ellipse_fill(w * 5 / 6, h / 6, 30, 30, 0xFFF8E0);
    cloud(w / 4, h / 8, 180, 0xFFFFFF);
    cloud(w * 2 / 3, h / 5, 140, 0xF3FAFF);
    /* Distant headland on the left. */
    gfx_triangle(-30, sea, w / 6, h / 3, w / 3, sea, 0x6E9E7E);
    gfx_triangle(w / 8, sea, w / 4, h / 2 - 8, w * 5 / 12, sea, 0x5C8C6E);
    gfx_rect(0, sea, w, h - sea, 0x2E7BB5);
    gfx_rect(0, sea, w, 3, 0x8FD6F2);
    /* Wave crests get sparser and darker towards the viewer. */
    for (int row = 0; row < 9; ++row) {
        int y = sea + 10 + row * (h - sea - 16) / 9;
        uint32_t crest = mix(0x8FD6F2, 0x1F5F94, (unsigned int)(row * 28));
        scene_seed = 0x0CEA11u + (unsigned int)row;
        for (int i = 0; i < 7; ++i) {
            int x = scene_chance(w);
            int span = 40 + scene_chance(90);
            gfx_rect(x, y, span, 3, crest);
            gfx_rect(x + 12, y - 3, span / 2, 3, crest);
        }
    }
    /* A small sloop sits mid-bay. */
    int bx = w * 2 / 5, by = sea + h / 14;
    gfx_triangle(bx, by - 78, bx, by - 6, bx - 40, by - 6, 0xF8FBFF);
    gfx_triangle(bx + 6, by - 62, bx + 6, by - 6, bx + 34, by - 6, 0xE4EEF8);
    gfx_triangle(bx - 46, by - 4, bx + 40, by - 4, bx + 26, by + 12, 0x7A4B2A);
    gfx_rect(bx - 44, by - 6, 84, 4, 0x8F5A33);
}

/* Galaxy: a spiral star field with a ringed planet in the corner. */
static void wallpaper_galaxy(void)
{
    int w = screen_width, h = screen_height;
    gradient_rows(0, h, 0x06060F, 0x191533);
    int cx = w / 2, cy = h / 2;
    scene_seed = 0x6A1A9Eu;
    for (int arm = 0; arm < 2; ++arm) {
        /* Fixed-point unit vector rotated ~4 degrees per star. */
        int ux = arm ? -1000 : 1000, uy = 0, radius = 12;
        for (int i = 0; i < 240; ++i) {
            int px = cx + ux * radius / 1000;
            int py = cy + uy * radius / 600;
            px += scene_chance(28) - 14;
            py += scene_chance(24) - 12;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                uint32_t tone = scene_chance(4) == 0 ? 0xC9B8FF :
                                scene_chance(3) == 0 ? 0x89C4FF : 0xE8ECFF;
                int size = scene_chance(3) ? 1 : 2;
                gfx_rect(px, py, size, size, tone);
            }
            {
                int next_x = (ux * 997 - uy * 70) / 1000;
                int next_y = (ux * 70 + uy * 997) / 1000;
                ux = next_x;
                uy = next_y;
            }
            radius += 2;
        }
    }
    for (int i = 0; i < 70; ++i)
        gfx_rect(scene_chance(w), scene_chance(h), 1, 1,
                 scene_chance(5) ? 0x9AA6D8 : 0xFFFFFF);
    ellipse_fill(cx, cy, 60, 24, mix(0x2A2450, 0xC9B8FF, 96));
    ellipse_fill(cx, cy, 34, 14, mix(0x8E7BE8, 0xFFFFFF, 120));
    ellipse_fill(cx, cy, 14, 7, 0xFFFFFF);
    /* Ringed planet tucked into the lower-right corner. */
    int px = w - 150, py = h - 130;
    ellipse_fill(px, py, 66, 12, 0xC9A46A);
    ellipse_fill(px, py, 54, 54, 0xE0B978);
    gfx_rect(px - 54, py - 12, 108, 5, 0xB98F55);
    gfx_rect(px - 54, py + 8, 108, 4, 0xC9A46A);
    ellipse_fill(px + 18, py - 20, 22, 16, 0xF0D69C);
    ellipse_fill(px - 54, py + 12, 30, 10, 0x7A5B33);
    ellipse_fill(px + 54, py + 12, 30, 10, 0x7A5B33);
}

static void wallpaper_topo(void)
{
    gfx_mask_scaled(0, 0, screen_width, screen_height, nova_topo_aa, 1280, 800,
                    0xFFFFFF, 0x000000);
}

static void wallpaper(void)
{
    static uint32_t cached[GFX_MAX_WIDTH * GFX_MAX_HEIGHT];
    static int cached_id = -1, cached_width, cached_height;
    static uint32_t cached_accent;
    static bool cached_dark;
    if (cached_id == wallpaper_id && cached_width == screen_width &&
        cached_height == screen_height && cached_accent == theme.accent &&
        cached_dark == theme.dark) {
        gfx_blit(0, 0, screen_width, screen_height, cached);
        return;
    }
    switch (wallpaper_id) {
    case WALLPAPER_ALPINE:   wallpaper_alpine();   break;
    case WALLPAPER_SKYLINE:  wallpaper_skyline();  break;
    case WALLPAPER_SUNSET:   wallpaper_sunset();   break;
    case WALLPAPER_OCEAN:    wallpaper_ocean();    break;
    case WALLPAPER_GALAXY:   wallpaper_galaxy();   break;
    case WALLPAPER_TOPO:     wallpaper_topo();     break;
    default:                 wallpaper_aurora();   break;
    }
    gfx_capture(cached);
    cached_id = wallpaper_id; cached_width = screen_width; cached_height = screen_height;
    cached_accent = theme.accent; cached_dark = theme.dark;
}

/* Frosted panel shared by all desktop widgets. */
static void widget_panel(int x, int y, int width, int height)
{
    uint32_t sky_bottom = theme.dark ? 0x2F3555 : 0xDFEAF7;
    uint32_t card = mix(sky_bottom, theme.surface, 176);
    gfx_round_rect(x + 2, y + 5, width, height, 14,
                   theme.dark ? 0x10101C : 0x93A4BC);
    gfx_round_rect(x, y, width, height, 14, mix(sky_bottom, theme.border, 160));
    gfx_round_rect(x + 1, y + 1, width - 2, height - 2, 13, card);
}

static uint32_t widget_surface(void)
{
    uint32_t sky_bottom = theme.dark ? 0x2F3555 : 0xDFEAF7;
    return mix(sky_bottom, theme.surface, 176);
}

/* macOS-style clock widget: big time plus the date. */
static void widget_clock(int x, int y)
{
    int width = 216, height = 100;
    widget_panel(x, y, width, height);
    uint32_t card = widget_surface();
    char time_value[16], date_value[24];
    clock_text(time_value);
    system_date(date_value);
    gfx_text_scaled(x + 14, y + 14, time_value, theme.text, card, 2);
    gfx_text(x + 14, y + 56, date_value, theme.muted, card);
    gfx_text(x + 14, y + 76, "Nova OS", theme.muted, card);
}

/* macOS-style battery widget: cell outline, fill bar and percentage. */
static void widget_battery(int x, int y)
{
    int width = 216, height = 64;
    widget_panel(x, y, width, height);
    uint32_t card = widget_surface();
    uint32_t percent_value = system_battery_percent();
    uint32_t fill = percent_value > 60 ? 0x7BD88F :
                    percent_value > 30 ? 0xF3C76D : 0xE06C75;
    /* Cell body with a nub, like the macOS menu-bar battery. */
    gfx_round_rect(x + 14, y + 20, 44, 24, 5, mix(card, theme.text, 48));
    gfx_round_rect(x + 16, y + 22, 40, 20, 4, card);
    gfx_rect(x + 58, y + 27, 4, 10, mix(card, theme.text, 48));
    int inner = (int)(percent_value * 36 / 100);
    if (inner > 0) gfx_round_rect(x + 18, y + 24, inner, 16, 3, fill);
    char digits[12];
    decimal(digits, (int)percent_value);
    int characters = 0;
    while (digits[characters]) ++characters;
    digits[characters++] = '%';
    digits[characters] = 0;
    gfx_text(x + 70, y + 17, digits, theme.text, card);
    gfx_text(x + 70, y + 35, system_battery_state(), theme.muted, card);
}

/* A frosted welcome card floats on the wallpaper like a GNOME widget. */
static void widget_welcome(int x, int y)
{
    int width = 216, height = 76;
    widget_panel(x, y, width, height);
    uint32_t card = widget_surface();
    star(x + 24, y + 24, 11, theme.accent);
    gfx_text(x + 44, y + 13, "NOVA OS 0.4", theme.text, card);
    gfx_text(x + 16, y + 34, "Make it your own.", theme.muted, card);
    gfx_text(x + 16, y + 52, "F1 opens your launcher", theme.muted, card);
}

/* The right-hand widget column, shown per the Settings toggles. */
static void widget_cards(void)
{
    int x = screen_width - 232, y = 40;
    if (widgets_clock) { widget_clock(x, y); y += 108; }
    if (widgets_battery) { widget_battery(x, y); y += 72; }
    widget_welcome(x, y);
}

static void desktop_icons(void)
{
    if (!icons_visible) return;
    static const AppId ids[] = { APP_FILES, APP_TERMINAL, APP_NOTES, APP_BROWSER };
    static const char *labels[] = { "Home", "Terminal", "Notes", "Browser" };
    static const int lengths[] = { 4, 8, 5, 7 };
    for (int i = 0; i < 4; ++i) {
        int y = 65 + i * 92;
        uint32_t sky_top = theme.dark ? 0x191926 : 0xAECBE4;
        uint32_t sky_bottom = theme.dark ? 0x2F3555 : 0xDFEAF7;
        uint32_t tile = mix(mix(sky_top, sky_bottom,
                            (unsigned int)(y * 256 / screen_height)),
                        theme.dark ? 0x27273A : 0xFFFFFF, 96);
        gfx_round_rect(46 - 25, y - 25, 50, 50, 12, tile);
        gfx_round_rect(46 - 25, y - 25, 50, 50, 12, mix(tile, theme.border, 96));
        gfx_round_rect(46 - 24, y - 24, 48, 48, 11, tile);
        icon(ids[i], 46, y, 36, 0xC4E5FF, tile);
        int characters = lengths[i];
        gfx_round_rect(40 - characters * 4, y + 28, characters * 8 + 12, 20, 6,
                       theme.surface);
        gfx_text(46 - characters * 4, y + 30, labels[i], theme.text, GFX_TRANSPARENT);
    }
}

/* GNOME-style circular title-bar control. Hit rectangles stay unchanged. */
static void window_control(UiRect rectangle, int kind, bool active)
{
    int center_x = rectangle.x + 12, center_y = rectangle.y + 11;
    bool hover = contains(rectangle, mouse_x, mouse_y);
    uint32_t glyph = active ? theme.text : theme.muted;
    if (hover) {
        if (kind == 0) {
            gfx_circle(center_x, center_y, 11, 0xE06C75);
            glyph = 0xFFFFFF;
        } else {
            gfx_circle(center_x, center_y, 11, theme.dark ? 0x45475A : 0xC7CEDC);
        }
    }
    if (kind == 0) {
        gfx_line(center_x - 4, center_y - 4, center_x + 4, center_y + 4, glyph);
        gfx_line(center_x - 4, center_y + 4, center_x + 4, center_y - 4, glyph);
    } else if (kind == 1) {
        gfx_rect(center_x - 4, center_y - 4, 9, 1, glyph);
        gfx_rect(center_x - 4, center_y + 4, 9, 1, glyph);
        gfx_rect(center_x - 4, center_y - 4, 1, 9, glyph);
        gfx_rect(center_x + 4, center_y - 4, 1, 9, glyph);
    } else {
        gfx_rect(center_x - 4, center_y, 9, 1, glyph);
    }
}

static void draw_window(AppId app)
{
    NovaWindow *window = &windows[app];
    UiRect r = window->rectangle;
    bool active = focused == app;
    uint32_t header = active ? theme.surface_alt : theme.surface;
    /* Two shadow layers give each floating window a soft GNOME edge. */
    gfx_round_rect(r.x + 3, r.y + 9, r.width, r.height, 12,
                   theme.dark ? 0x101019 : 0x99A6BB);
    gfx_round_rect(r.x + 1, r.y + 4, r.width + 2, r.height + 2, 11,
                   theme.dark ? 0x171726 : 0xB0BCCC);
    gfx_round_rect(r.x, r.y, r.width, r.height, 10, theme.border);
    gfx_round_rect(r.x + 1, r.y + 1, r.width - 2, r.height - 2, 9, theme.surface);
    gfx_round_rect(r.x + 1, r.y + 1, r.width - 2, TITLE_HEIGHT + 8, 9, header);
    gfx_rect(r.x + 1, r.y + TITLE_HEIGHT - 8, r.width - 2, 8, header);
    gfx_rect(r.x + 1, r.y + TITLE_HEIGHT, r.width - 2, 1, theme.border);
    if (active)
        gfx_rect(r.x + 48, r.y + TITLE_HEIGHT - 1, r.width - 96, 1,
                 mix(header, theme.accent, 176));
    icon(app, r.x + 21, r.y + 19, 22, theme.accent, header);
    const char *title = apps_title(app);
    unsigned int characters = 0;
    while (title[characters]) ++characters;
    gfx_text(r.x + (int)(r.width - characters * GFX_CHAR_WIDTH) / 2, r.y + 12, title,
             active ? theme.text : theme.muted, header);
    /* These hit rectangles stay exactly aligned with scripts/check_boot.py. */
    UiRect close = { r.x + r.width - 34, r.y + 8, 24, 22 };
    UiRect maximize = { r.x + r.width - 64, r.y + 8, 24, 22 };
    UiRect minimize = { r.x + r.width - 94, r.y + 8, 24, 22 };
    window_control(minimize, 2, active);
    window_control(maximize, 1, active);
    window_control(close, 0, active);
    UiRect content = { r.x + 1, r.y + TITLE_HEIGHT + 1,
                       r.width - 2, r.height - TITLE_HEIGHT - 2 };
    apps_draw(app, content, active, &theme);
}

static int panel_slot_width(void)
{
    return smaller(56, (screen_width - 228) / process_catalog_count());
}

/* The floating GNOME-style dock. Its hit rectangles stay exactly where the
 * integration test expects them: the launcher orb sits at (38, height - 36). */
static void panel(void)
{
    int y = screen_height - PANEL_HEIGHT - PANEL_MARGIN;
    uint32_t color = theme.dark ? 0x16162A : 0xFFFFFF;
    gfx_round_rect(PANEL_MARGIN, y + 4, screen_width - 2 * PANEL_MARGIN,
                   PANEL_HEIGHT, 14, theme.dark ? 0x0B0B14 : 0x93A2B8);
    gfx_round_rect(PANEL_MARGIN, y, screen_width - 2 * PANEL_MARGIN,
                   PANEL_HEIGHT, 13, mix(color, theme.border, 64));
    gfx_round_rect(PANEL_MARGIN + 1, y + 1, screen_width - 2 * PANEL_MARGIN - 2,
                   PANEL_HEIGHT - 2, 12, color);
    if (launcher)
        gfx_round_rect(20, y + 6, 48, 44, 10, theme.dark ? 0x27273A : 0xE7EBF3);
    star(44, y + 28, 15, theme.accent);
    gfx_rect(76, y + 13, 1, 30, theme.border);
    int slot = panel_slot_width(), button_width = slot - 8;
    for (int i = 0; i < process_catalog_count(); ++i) {
        int x = 88 + i * slot;
        if (windows[i].open && !windows[i].minimized) {
            gfx_round_rect(x, y + 5, button_width, 44, 9,
                           focused == (AppId)i ?
                           (theme.dark ? 0x27273A : 0xE7EBF3) : color);
            gfx_rect(x + button_width / 2 - 9, y + 48, 18, 2,
                     focused == (AppId)i ? theme.accent : theme.muted);
        } else if (windows[i].open) {
            gfx_rect(x + button_width / 2 - 4, y + 48, 8, 2, theme.muted);
        }
        icon((AppId)i, x + button_width / 2, y + 26, smaller(30, button_width),
             theme.accent, color);
    }
    gfx_round_rect(screen_width - 124, y + 8, 112, 40, 10,
                   theme.dark ? 0x27273A : 0xE7EBF3);
    char clock[16];
    clock_text(clock);
    gfx_text(screen_width - 101, y + 11, clock, theme.text,
             theme.dark ? 0x27273A : 0xE7EBF3);
    gfx_text(screen_width - 117, y + 29, "NOVA OS", theme.muted,
             theme.dark ? 0x27273A : 0xE7EBF3);
}

/* The GNOME-style top bar: Activities star, centred clock, live network. */
static void topbar(void)
{
    uint32_t bar = theme.dark ? 0x12121E : 0xF2F4F9;
    uint32_t hover = theme.dark ? 0x27273A : 0xE2E7F0;
    gfx_rect(0, 0, screen_width, TOP_BAR, bar);
    gfx_rect(0, TOP_BAR - 1, screen_width, 1, theme.border);
    bool star_hover = contains((UiRect){ 10, 4, 40, 22 }, mouse_x, mouse_y);
    if (star_hover) gfx_round_rect(10, 4, 40, 22, 11, hover);
    star(26, 15, 8, theme.accent);
    char clock[16];
    clock_text(clock);
    int characters = 0;
    while (clock[characters]) ++characters;
    int start = screen_width / 2 - (characters + 12) * GFX_CHAR_WIDTH / 2;
    gfx_text(start, 7, clock, theme.text, bar);
    gfx_text(start + (characters + 1) * GFX_CHAR_WIDTH, 7, "Nova OS 0.4",
             theme.muted, bar);
    UiRect indicator = { screen_width - 116, 4, 112, 22 };
    bool indicator_hover = contains(indicator, mouse_x, mouse_y);
    if (indicator_hover) gfx_round_rect(indicator.x, indicator.y,
                                        indicator.width, indicator.height, 11, hover);
    uint32_t state = network_present() && network_link_up() ? 0x74C8A5 : theme.muted;
    gfx_circle(screen_width - 104, 15, 4, state);
    gfx_text(screen_width - 92, 7, network_ip(), theme.muted,
             indicator_hover ? hover : bar);
}

static UiRect launcher_rectangle(void)
{
    return (UiRect){ 20, screen_height - 68 - 348, smaller(600, screen_width - 40), 338 };
}

static int launcher_choice(int x, int y)
{
    UiRect r = launcher_rectangle();
    /* 16 apps in a 4x4 grid, column-major so F2..F12 labels stay in order. */
    int cell_width = (r.width - 16) / 4;
    if (x < r.x + 8 || x >= r.x + 8 + cell_width * 4 ||
        y < r.y + 65 || y >= r.y + 65 + 4 * 44) return -1;
    int column = (x - r.x - 8) / cell_width;
    int choice = column * 4 + (y - r.y - 65) / 44;
    choice += launcher_page * 16;
    return choice < process_catalog_count() ? choice : -1;
}

/* A GNOME app grid: icon tiles in three columns above the dock. */
static void draw_launcher(void)
{
    UiRect r = launcher_rectangle();
    gfx_round_rect(r.x + 4, r.y + 6, r.width, r.height, 14,
                   theme.dark ? 0x0B0B14 : 0x93A2B8);
    gfx_round_rect(r.x, r.y, r.width, r.height, 14,
                   mix(theme.surface, theme.border, 64));
    gfx_round_rect(r.x + 1, r.y + 1, r.width - 2, r.height - 2, 13, theme.surface);
    star(r.x + 28, r.y + 29, 13, theme.accent);
    gfx_text(r.x + 56, r.y + 13, "Applications", theme.text, theme.surface);
    gfx_text(r.x + 56, r.y + 31, "Nova OS 0.4 app grid", theme.muted, theme.surface);
    gfx_rect(r.x + 16, r.y + 57, r.width - 32, 1, theme.border);
    int cell_width = (r.width - 16) / 4;
    for (int i = launcher_page * 16; i < process_catalog_count() && i < (launcher_page+1)*16; ++i) {
        int x = r.x + 8 + ((i % 16) / 4) * cell_width;
        int y = r.y + 65 + (i % 4) * 44;
        bool selected = launcher_selection == i;
        uint32_t tile = selected ? theme.surface_alt :
                        mix(theme.surface, theme.surface_alt, 72);
        gfx_round_rect(x, y, cell_width - 8, 40, 8, tile);
        if (selected)
            gfx_round_rect(x, y, cell_width - 8, 40, 8, theme.accent);
        gfx_round_rect(x + 1, y + 1, cell_width - 10, 38, 7, tile);
        icon((AppId)i, x + (cell_width - 8) / 2, y + 13, 21, theme.accent, tile);
        const char *name = apps_title((AppId)i);
        unsigned int characters = 0;
        while (name[characters]) ++characters;
        gfx_text(x + (cell_width - 8 - (int)characters * GFX_CHAR_WIDTH) / 2, y + 24,
                 name, theme.text, tile);
        if (i <= 10) {
            char key[5] = { 'F', 0, 0, 0, 0 };
            decimal(key + 1, i + 2);
            gfx_text(x + cell_width - 30, y + 3, key, theme.muted, tile);
        }
    }
    if(process_catalog_count()>16){
        gfx_round_rect(r.x+18,r.y+260,120,30,8,theme.surface_alt);
        gfx_text(r.x+26,r.y+267,launcher_page?"Previous apps":"More apps",theme.text,theme.surface_alt);
    }
    gfx_text(r.x + 18, r.y + r.height - 23, "Your desktop, your universe.",
             theme.muted, theme.surface);
}

static void pointer(void)
{
    /* A compact outlined cursor remains visible over both desktop themes. */
    gfx_triangle(mouse_x, mouse_y, mouse_x + 2, mouse_y + 20,
                 mouse_x + 15, mouse_y + 14, 0x07101C);
    gfx_triangle(mouse_x + 2, mouse_y + 3, mouse_x + 3, mouse_y + 17,
                 mouse_x + 12, mouse_y + 13, 0xF6FAFF);
    gfx_line(mouse_x + 8, mouse_y + 14, mouse_x + 12, mouse_y + 22, 0x07101C);
    gfx_line(mouse_x + 8, mouse_y + 14, mouse_x + 11, mouse_y + 21, 0xF6FAFF);
}

static int snow_random(int bound)
{
    snow_seed = snow_seed * 1103515245U + 12345U;
    return (int)((snow_seed >> 16) & 0x7FFF) % bound;
}

static void snow_reset(void)
{
    snow_seed = 0x5EED07A5;
    for (int i = 0; i < SNOW_FLAKES; ++i) {
        flakes[i].x = snow_random(screen_width + 8) - 4;
        flakes[i].y = snow_random(screen_height + 8) - 4;
        flakes[i].speed = 2 + snow_random(4);
        flakes[i].size = 1 + snow_random(3);
        flakes[i].sway = snow_random(2) ? 1 : -1;
        flakes[i].phase = (unsigned int)snow_random(64);
    }
}

static void snow_advance(void)
{
    for (int i = 0; i < SNOW_FLAKES; ++i) {
        struct snow_flake *flake = &flakes[i];
        flake->y += flake->speed;
        if ((flake->phase++ & 31) == 0) flake->sway = -flake->sway;
        flake->x += flake->sway;
        if (flake->y > screen_height + 4) {
            flake->y = -4;
            flake->x = snow_random(screen_width + 8) - 4;
            flake->speed = 2 + snow_random(4);
            flake->size = 1 + snow_random(3);
        }
        if (flake->x < -6) flake->x += screen_width + 12;
        if (flake->x > screen_width + 6) flake->x -= screen_width + 12;
    }
}

/* Flakes fall in front of the wallpaper and windows, behind the shell chrome. */
static void snow_draw(void)
{
    for (int i = 0; i < SNOW_FLAKES; ++i) {
        struct snow_flake *flake = &flakes[i];
        if (flake->size <= 2) gfx_rect(flake->x, flake->y, flake->size, flake->size, 0xFFFFFF);
        else gfx_circle(flake->x, flake->y, 1, 0xF4F9FF);
    }
}

void desktop_render(void)
{
    static uint32_t next_frame;
    uint32_t now_ms=system_ms();
    if((int32_t)(now_ms-next_frame)<0)return;
    next_frame=now_ms+20;
    /* The clock and battery widgets only need a redraw when their text moves. */
    char now[16];
    clock_text(now);
    if (!text_equal(now, shown_clock)) {
        text_copy(shown_clock, now);
        dirty = true;
    }
    int battery_now = (int)system_battery_percent();
    if (battery_now != shown_battery) {
        shown_battery = battery_now;
        dirty = true;
    }
    bool focused_animates = focused != APP_COUNT && windows[focused].open &&
                            !windows[focused].minimized && apps_animate(focused);
    bool animate = snow || focused_animates;
    if (animate) {
        if (snow) snow_advance();
        if (focused_animates) apps_tick(focused);
        dirty = true;
    }
    if (!dirty) return;
    wallpaper();
    widget_cards();
    desktop_icons();
    for (int i = 0; i < APP_COUNT; ++i) {
        AppId app = stacking[i];
        if (windows[app].open && !windows[app].minimized) draw_window(app);
    }
    if (snow) snow_draw();
    topbar();
    panel();
    if (launcher) draw_launcher();
    pointer();
    gfx_present();
    next_frame=system_ms()+20;
    dirty = false;
    /* Animated frames are paced so snow and games run smoothly, not flat out. */
    /* Never sleep here: user syscalls and input must continue between frames. */
}

static void cycle_window(void)
{
    for (int i = APP_COUNT - 2; i >= 0; --i) {
        AppId app = stacking[i];
        if (windows[app].open && !windows[app].minimized) {
            raise_window(app);
            return;
        }
    }
}

static void keyboard_event(int key)
{
    if (key == KEY_F1) {
        launcher = !launcher;
        terminal_log(launcher ? "MENU_OPEN\n" : "MENU_CLOSE\n");
        dirty = true;
        return;
    }
    if (key >= KEY_F2 && key <= KEY_F12) { desktop_open((AppId)(key - KEY_F2)); return; }
    if (key == KEY_CTRL_ALT_T) { desktop_open(APP_TERMINAL); return; }
    if (key == KEY_ALT_TAB) { launcher = false; cycle_window(); return; }
    if (launcher) {
        if (key == 27) launcher = false;
        else if (key == KEY_UP) launcher_selection = (launcher_selection + process_catalog_count() - 1) % process_catalog_count();
        else if (key == KEY_DOWN) launcher_selection = (launcher_selection + 1) % process_catalog_count();
        else if (key == KEY_LEFT && launcher_selection >= 4) launcher_selection -= 4;
        else if (key == KEY_RIGHT && launcher_selection + 4 < process_catalog_count()) launcher_selection += 4;
        else if (key == '\n') desktop_open((AppId)launcher_selection);
        launcher_page=launcher_selection/16;
        dirty = true;
        return;
    }
    if (focused != APP_COUNT && windows[focused].open && !windows[focused].minimized) {
        if(key>=0 && key<512)key_owner[key]=focused;
        apps_key(focused, key);
        dirty = true;
    }
}

/* Clicks on the top bar: the star toggles the app grid, the network
 * indicator opens Network. Everything else on the bar is absorbed. */
static bool topbar_click(int x, int y)
{
    if (y >= TOP_BAR) return false;
    if (contains((UiRect){ 10, 4, 40, 22 }, x, y)) {
        launcher = !launcher;
        terminal_log(launcher ? "MENU_OPEN\n" : "MENU_CLOSE\n");
        dirty = true;
    } else if (contains((UiRect){ screen_width - 116, 4, 112, 22 }, x, y)) {
        launcher = false;
        desktop_open(APP_NETWORK);
    }
    return true;
}

static bool panel_click(int x, int y)
{
    int panel_y = screen_height - PANEL_HEIGHT - PANEL_MARGIN;
    if (!contains((UiRect){ 12, panel_y, screen_width - 24, PANEL_HEIGHT }, x, y)) return false;
    if (contains((UiRect){ 20, panel_y + 6, 48, 44 }, x, y)) {
        launcher = !launcher;
        terminal_log(launcher ? "MENU_OPEN\n" : "MENU_CLOSE\n");
        dirty = true;
        return true;
    }
    for (int i = 0; i < process_catalog_count(); ++i) {
        int slot = panel_slot_width();
        if (contains((UiRect){ 88 + i * slot, panel_y + 5, slot - 8, 46 }, x, y)) {
            launcher = false;
            if (windows[i].open && !windows[i].minimized && focused == (AppId)i)
                minimize_window((AppId)i);
            else desktop_open((AppId)i);
            return true;
        }
    }
    if (launcher) { launcher = false; dirty = true; }
    return true;
}

static void mouse_press(int x, int y)
{
    {
        char digits[6];
        terminal_log("CLICK: x=");
        decimal(digits, x);
        terminal_log(digits);
        terminal_log(" y=");
        decimal(digits, y);
        terminal_log(digits);
        terminal_log("\n");
    }
    if (topbar_click(x, y)) return;
    if (panel_click(x, y)) return;
    if (launcher) {
        UiRect menu = launcher_rectangle();
        if (contains(menu, x, y)) {
            if(process_catalog_count()>16 && contains((UiRect){menu.x+18,menu.y+260,120,30},x,y)){
                launcher_page=1-launcher_page;launcher_selection=launcher_page*16;dirty=true;return;
            }
            int choice = launcher_choice(x, y);
            if (choice >= 0) desktop_open((AppId)choice);
        } else launcher = false;
        dirty = true;
        return;
    }
    for (int i = APP_COUNT - 1; i >= 0; --i) {
        AppId app = stacking[i];
        NovaWindow *window = &windows[app];
        UiRect r = window->rectangle;
        if (!window->open || window->minimized || !contains(r, x, y)) continue;
        raise_window(app);
        if (y < r.y + TITLE_HEIGHT) {
            if (contains((UiRect){ r.x + r.width - 34, r.y + 8, 24, 22 }, x, y))
                close_window(app);
            else if (contains((UiRect){ r.x + r.width - 64, r.y + 8, 24, 22 }, x, y))
                maximize_window(app);
            else if (contains((UiRect){ r.x + r.width - 94, r.y + 8, 24, 22 }, x, y))
                minimize_window(app);
            else if (!window->maximized) {
                dragging = (int)app;
                drag_x = x - r.x;
                drag_y = y - r.y;
            }
        } else {
            pointer_capture=app;
            if (app == APP_PAINT) painting = true;
            if (app == APP_DOOM) doom_firing = true;
            apps_click(app, x - r.x - 1, y - r.y - TITLE_HEIGHT - 1,
                       r.width - 2, r.height - TITLE_HEIGHT - 2);
        }
        dirty = true;
        return;
    }
    if (icons_visible) {
        static const AppId ids[] = { APP_FILES, APP_TERMINAL, APP_NOTES, APP_BROWSER };
        for (int i = 0; i < 4; ++i)
            if (contains((UiRect){ 12, 42 + i * 92, 78, 77 }, x, y)) {
                desktop_open(ids[i]);
                return;
            }
    }
}

/* Key releases fan out to open windows: DOOM needs Break codes so a held
 * key stops the moment it is let go. Desktop shortcuts (F-keys, combos) are
 * press-only, so their releases are dropped here. */
static void key_release(int key)
{
    if (key >= KEY_F1 && key <= KEY_F12) return;
    if (key == KEY_ALT_TAB || key == KEY_CTRL_S || key == KEY_CTRL_L ||
        key == KEY_CTRL_ALT_T)
        return;
    if(key>=0 && key<512 && key_owner[key]>=0){apps_key_up((AppId)key_owner[key],key);key_owner[key]=-1;}
}

void desktop_event(const InputEvent *event)
{
    if (event->type == INPUT_KEY) {
        if (event->pressed) keyboard_event(event->key);
        else key_release(event->key);
        return;
    }
    int old_x = mouse_x, old_y = mouse_y;
    uint8_t previous = mouse_buttons;
    mouse_x = clamp(event->x, 0, screen_width - 1);
    mouse_y = clamp(event->y, 0, screen_height - 1);
    mouse_buttons = event->buttons;
    if (old_x != mouse_x || old_y != mouse_y || previous != mouse_buttons) dirty = true;
    if (dragging >= 0 && (mouse_buttons & 1)) {
        NovaWindow *window = &windows[dragging];
        window->rectangle.x = clamp(mouse_x - drag_x, 0, screen_width - window->rectangle.width);
        window->rectangle.y = clamp(mouse_y - drag_y, TOP_BAR + 6,
                                    screen_height - window->rectangle.height - 80);
        dirty = true;
    }
    if (!(mouse_buttons & 1) && (previous & 1) && dragging >= 0) {
        log_window("WINDOW_MOVE: ", (AppId)dragging);
        log_geometry((AppId)dragging);
        dragging = -1;
    }
    /* Releasing the left button anywhere ends DOOM's mouse fire. */
    if (!(mouse_buttons & 1) && (previous & 1) && doom_firing) {
        doom_firing = false;
        apps_key_up(APP_DOOM, DOOM_NOVA_FIRE);
    }
    if (pointer_capture>=0) {
        UiRect r=windows[pointer_capture].rectangle;
        apps_pointer((AppId)pointer_capture,mouse_x-r.x-1,mouse_y-r.y-TITLE_HEIGHT-1,
                     r.width-2,r.height-TITLE_HEIGHT-2,(mouse_buttons&1)!=0);
        if(!(mouse_buttons&1)){pointer_capture=-1;painting=false;}
    }
    if ((mouse_buttons & 1) && !(previous & 1)) mouse_press(mouse_x, mouse_y);
    if (launcher && !(mouse_buttons & 1)) {
        UiRect menu = launcher_rectangle();
        int choice = launcher_choice(mouse_x, mouse_y);
        if (contains(menu, mouse_x, mouse_y) && choice >= 0) {
            if (launcher_selection != choice) dirty = true;
            launcher_selection = choice;
        }
    }
}
