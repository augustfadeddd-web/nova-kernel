#include "terminal.h"
#include "process.h"
#include "graphics.h"
#include <stddef.h>

/* Keeping output in cells makes windows movable without losing their text. */
enum { COLUMNS = 80, ROWS = 64, SERIAL = 0x3F8, PADDING = 12 };
struct cell { char character; uint32_t foreground, background; };
static struct cell cells[ROWS][COLUMNS];
static int column, row;
static uint32_t foreground = TERM_FG, background = TERM_BG;

static void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t inb(uint16_t port)
{
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

void terminal_serial_init(void)
{
    outb(SERIAL + 1, 0);
    outb(SERIAL + 3, 0x80);
    outb(SERIAL, 1);
    outb(SERIAL + 1, 0);
    outb(SERIAL + 3, 3);
    outb(SERIAL + 2, 0xC7);
    outb(SERIAL + 4, 0x0B);
}

static void serial_char(char character)
{
    for (unsigned int i = 0; i < 100000; ++i) {
        if (inb(SERIAL + 5) & 0x20) {
            outb(SERIAL, (uint8_t)character);
            return;
        }
    }
}

void terminal_log(const char *text)
{
    while (*text) serial_char(*text++);
}

static void blank_row(int y)
{
    for (int x = 0; x < COLUMNS; ++x)
        cells[y][x] = (struct cell){ ' ', TERM_FG, TERM_BG };
}

static void scroll(void)
{
    for (int y = 0; y < ROWS - 1; ++y)
        for (int x = 0; x < COLUMNS; ++x)
            cells[y][x] = cells[y + 1][x];
    blank_row(ROWS - 1);
    row = ROWS - 1;
}

void terminal_clear(void)
{
    for (int y = 0; y < ROWS; ++y) blank_row(y);
    column = row = 0;
    foreground = TERM_FG;
    background = TERM_BG;
}

void terminal_init(void)
{
    terminal_clear();
}

void terminal_putc(char character)
{
    if (character == '\n') {
        column = 0;
        ++row;
        serial_char('\n');
    } else if (character == '\b') {
        if (column > 0) --column;
        else if (row > 0) { --row; column = COLUMNS - 1; }
        cells[row][column] = (struct cell){ ' ', foreground, background };
        terminal_log("\b \b");
    } else if ((unsigned char)character >= 32 && (unsigned char)character <= 126) {
        cells[row][column] = (struct cell){ character, foreground, background };
        serial_char(character);
        if (++column == COLUMNS) { column = 0; ++row; }
    }
    if (row >= ROWS) scroll();
    struct nova_event update={.type=NOVA_EV_REDRAW};
    process_app_event(APP_TERMINAL,&update);
}

void terminal_write(const char *text)
{
    while (*text) terminal_putc(*text++);
}

void terminal_color(uint32_t color) { foreground = color; }
void terminal_background(uint32_t color) { background = color; }

void terminal_render(UiRect area, bool focused, const UiTheme *theme)
{
    if (area.width <= 0 || area.height <= 0) return;
    gfx_rect(area.x, area.y, area.width, area.height, TERM_BG);
    int visible_columns = (area.width - PADDING * 2) / GFX_CHAR_WIDTH;
    int visible_rows = (area.height - PADDING * 2) / GFX_CHAR_HEIGHT;
    if (visible_columns < 1 || visible_rows < 1) return;
    if (visible_columns > COLUMNS) visible_columns = COLUMNS;
    if (visible_rows > ROWS) visible_rows = ROWS;
    int first_row = row >= visible_rows ? row - visible_rows + 1 : 0;
    int first_column = column >= visible_columns ? column - visible_columns + 1 : 0;
    for (int y = 0; y < visible_rows; ++y) {
        int source_row = first_row + y;
        if (source_row >= ROWS) break;
        for (int x = 0; x < visible_columns; ++x) {
            const struct cell *cell = &cells[source_row][first_column + x];
            uint32_t color = cell->foreground == TERM_ACCENT ? theme->accent : cell->foreground;
            gfx_char(area.x + PADDING + x * GFX_CHAR_WIDTH,
                     area.y + PADDING + y * GFX_CHAR_HEIGHT,
                     cell->character, color, cell->background);
        }
    }
    if (focused) {
        int cursor_x = column - first_column, cursor_y = row - first_row;
        if (cursor_x >= 0 && cursor_x < visible_columns && cursor_y >= 0 && cursor_y < visible_rows)
            gfx_rect(area.x + PADDING + cursor_x * GFX_CHAR_WIDTH,
                     area.y + PADDING + cursor_y * GFX_CHAR_HEIGHT + GFX_CHAR_HEIGHT - 2,
                     GFX_CHAR_WIDTH, 2, theme->accent);
    }
}
