/*
 * input.c: every keyboard's keys, held keys and repeat, today's translation
 * of them for the console, and the mouse's presence and its USB reports.
 *
 * Nothing about deciding where a keystroke goes is particular to the 8042:
 * the held keys, the repeat, the Ctrl+C interception, the scrollback keys and
 * the choice between the line discipline and the debug shell are the same
 * whether the key came off a port or out of a USB report. What is particular
 * to the 8042, reading the port, is in its interrupt handler, and what is
 * particular to USB, comparing reports, is in the HID driver.
 */

#include "proc/suspend.h"
#include "input.h"

#include "display/blank.h"
#include "display/textcon.h"
#include "evdev.h"
#include "kd.h"
#include "lib/kstring.h"
#include "lib/spinlock.h"
#include "kbd.h"
#include "mouse.h"
#include "kio.h"
#include "klog.h"
#include "proc/wait_queue.h"
#include "timer.h"
#include "tty/tty.h"

/* The boot processor's tick, 100 a second, in milliseconds. */
#define TICK_MS 10u

/* The ring's slots a press or a repeat leaves free, so that the releases of
 * the keys held still fit when its reader falls behind. */
#define RING_RESERVE 16

/* Linux's key codes of the modifiers and locks, which do not repeat. */
#define KEY_LEFTCTRL   29
#define KEY_LEFTSHIFT  42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTALT    56
#define KEY_CAPSLOCK   58
#define KEY_NUMLOCK    69
#define KEY_SCROLLLOCK 70
#define KEY_RIGHTCTRL  97
#define KEY_RIGHTALT   100
#define KEY_LEFTMETA   125
#define KEY_RIGHTMETA  126

/* The modifier keys in the order of their INPUT_MOD_* bits. */
static const unsigned int modifier_keys[8] = {KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL,
                                              KEY_RIGHTCTRL, KEY_LEFTALT,    KEY_RIGHTALT,
                                              KEY_LEFTMETA,  KEY_RIGHTMETA};

/* Over the keyboards' list, their held keys, the modifiers' counts, the
 * repeat and the LEDs. */
static struct spinlock input_lock = SPINLOCK_INIT;

/* The keyboards attached. */
static struct input_keyboard *keyboards;

/* For each modifier key, in the order of `modifier_keys`, the keyboards that
 * hold it. */
static int modifier_count[8];

/* The key that repeats, NULL `repeat_kb` for none, and when its next repeat
 * is due, in milliseconds of the tick's count. */
static struct input_keyboard *repeat_kb;
static unsigned int repeat_key;
static uint32_t repeat_due;

/* The repeat's times in milliseconds, as `input_set_repeat` sets them; a
 * period of 0 is no repeat. */
static int repeat_delay_ms = 250;
static int repeat_period_ms = 33;

/* Whether today's translation saw the extended prefix last. */
static int saw_e0;

/* Whether a mouse of any kind was found. */
static int mouse_present;

/** The tick's count in milliseconds, which wraps after 49 days. */
static uint32_t now_ms(void)
{
    return kernel_ticks * TICK_MS;
}

/** The evdev LED bits, 1 << LED_*, of `mask`, LED_* of kd.h. */
static unsigned int evdev_leds(unsigned int mask)
{
    return ((mask & LED_NUM) ? 1u << LED_NUML : 0) | ((mask & LED_CAP) ? 1u << LED_CAPSL : 0) |
           ((mask & LED_SCR) ? 1u << LED_SCROLLL : 0);
}

/* The LEDs every keyboard is to show, LED_* of kd.h. One word, which each
 * keyboard's driver reads and compares with what its keyboard shows, so a
 * driver needs no lock to read it and a keyboard connected later sees it;
 * the lock keeps the event devices' reports of it in order. */
static volatile unsigned int leds_wanted;

void input_set_repeat(int delay_ms, int period_ms)
{
    struct input_keyboard *kb;
    uint32_t flags;

    flags = spin_lock_irqsave(&input_lock);
    if (delay_ms >= 0) {
        repeat_delay_ms = delay_ms;
    }
    if (period_ms >= 0) {
        repeat_period_ms = period_ms;
    }
    for (kb = keyboards; kb; kb = kb->next) {
        evdev_report_repeat(kb->dev, repeat_delay_ms, repeat_period_ms);
    }
    spin_unlock_irqrestore(&input_lock, flags);
}

void input_get_repeat(int *delay_ms, int *period_ms)
{
    uint32_t flags;

    flags = spin_lock_irqsave(&input_lock);
    *delay_ms = repeat_delay_ms;
    *period_ms = repeat_period_ms;
    spin_unlock_irqrestore(&input_lock, flags);
}

