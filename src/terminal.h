#ifndef NOVA_TERMINAL_H
#define NOVA_TERMINAL_H
#include "ui.h"
#include <stdint.h>
#define TERM_FG 0xD5DFEF
#define TERM_BG 0x111A29
#define TERM_ACCENT 0x66E3C4
#define TERM_MUTED 0x8190AA
#define TERM_RED 0xF38BA8
void terminal_serial_init(void);
void terminal_log(const char *text);
void terminal_init(void);
void terminal_clear(void);
void terminal_putc(char character);
void terminal_write(const char *text);
void terminal_color(uint32_t color);
void terminal_background(uint32_t color);
void terminal_render(UiRect area, bool focused, const UiTheme *theme);
#endif
