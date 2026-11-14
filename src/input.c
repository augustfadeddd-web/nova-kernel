#include "input.h"

enum {
    DATA_PORT = 0x60,
    STATUS_PORT = 0x64,
    WAIT_LIMIT = 100000
};

static bool left_shift, right_shift, left_ctrl, right_ctrl, left_alt, right_alt;
static bool caps_lock, caps_pressed, num_lock, num_pressed, extended;
static unsigned int pause_remaining;
static uint8_t mouse_packet[3];
static uint8_t mouse_buttons;
static unsigned int mouse_bytes;
static int mouse_x, mouse_y, screen_width, screen_height;

/* US keyboard layout, using the firmware's translated scan-code set 1. */
static const char plain[128] = {
    [0x01] = 27,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x37] = '*', [0x39] = ' ',
    [0x4A] = '-', [0x4E] = '+'
};

static uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static bool wait_to_write(void)
{
    for (unsigned int i = 0; i < WAIT_LIMIT; ++i) {
        if (!(inb(STATUS_PORT) & 2))
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool controller_command(uint8_t command)
{
    if (!wait_to_write())
        return false;
    outb(STATUS_PORT, command);
    return true;
}

static bool controller_data(uint8_t data)
{
    if (!wait_to_write())
        return false;
    outb(DATA_PORT, data);
    return true;
}

static bool controller_reply(uint8_t *reply)
{
    for (unsigned int i = 0; i < WAIT_LIMIT; ++i) {
        uint8_t status = inb(STATUS_PORT);
        if (status & 1) {
            uint8_t byte = inb(DATA_PORT);
            if (!(status & 0xE0)) {
                *reply = byte;
                return true;
            }
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool mouse_command(uint8_t command)
{
    /* A mouse may request one resend; all waits have a fixed upper bound. */
    for (unsigned int attempt = 0; attempt < 2; ++attempt) {
        if (!controller_command(0xD4) || !controller_data(command))
            return false;
        for (unsigned int i = 0; i < WAIT_LIMIT; ++i) {
            uint8_t status = inb(STATUS_PORT);
            if (status & 1) {
                uint8_t reply = inb(DATA_PORT);
                if ((status & 0xC0) || !(status & 0x20))
                    continue;
                if (reply == 0xFA)
                    return true;
                if (reply == 0xFE)
                    break;
            }
            __asm__ volatile ("pause");
        }
    }
    return false;
}

void input_init(int width, int height)
{
    left_shift = right_shift = left_ctrl = right_ctrl = false;
    left_alt = right_alt = caps_lock = caps_pressed = false;
    num_lock = num_pressed = extended = false;
    pause_remaining = mouse_bytes = 0;
    mouse_buttons = 0;
    screen_width = width > 0 ? width : 1;
    screen_height = height > 0 ? height : 1;
    mouse_x = screen_width / 2;
    mouse_y = screen_height / 2;

    /* Firmware can leave old keystrokes or a mouse self-test reply behind. */
    for (unsigned int i = 0; i < 256 && (inb(STATUS_PORT) & 1); ++i)
        (void)inb(DATA_PORT);

    (void)controller_command(0xA8); /* Enable the auxiliary device's clock. */
    uint8_t configuration;
    if (controller_command(0x20) && controller_reply(&configuration)) {
        /* Poll both devices; preserve the firmware's keyboard translation. */
        configuration &= (uint8_t)~0x23;
        if (controller_command(0x60))
            (void)controller_data(configuration);
    }
    (void)controller_command(0xAE); /* Keep the keyboard interface enabled. */
    (void)mouse_command(0xF6);      /* Defaults: standard three-byte packets. */
    (void)mouse_command(0xF4);      /* Enable stream reporting. */
}

static int shifted_symbol(int character)
{
    switch (character) {
    case '1': return '!';
    case '2': return '@';
    case '3': return '#';
    case '4': return '$';
    case '5': return '%';
    case '6': return '^';
    case '7': return '&';
    case '8': return '*';
    case '9': return '(';
    case '0': return ')';
    case '-': return '_';
    case '=': return '+';
    case '[': return '{';
    case ']': return '}';
    case ';': return ':';
    case '\'': return '"';
    case '`': return '~';
    case '\\': return '|';
    case ',': return '<';
    case '.': return '>';
    case '/': return '?';
    default: return character;
    }
}

/* A make is encoded as the key itself, a break as -(key + 2); both stay
 * distinct from the -1 that means "this byte produced no event". */
static int key_event(int key, bool pressed)
{
    return pressed ? key : -(key + 2);
}

static int extended_key(uint8_t code)
{
    switch (code) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x53: return KEY_DELETE;
    case 0x1C: return '\n';
    case 0x35: return '/';
    default: return -1; /* Includes Print Screen's fake Shift bytes. */
    }
}

static int decode_keyboard(uint8_t scan)
{
    if (scan == 0xFA || scan == 0xFE)
        return -1; /* Device ACK or resend, not a keystroke. */
    if (pause_remaining) {
        --pause_remaining;
        return -1;
    }
    if (scan == 0xE1) {
        pause_remaining = 5;
        extended = false;
        return -1;
    }
    if (scan == 0xE0) {
        extended = true;
        return -1;
    }

    bool released = (scan & 0x80) != 0;
    uint8_t code = scan & 0x7F;
    if (extended) {
        extended = false;
        if (code == 0x1D) {
            bool was = left_ctrl || right_ctrl;
            right_ctrl = !released;
            if (was == (left_ctrl || right_ctrl)) return -1;
            return key_event(KEY_RCTRL, !released);
        }
        if (code == 0x38) {
            bool was = left_alt || right_alt;
            right_alt = !released;
            if (was == (left_alt || right_alt)) return -1;
            return key_event(KEY_RALT, !released);
        }
        int key = extended_key(code);
        return key == -1 ? -1 : key_event(key, !released);
    }

    if (code == 0x2A || code == 0x36) {
        bool was = left_shift || right_shift;
        if (code == 0x2A) left_shift = !released; else right_shift = !released;
        if (was == (left_shift || right_shift)) return -1;
        return key_event(KEY_RSHIFT, !released);
    }
    if (code == 0x1D) {
        bool was = left_ctrl || right_ctrl;
        left_ctrl = !released;
        if (was == (left_ctrl || right_ctrl)) return -1;
        return key_event(KEY_RCTRL, !released);
    }
    if (code == 0x38) {
        bool was = left_alt || right_alt;
        left_alt = !released;
        if (was == (left_alt || right_alt)) return -1;
        return key_event(KEY_RALT, !released);
    }
    if (code == 0x3A) {
        if (!released && !caps_pressed) caps_lock = !caps_lock;
        caps_pressed = !released;
        return -1;
    }
    if (code == 0x45) {
        if (!released && !num_pressed) num_lock = !num_lock;
        num_pressed = !released;
        return -1;
    }

    bool ctrl = left_ctrl || right_ctrl;
    bool alt = left_alt || right_alt;
    if (!released) {
        if (ctrl && alt && code == 0x14) return KEY_CTRL_ALT_T;
        if (alt && code == 0x0F) return KEY_ALT_TAB;
        if (ctrl && code == 0x1F) return KEY_CTRL_S;
        if (ctrl && code == 0x26) return KEY_CTRL_L;
    }
    if (ctrl || alt)
        return -1; /* combos fire on press only; the rest is swallowed both ways */
    if (code >= 0x3B && code <= 0x44)
        return key_event(KEY_F1 + code - 0x3B, !released);
    if (code == 0x57) return key_event(KEY_F11, !released);
    if (code == 0x58) return key_event(KEY_F12, !released);

    bool shift = left_shift || right_shift;
    if (code >= 0x47 && code <= 0x53) {
        static const char keypad[13] = {
            '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.'
        };
        int key = -1;
        if (num_lock != shift) key = keypad[code - 0x47];
        else switch (code) {
        case 0x47: key = KEY_HOME; break;
        case 0x48: key = KEY_UP; break;
        case 0x4B: key = KEY_LEFT; break;
        case 0x4D: key = KEY_RIGHT; break;
        case 0x4F: key = KEY_END; break;
        case 0x50: key = KEY_DOWN; break;
        case 0x53: key = KEY_DELETE; break;
        case 0x4A: key = '-'; break;
        case 0x4E: key = '+'; break;
        default: break;
        }
        return key == -1 ? -1 : key_event(key, !released);
    }
    int character = plain[code];
    if (!character)
        return -1;
    if (character >= 'a' && character <= 'z')
        character = shift != caps_lock ? character - 'a' + 'A' : character;
    else
        character = shift ? shifted_symbol(character) : character;
    return key_event(character, !released);
}

static bool decode_mouse(uint8_t byte, InputEvent *event)
{
    if (!mouse_bytes) {
        /* Every standard packet starts with bit 3 set. ACKs are not packets. */
        if (byte == 0xFA || byte == 0xFE || !(byte & 8))
            return false;
    }
    mouse_packet[mouse_bytes++] = byte;
    if (mouse_bytes != 3)
        return false;
    mouse_bytes = 0;
    uint8_t flags = mouse_packet[0];
    if (flags & 0xC0)
        return false; /* Movement overflow: discard the complete packet. */

    int dx = mouse_packet[1] - ((flags & 0x10) ? 256 : 0);
    int dy = mouse_packet[2] - ((flags & 0x20) ? 256 : 0);
    mouse_x += dx;
    mouse_y -= dy; /* PS/2 Y grows upward; screen Y grows downward. */
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= screen_width) mouse_x = screen_width - 1;
    if (mouse_y >= screen_height) mouse_y = screen_height - 1;

    event->type = INPUT_MOUSE;
    event->key = 0;
    event->pressed = true; /* Buttons ride along; the desktop watches transitions. */
    event->x = mouse_x;
    event->y = mouse_y;
    mouse_buttons = flags & 7; /* Left = 1, right = 2, middle = 4. */
    event->buttons = mouse_buttons;
    return true; /* A zero-delta packet can still press or release a button. */
}

bool input_poll(InputEvent *event)
{
    if (!event)
        return false;
    for (unsigned int i = 0; i < 64; ++i) {
        uint8_t status = inb(STATUS_PORT);
        if (!(status & 1))
            return false;
        uint8_t byte = inb(DATA_PORT);
        if (status & 0xC0) {
            if (status & 0x20) mouse_bytes = 0;
            else { extended = false; pause_remaining = 0; }
            continue;
        }
        if (status & 0x20) {
            if (decode_mouse(byte, event))
                return true;
        } else {
            int key = decode_keyboard(byte);
            if (key != -1) {
                event->type = INPUT_KEY;
                event->pressed = key >= 0;
                event->key = key >= 0 ? key : -(key + 2);
                event->x = mouse_x;
                event->y = mouse_y;
                event->buttons = mouse_buttons;
                return true;
            }
        }
    }
    return false;
}