void input_cancel_repeat(void)
{
    uint32_t flags;

    flags = spin_lock_irqsave(&input_lock);
    repeat_kb = NULL;
    spin_unlock_irqrestore(&input_lock, flags);
}

void input_set_leds(unsigned int mask)
{
    struct input_keyboard *kb;
    uint32_t flags;

    flags = spin_lock_irqsave(&input_lock);
    leds_wanted = mask & (LED_SCR | LED_NUM | LED_CAP);
    for (kb = keyboards; kb; kb = kb->next) {
        evdev_report_leds(kb->dev, evdev_leds(leds_wanted));
    }
    spin_unlock_irqrestore(&input_lock, flags);
}

unsigned int input_leds(void)
{
    return leds_wanted;
}

int input_mouse_present(void)
{
    return mouse_present;
}

void input_mouse_found(void)
{
    mouse_present = 1;
}

void input_mouse(int dx, int dy, int buttons)
{
    mouse_push_packet((uint8_t)buttons, dx, dy);
}

/**
 * Send the escape sequence of the extended key `scancode`, the code after an
 * E0 prefix, to the console's line discipline, as the `rach2` terminfo entry
 * lists it: the arrows, Home and End as xterm sends them, and Insert, Delete,
 * Page Up and Page Down as VT220 editing keys.
 *
 * @return 1 when `scancode` is one of those keys, 0 otherwise.
 */
static int extended_key(uint8_t scancode)
{
    const char *seq;

    switch (scancode) {
    case 0x48:
        seq = "\033[A";
        break;
    case 0x50:
        seq = "\033[B";
        break;
    case 0x4D:
        seq = "\033[C";
        break;
    case 0x4B:
        seq = "\033[D";
        break;
    case 0x47:
        seq = "\033[H";
        break;
    case 0x4F:
        seq = "\033[F";
        break;
    case 0x52:
        seq = "\033[2~";
        break;
    case 0x53:
        seq = "\033[3~";
        break;
    case 0x49:
        seq = "\033[5~";
        break;
    case 0x51:
        seq = "\033[6~";
        break;
    default:
        return 0;
    }
    while (*seq) {
        tty_input_char(&console_tty, (unsigned char)*seq++);
    }
    return 1;
}

/**
 * Give the key of `scancode`, `c` in the keymap and extended when `was_e0`,
 * to the console's line discipline, for a reader parked on it or the session
 * that owns it, with the modifiers `mods` held. Shift with Page Up or Page
 * Down scrolls the screen back instead, and any other key first returns it
 * to the live screen.
 */
static void session_key(uint8_t scancode, char c, int was_e0, unsigned int mods)
{
    unsigned char tc = (unsigned char)c;
    int shift_held = (mods & INPUT_MOD_SHIFT) != 0;
    int ctrl_held = (mods & INPUT_MOD_CTRL) != 0;
    int alt_held = (mods & (INPUT_MOD_ALT | INPUT_MOD_ALTGR)) != 0;

    if (was_e0 && shift_held && scancode == 0x49) {
        k_scrollback_page_up();
        return;
    }
    if (was_e0 && shift_held && scancode == 0x51) {
        k_scrollback_page_down();
        return;
    }
    if (k_scrollback_is_active()) {
        k_scrollback_end();
    }
    if ((was_e0 && extended_key(scancode)) || c == 0) {
        return;
    }
    if (ctrl_held && c >= 'a' && c <= 'z') {
        tc = (unsigned char)(c - 'a' + 1);
    } else if (ctrl_held && c >= 'A' && c <= 'Z') {
        tc = (unsigned char)(c - 'A' + 1);
    } else if (c == '\b') {
        tc = 0x7F;
    }
    if (alt_held) {
        tty_input_char(&console_tty, 0x1B);
    }
    tty_input_char(&console_tty, tc);
}

/** Scroll the kernel shell's console back for the extended key `scancode`
 * pressed, Page Up, Page Down, Home or End, and say whether it was one. */
static int shell_scroll_key(uint8_t scancode)
{
    switch (scancode) {
    case 0x49:
        k_scrollback_page_up();
        return 1;
    case 0x51:
        k_scrollback_page_down();
        return 1;
    case 0x47:
        k_scrollback_home();
        return 1;
    case 0x4F:
        k_scrollback_end();
        return 1;
    default:
        return 0;
    }
}

/**
 * Today's translation of one set-1 byte of the raw view for the console,
 * as input.h describes it: the modifiers are `input_modifiers`', so their
 * own codes, and the Pause sequence's E1 1D, do nothing here. While the
 * console's keyboard does not translate, the byte is `kd_raw_code`'s.
 */
