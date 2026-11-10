/*
 * input.c: every keyboard's keys, held keys and repeat, and the mouse's
 * presence.
 *
 * Nothing about a keystroke's way is particular to the 8042: the held keys,
 * the repeat and the hand-off to the console's keyboard (kd.c), which
 * translates it, are the same whether the key came off a port or out of a
 * USB report. What is particular to the 8042, reading the port, is in its
 * interrupt handler, and what is particular to USB, comparing reports, is in
 * the HID driver.
 */

#include "proc/suspend.h"
#include "input.h"

#include "display/blank.h"
#include "evdev.h"
#include "kd.h"
#include "lib/kstring.h"
#include "lib/spinlock.h"
#include "kbd.h"
#include "timer.h"

/* The boot processor's tick, TICK_HZ a second, in milliseconds. */
#define TICK_MS (1000u / TICK_HZ)

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

/* Whether a mouse of any kind was found. */
static int mouse_present;

/** The tick's count in milliseconds, which wraps after 49 days. */
static uint32_t now_ms(void)
{
    return (uint32_t)kernel_ticks * TICK_MS;
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
 * `has_scan`; the console's keyboard, which translates it; and the
 * console's raw view. Every event ends a suspend and puts off the blanking,
 * as a key always has.
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
    if (evdev_grabbed(kb->dev)) {
        return;
    }
    kd_key_event(kb, keycode, value);
    n = input_raw_bytes(keycode, value, raw);
    for (i = 0; i < n; i++) {
        kd_raw_code(raw[i]);
    }
}

void input_written(uint16_t type, uint16_t code, int32_t value)
{
    static const unsigned int leds[3] = {LED_NUM, LED_CAP, LED_SCR};
    uint8_t raw[3];
    unsigned int mask, now;
    int delay, period, i, n;

    switch (type) {
    case EV_LED:
        if (code >= 3) {
            return;
        }
        mask = input_leds();
        now = value ? mask | leds[code] : mask & ~leds[code];
        if (now != mask) {
            input_set_leds(now);
        }
        return;
    case EV_REP:
        if (code > REP_PERIOD || value < 0) {
            return;
        }
        input_get_repeat(&delay, &period);
        if ((code == REP_DELAY ? delay : period) != value) {
            input_set_repeat(code == REP_DELAY ? value : delay,
                             code == REP_PERIOD ? value : period);
        }
        return;
    case EV_KEY:
        console_poke();
        kd_key_event(NULL, code, value);
        n = input_raw_bytes(code, value, raw);
        for (i = 0; i < n; i++) {
            kd_raw_code(raw[i]);
        }
        return;
    default:
        return;
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
    struct evdev *dev;

    k_memset(keys, 0, sizeof(keys));
    set1_keys(keys);
    dev = evdev_register(name, bustype, vendor, product, keys);
    evdev_describe(dev, phys, EVDEV_SCAN | EVDEV_KEYBOARD);
    input_keyboard_attach_dev(kb, dev);
}

void input_keyboard_attach_dev(struct input_keyboard *kb, struct evdev *dev)
{
    uint32_t flags;

    kb->dev = dev;
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
