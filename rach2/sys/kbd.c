#include "kbd.h"
#include "serial.h"

/*
 * Raw scancode ring buffer for games/graphics mode
 */
#define SCANCODE_BUF_SIZE 64
static uint8_t scancode_buf[SCANCODE_BUF_SIZE];
static volatile int scancode_head = 0;
static volatile int scancode_tail = 0;

void kbd_push_scancode(uint8_t sc)
{
    int next = (scancode_tail + 1) % SCANCODE_BUF_SIZE;
    if (next != scancode_head) {
        scancode_buf[scancode_tail] = sc;
        scancode_tail = next;
    }
}

void kbd_clear_scancodes(void)
{
    scancode_head = 0;
    scancode_tail = 0;
}

int kbd_read_scancodes(uint8_t *buf, int max_count)
{
    int count = 0;
    while (count < max_count && scancode_head != scancode_tail) {
        buf[count++] = scancode_buf[scancode_head];
        scancode_head = (scancode_head + 1) % SCANCODE_BUF_SIZE;
    }
    return count;
}

uint8_t EN_us[128] = {
    0,      /* 0x0: Error code */
    27,     /* 0x1: Escape */
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b',   /* 0x0E: Backspace */
    '\t',   /* 0x0F: Tab */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\n',   /* 0x1C: Enter key */
    0,      /* 0x1D: Left control */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,      /* 0x2A: Left shift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,      /* 0x36: Right shift */
    '*',
    0,      /* 0x38: Left alt */
    ' ',    /* 0x39: Space bar */
    0,      /* 0x3A: Caps Lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x3B...0x44: F1...F10 */
    0,      /* 0x45: Num Lock*/
    0,      /* 0x46: Scroll Lock */
    0,      /* 0x47: Home */
    0,      /* 0x48: Up */
    0,      /* 0x49: Page Up */
    '-',
    0,      /* 0x4B: Left */
    0,
    0,      /* 0x4D: Right */
    '+',
    0,      /* 0x4F: End */
    0,      /* 0x50: Down */
    0,      /* 0x51: Page Down */
    0,      /* 0x52: Insert */
    0,      /* 0x53: Delete */
    0,   0,   0,
    0,      /* 0x57: F11 */
    0,      /* 0x58: F12 */
    0

    /* All others are undefined... */
};

/*
 * Shifted scancode table for US keyboard layout
 * Maps scancodes to characters when Shift is held
 */
uint8_t EN_us_shift[128] = {
    0,      /* 0x0: Error code */
    27,     /* 0x1: Escape */
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b',   /* 0x0E: Backspace */
    '\t',   /* 0x0F: Tab */
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n',   /* 0x1C: Enter key */
    0,      /* 0x1D: Left control */
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,      /* 0x2A: Left shift */
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,      /* 0x36: Right shift */
    '*',
    0,      /* 0x38: Left alt */
    ' ',    /* 0x39: Space bar */
    0,      /* 0x3A: Caps Lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x3B...0x44: F1...F10 */
    0,      /* 0x45: Num Lock*/
    0,      /* 0x46: Scroll Lock */
    0,      /* 0x47: Home */
    0,      /* 0x48: Up */
    0,      /* 0x49: Page Up */
    '-',
    0,      /* 0x4B: Left */
    0,
    0,      /* 0x4D: Right */
    '+',
    0,      /* 0x4F: End */
    0,      /* 0x50: Down */
    0,      /* 0x51: Page Down */
    0,      /* 0x52: Insert */
    0,      /* 0x53: Delete */
    0,   0,   0,
    0,      /* 0x57: F11 */
    0,      /* 0x58: F12 */
    0

    /* All others are undefined... */
};

bool _shift = false;

static void _keyboard_test(isr_params_t *params);

void init_keyboard(void)
{
    register_isr_handler(33, &_keyboard_test);
}

static void _keyboard_test(isr_params_t *params)
{
    (void) params;

    uint8_t scancode = inb(0x60);

    /* Debug: log scancode to serial */
    serial_printf("KBD: sc=0x%x h=%d t=%d\n", scancode, scancode_head, scancode_tail);

    /* Always push raw scancode to buffer for games */
    kbd_push_scancode(scancode);

    switch (scancode) {
        case 0x2A:
        case 0x36:
            _shift = true;
            break;

        case 0xAA:
        case 0xB6:
            _shift = false;
            break;

        default:
            if (!(scancode & 0x80)) {
                if (_shift) {
                    k_putc(EN_us[scancode] -  ('a' - 'A'));
                } else {
                    k_putc(EN_us[scancode]);
                }
            }

            break;
    }
}