static void console_key(uint8_t scancode)
{
    unsigned int mods = input_modifiers();
    int was_e0 = saw_e0;
    char c;

    saw_e0 = 0;
    if (!kd_translates()) {
        kd_raw_code(scancode);
        return;
    }
    if (scancode == 0xE0) {
        saw_e0 = 1;
        return;
    }
    switch (scancode & 0x7F) {
    case 0x1D:
    case 0x2A:
    case 0x36:
    case 0x38:
    case 0x61: /* E1 */
        return;
    default:
        break;
    }

    /* The high bit marks a release. */
    if (scancode & 0x80) {
        return;
    }

    /* 'C' is scancode 0x2E. */
    if ((mods & INPUT_MOD_CTRL) && scancode == 0x2E &&
        !(wq_active(&console_tty.input_wq) || console_tty.sid != 0)) {
        shell_interrupt();
        return;
    }

    if (was_e0 && shell_reads_input() && shell_scroll_key(scancode)) {
        return;
    }

    /* Of the extended keys only the keypad's Enter and slash have a
     * character, their twins'; Mute, E0 20, is not a D. */
    if (was_e0 && scancode != 0x1C && scancode != 0x35) {
        c = 0;
    } else {
        c = (char)((mods & INPUT_MOD_SHIFT) ? EN_us_shift[scancode & 0x7F]
                                            : EN_us[scancode & 0x7F]);
    }

    if (wq_active(&console_tty.input_wq) || console_tty.sid != 0) {
        session_key(scancode, c, was_e0, mods);
    } else if (c != 0 && shell_reads_input()) {
        shell_input_char(c);
    }
}

unsigned int input_modifiers(void)
{
    unsigned int mods = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (modifier_count[i] > 0) {
            mods |= 1u << i;
        }
    }
    return mods;
}

int input_raw_bytes(unsigned int keycode, int value, uint8_t *out)
{
    unsigned scan = set1_scancode((int)keycode);
    uint8_t brk = value == KD_KEY_RELEASE ? 0x80u : 0u;
    int n = 0;

    if (scan == 0) {
        return 0;
    }
    if (keycode == KEY_PAUSE) {
        out[0] = 0xE1;
        out[1] = (uint8_t)(0x1Du | brk);
        out[2] = (uint8_t)(0x45u | brk);
        return 3;
    }
    if (scan & 0x80u) {
        out[n++] = 0xE0;
    }
    out[n++] = (uint8_t)((scan & 0x7Fu) | brk);
    return n;
}

/**
 * Give one accepted event of `kb`, `value` KD_KEY_*, to its consumers in
 * input.h's order: the event device, with MSC_SCAN `scan` first when
 * `has_scan`; the console's keyboard; the scancode ring, where a press or a
 * repeat leaves RING_RESERVE slots free; and today's translation. Every
 * event ends a suspend and puts off the blanking, as a key always has.
 */
static void deliver(struct input_keyboard *kb, unsigned int keycode, int value, uint32_t scan,
                    int has_scan)
{
    uint8_t raw[3];
    int i, n;

    suspend_wake();
    console_poke();
    if (has_scan) {
        evdev_report(kb->dev, EV_MSC, MSC_SCAN, (int32_t)scan);
    }
    evdev_report(kb->dev, EV_KEY, (uint16_t)keycode, value);
    evdev_report(kb->dev, EV_SYN, SYN_REPORT, 0);
    kd_key_event(kb, keycode, value);
    n = input_raw_bytes(keycode, value, raw);
    if (value == KD_KEY_RELEASE || kbd_scancode_room() >= n + RING_RESERVE) {
        for (i = 0; i < n; i++) {
            kbd_push_scancode(raw[i]);
        }
    }
    for (i = 0; i < n; i++) {
        console_key(raw[i]);
    }
}

/** The index of `keycode` in `modifier_keys`, or -1 for another key. */
static int modifier_index(unsigned int keycode)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (modifier_keys[i] == keycode) {
            return i;
        }
    }
    return -1;
}

/**
 * `input_key_event`, with MSC_SCAN `scan` reported only when `has_scan`:
 * the held keys, the modifiers' counts and the repeat change under the lock,
 * and the event goes out after it.
 */
