#ifndef NOVA_INPUT_H
#define NOVA_INPUT_H
#include <stdbool.h>
#include <stdint.h>
enum {
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_DELETE,
    KEY_F1 = 0x120, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_ALT_TAB = 0x140, KEY_CTRL_S, KEY_CTRL_L, KEY_CTRL_ALT_T,
    /* Modifier keys arrive as their own events so games can see Shift/Ctrl/Alt. */
    KEY_RSHIFT = 0x150, KEY_RCTRL, KEY_RALT
};
typedef enum { INPUT_KEY, INPUT_MOUSE } InputType;
typedef struct {
    InputType type;
    int key, x, y;
    uint8_t buttons;
    bool pressed; /* keyboard events only: false means the key was released */
} InputEvent;
/* Polling PS/2 input; mouse starts at the screen center. No IRQs required. */
void input_init(int width, int height);
/* Mouse buttons: left = 1, right = 2, middle = 4; x/y are clamped pixels. */
bool input_poll(InputEvent *event);
#endif
