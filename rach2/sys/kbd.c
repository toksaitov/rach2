/*
 * kbd.c: the ring of raw scancodes and the map between set-1 codes and
 * Linux's key codes, as kbd.h describes them.
 */
#include "kbd.h"

/*
 * The scancode ring. The input layer is its one producer, at `tail`, and
 * the /fb reader its one consumer, at `head`; a slot stays empty to tell a
 * full ring from an empty one, and a full ring drops the new code rather
 * than overwrite an unread one.
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

int kbd_scancode_room(void)
{
    return (scancode_head - scancode_tail - 1 + SCANCODE_BUF_SIZE) % SCANCODE_BUF_SIZE;
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

/*
 * The extended keys' codes, the code after an E0 prefix, and Linux's key
 * for each, as its keyboard driver gives them for a translated set 2
 * keyboard; Pause, whose E1 sequence ends in 0x45, is among them.
 */
static const uint16_t extended_keys[][2] = {
    {0x10, 165}, /* KEY_PREVIOUSSONG */
    {0x19, 163}, /* KEY_NEXTSONG */
    {0x1C, 96},  /* KEY_KPENTER */
    {0x1D, 97},  /* KEY_RIGHTCTRL */
    {0x20, 113}, /* KEY_MUTE */
    {0x21, 140}, /* KEY_CALC */
    {0x22, 164}, /* KEY_PLAYPAUSE */
    {0x24, 166}, /* KEY_STOPCD */
    {0x2E, 114}, /* KEY_VOLUMEDOWN */
    {0x30, 115}, /* KEY_VOLUMEUP */
    {0x32, 172}, /* KEY_HOMEPAGE */
    {0x35, 98},  /* KEY_KPSLASH */
    {0x37, 99},  /* KEY_SYSRQ: Print Screen */
    {0x38, 100}, /* KEY_RIGHTALT */
    {0x45, 119}, /* KEY_PAUSE */
    {0x47, 102}, /* KEY_HOME */
    {0x48, 103}, /* KEY_UP */
    {0x49, 104}, /* KEY_PAGEUP */
    {0x4B, 105}, /* KEY_LEFT */
    {0x4D, 106}, /* KEY_RIGHT */
    {0x4F, 107}, /* KEY_END */
    {0x50, 108}, /* KEY_DOWN */
    {0x51, 109}, /* KEY_PAGEDOWN */
    {0x52, 110}, /* KEY_INSERT */
    {0x53, 111}, /* KEY_DELETE */
    {0x5B, 125}, /* KEY_LEFTMETA */
    {0x5C, 126}, /* KEY_RIGHTMETA */
    {0x5D, 127}, /* KEY_COMPOSE */
    {0x5E, 116}, /* KEY_POWER */
    {0x5F, 142}, /* KEY_SLEEP */
    {0x63, 143}, /* KEY_WAKEUP */
    {0x65, 217}, /* KEY_SEARCH */
    {0x66, 156}, /* KEY_BOOKMARKS */
    {0x67, 173}, /* KEY_REFRESH */
    {0x68, 128}, /* KEY_STOP */
    {0x69, 159}, /* KEY_FORWARD */
    {0x6A, 158}, /* KEY_BACK */
    {0x6B, 157}, /* KEY_COMPUTER */
    {0x6C, 155}, /* KEY_MAIL */
    {0x6D, 226}  /* KEY_MEDIA */
};

/* The plain codes past the keypad that have a key, and Linux's key. */
static const uint16_t plain_keys[][2] = {
    {0x54, 99},  /* KEY_SYSRQ: Alt with Print Screen */
    {0x56, 86},  /* KEY_102ND */
    {0x57, 87},  /* KEY_F11 */
    {0x58, 88},  /* KEY_F12 */
    {0x59, 117}, /* KEY_KPEQUAL */
    {0x64, 183}, /* KEY_F13, and on to KEY_F23 at 0x6E */
    {0x65, 184}, {0x66, 185}, {0x67, 186}, {0x68, 187}, {0x69, 188}, {0x6A, 189},
    {0x6B, 190}, {0x6C, 191}, {0x6D, 192}, {0x6E, 193}, {0x70, 93}, /* KEY_KATAKANAHIRAGANA */
    {0x73, 89},                                                     /* KEY_RO */
    {0x76, 194},                                                    /* KEY_F24 */
    {0x79, 92},                                                     /* KEY_HENKAN */
    {0x7B, 94},                                                     /* KEY_MUHENKAN */
    {0x7D, 124},                                                    /* KEY_YEN */
    {0x7E, 121}                                                     /* KEY_KPCOMMA */
};

int set1_keycode(unsigned scan)
{
    const uint16_t (*table)[2] = (scan & 0x80) ? extended_keys : plain_keys;
    size_t n = (scan & 0x80) ? sizeof(extended_keys) / sizeof(extended_keys[0])
                             : sizeof(plain_keys) / sizeof(plain_keys[0]);
    size_t i;

    scan &= 0xFF;
    if (scan >= 0x01 && scan <= 0x53) {
        return (int)scan; /* Linux numbered its keys after these codes */
    }
    for (i = 0; i < n; i++) {
        if (table[i][0] == (scan & 0x7F)) {
            return table[i][1];
        }
    }
    return 0;
}

unsigned set1_scancode(int keycode)
{
    size_t i;

    if (keycode >= 0x01 && keycode <= 0x53) {
        return (unsigned)keycode;
    }
    for (i = 0; i < sizeof(extended_keys) / sizeof(extended_keys[0]); i++) {
        if (extended_keys[i][1] == keycode) {
            return 0x80u | extended_keys[i][0];
        }
    }
    for (i = 0; i < sizeof(plain_keys) / sizeof(plain_keys[0]); i++) {
        if (plain_keys[i][1] == keycode) {
            return plain_keys[i][0];
        }
    }
    return 0;
}

void set1_keys(uint8_t *keys)
{
    unsigned scan;

    for (scan = 1; scan < 0x100; scan++) {
        int code = set1_keycode(scan);

        if (code != 0) {
            keys[code >> 3] |= (uint8_t)(1u << (code & 7));
        }
    }
}