static void key_event(struct input_keyboard *kb, uint32_t scan, unsigned int keycode, int down,
                      int has_scan)
{
    uint8_t bit = (uint8_t)(1u << (keycode & 7));
    uint8_t *held = &kb->down[keycode >> 3];
    uint32_t flags;
    int mod;

    if (keycode == 0 || keycode >= INPUT_KEY_BYTES * 8) {
        if (has_scan) {
            evdev_report_key(kb->dev, scan, 0, down);
        }
        return;
    }
    mod = modifier_index(keycode);
    flags = spin_lock_irqsave(&input_lock);
    if (down ? (*held & bit) != 0 : (*held & bit) == 0) {
        spin_unlock_irqrestore(&input_lock, flags);
        return;
    }
    if (down) {
        *held |= bit;
        if (mod >= 0) {
            modifier_count[mod]++;
        } else if (keycode != KEY_CAPSLOCK && keycode != KEY_NUMLOCK && keycode != KEY_SCROLLLOCK) {
            repeat_kb = kb;
            repeat_key = keycode;
            repeat_due = now_ms() + (uint32_t)repeat_delay_ms;
        }
    } else {
        *held &= (uint8_t)~bit;
        if (mod >= 0) {
            modifier_count[mod]--;
        }
        if (repeat_kb == kb && repeat_key == keycode) {
            repeat_kb = NULL;
        }
    }
    spin_unlock_irqrestore(&input_lock, flags);
    deliver(kb, keycode, down ? KD_KEY_PRESS : KD_KEY_RELEASE, scan, has_scan);
}

void input_key_event(struct input_keyboard *kb, uint32_t scan, unsigned int keycode, int down)
{
    key_event(kb, scan, keycode, down, 1);
}

void input_tick(void)
{
    struct input_keyboard *kb;
    uint32_t now = now_ms();
    unsigned int key;
    uint32_t flags;
    int sent;

    for (sent = 0;; sent++) {
        flags = spin_lock_irqsave(&input_lock);
        kb = repeat_kb;
        key = repeat_key;
        if (!kb || repeat_period_ms <= 0 || (int32_t)(now - repeat_due) < 0) {
            spin_unlock_irqrestore(&input_lock, flags);
            return;
        }
        if (sent == 4) {
            repeat_due = now + (uint32_t)repeat_period_ms;
            spin_unlock_irqrestore(&input_lock, flags);
            return;
        }
        repeat_due += (uint32_t)repeat_period_ms;
        spin_unlock_irqrestore(&input_lock, flags);
        deliver(kb, key, KD_KEY_REPEAT, 0, 0);
    }
}

void input_keyboard_attach(struct input_keyboard *kb, const char *name, const char *phys,
                           uint16_t bustype, uint16_t vendor, uint16_t product)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    uint32_t flags;

    k_memset(keys, 0, sizeof(keys));
    set1_keys(keys);
    kb->dev = evdev_register(name, bustype, vendor, product, keys);
    evdev_describe(kb->dev, phys, EVDEV_SCAN | EVDEV_KEYBOARD);
    flags = spin_lock_irqsave(&input_lock);
    evdev_report_repeat(kb->dev, repeat_delay_ms, repeat_period_ms);
    evdev_report_leds(kb->dev, evdev_leds(leds_wanted));
    kb->next = keyboards;
    keyboards = kb;
    spin_unlock_irqrestore(&input_lock, flags);
}

void input_keyboard_detach(struct input_keyboard *kb)
{
    struct input_keyboard **p;
    unsigned int code;
    uint32_t flags;

    flags = spin_lock_irqsave(&input_lock);
    for (p = &keyboards; *p; p = &(*p)->next) {
        if (*p == kb) {
            *p = kb->next;
            break;
        }
    }
    kb->next = NULL;
    if (repeat_kb == kb) {
        repeat_kb = NULL;
    }
    spin_unlock_irqrestore(&input_lock, flags);
    for (code = 1; code < INPUT_KEY_BYTES * 8; code++) {
        if (kb->down[code >> 3] & (1u << (code & 7))) {
            key_event(kb, 0, code, 0, 0);
        }
    }
    evdev_unregister(kb->dev);
    kb->dev = NULL;
    kb->prefix = 0;
    kb->skip = 0;
}

void input_set1_event(struct input_keyboard *kb, uint8_t code)
{
    unsigned scan;

    if (code == 0xE0 || code == 0xE1) {
        kb->prefix = code;
        kb->skip = code == 0xE1;
        return;
    }
    if (kb->skip) {
        kb->skip = 0;
        return;
    }
    if (code == 0x00 || code == 0xFA || code == 0xFE || code == 0xFF) {
        kb->prefix = 0;
        return;
    }
    scan = (code & 0x7Fu) | (kb->prefix ? 0x80u : 0u);
    kb->prefix = 0;
    if (scan == 0xAA || scan == 0xB6) {
        return;
    }
    input_key_event(kb, scan, (unsigned int)set1_keycode(scan), !(code & 0x80));
}
