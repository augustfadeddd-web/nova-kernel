#ifndef NOVA_UI_H
#define NOVA_UI_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { APP_TERMINAL, APP_NOTES, APP_CALCULATOR, APP_FILES, APP_SETTINGS, APP_ABOUT,
               APP_BROWSER, APP_NETWORK, APP_CLOCK, APP_PAINT, APP_SNAKE, APP_TICTAC,
               APP_MINES, APP_SYSMON, APP_CAR, APP_DOOM, APP_BUILTIN_COUNT, APP_COUNT = 24 } AppId;
typedef struct { int x, y, width, height; } UiRect;
typedef struct {
    uint32_t desktop, surface, surface_alt, text, muted, border, accent;
    bool dark;
} UiTheme;
#endif
