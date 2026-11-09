/*
 * input_hosttest.c: every keyboard's key stream in rach2/sys/input.c, its
 * held keys, modifiers, repeat and raw view, driven by key events and ticks.
 *
 * host-deps: rach2/sys/kbd.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The file is included whole. Its consumers are models: the event devices,
 * the console's keyboard and the console's raw view record what they are
 * given, and the tick's count is a variable the rows advance.
 */

#include "input.c"

int printf(const char *format, ...);

static int fails;

#define CHECK(what, cond)                           \
    do {                                            \
        if (!(cond)) {                              \
            printf("input-test: fail: %s\n", what); \
            fails++;                                \
        }                                           \
    } while (0)

volatile uint64_t kernel_ticks;

/** No interrupts to mask on the host: nothing to save. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing to restore. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* The key events the console's keyboard took since `clear`. */
#define MAX_EVENTS 256
static struct {
    const void *device;
    unsigned int keycode;
    int value;
} kd_events[MAX_EVENTS];
static int n_kd;

/* The events the event devices took since `clear`. */
static struct {
    struct evdev *dev;
    uint16_t type;
    uint16_t code;
    int32_t value;
} ev_events[MAX_EVENTS];
static int n_ev;

/* The raw view's bytes the console took since `clear`. */
static unsigned char raw_bytes[MAX_EVENTS];
static int n_raw;

/* The event devices handed out, by address, and the last repeat and LEDs
 * each was told; `unregistered` counts the ones taken away. */
static char evdev_slots[4];
static int evdev_used;
static int unregistered;
static int rep_delay[4];
static int rep_period[4];
static unsigned int leds[4];

/** The index of `dev` among `evdev_slots`. */
static int slot_of(struct evdev *dev)
{
    return (int)((char *)dev - evdev_slots);
}

/** Hand out the next of `evdev_slots`, or NULL when all four are out. */
struct evdev *evdev_register(const char *name, uint16_t bustype, uint16_t vendor, uint16_t product,
                             const uint8_t *keys)
{
    (void)name;
    (void)bustype;
    (void)vendor;
    (void)product;
    (void)keys;
    return evdev_used < 4 ? (struct evdev *)&evdev_slots[evdev_used++] : NULL;
}

/** Do nothing: no row reads what a device reports beyond its events. */
void evdev_describe(struct evdev *dev, const char *phys, unsigned int flags)
{
    (void)dev;
    (void)phys;
    (void)flags;
}

/** Count a device taken away. */
void evdev_unregister(struct evdev *dev)
{
    if (dev) {
        unregistered++;
    }
}

/** Record one event of `dev`. */
void evdev_report(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    if (dev && n_ev < MAX_EVENTS) {
        ev_events[n_ev].dev = dev;
        ev_events[n_ev].type = type;
        ev_events[n_ev].code = code;
        ev_events[n_ev].value = value;
        n_ev++;
    }
}

/** Record a key's packet as evdev.c makes it. */
void evdev_report_key(struct evdev *dev, uint32_t scan, uint16_t code, int down)
{
    evdev_report(dev, EV_MSC, MSC_SCAN, (int32_t)scan);
    if (code != 0) {
        evdev_report(dev, EV_KEY, code, down);
    }
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
}

/** Keep the repeat's times `dev` was told. */
void evdev_report_repeat(struct evdev *dev, int delay_ms, int period_ms)
{
    if (dev) {
        rep_delay[slot_of(dev)] = delay_ms;
        rep_period[slot_of(dev)] = period_ms;
    }
}

/** Keep the LEDs `dev` was told. */
void evdev_report_leds(struct evdev *dev, unsigned int mask)
{
    if (dev) {
        leds[slot_of(dev)] = mask;
    }
}

/** Record one key event of the console's keyboard. */
void kd_key_event(const void *device, unsigned int keycode, int value)
{
    if (n_kd < MAX_EVENTS) {
        kd_events[n_kd].device = device;
        kd_events[n_kd].keycode = keycode;
        kd_events[n_kd].value = value;
        n_kd++;
    }
}

/** Record one byte of the console's raw view. */
void kd_raw_code(uint8_t code)
{
    if (n_raw < MAX_EVENTS) {
        raw_bytes[n_raw++] = code;
    }
}

/** No suspend is in force. */
void suspend_wake(void)
{}

/** The screen does not blank here. */
void console_poke(void)
{}

/** Forget what the models took. */
static void clear(void)
{
    n_kd = 0;
    n_ev = 0;
    n_raw = 0;
}

/** The console's keyboard's events of `value` since `clear`. */
static int kd_count(int value)
{
    int i, n = 0;

    for (i = 0; i < n_kd; i++) {
        n += kd_events[i].value == value;
    }
    return n;
}

/* The tick count `ms` milliseconds past 10 s, where the repeat rows run. */
#define AT(ms) ((10000u + (ms)) / TICK_MS)

/** Run the tick once at each count from the one after now to `to`. */
static void tick_to(uint32_t to)
{
    while (kernel_ticks != to) {
        kernel_ticks++;
        input_tick();
    }
}

/** Run the tick once every 10 ms from now to `to`, so that a repeat comes
 * only at those ticks, as it would when ticks are late. */
static void tick_by_10ms(uint32_t to)
{
    while (kernel_ticks != to) {
        kernel_ticks += 10u / TICK_MS;
        input_tick();
    }
}

/** Press or release `keycode` on `kb`, with its set-1 code as MSC_SCAN. */
static void key(struct input_keyboard *kb, unsigned int keycode, int down)
{
    input_key_event(kb, set1_scancode((int)keycode), keycode, down);
}

/** Whether the raw view took exactly the bytes of `s` since `clear`. */
static int raw_is(const char *s)
{
    int i;

    for (i = 0; s[i]; i++) {
        if (i >= n_raw || raw_bytes[i] != (unsigned char)s[i]) {
            return 0;
        }
    }
    return i == n_raw;
}

/** Whether the last press the console's keyboard took was of `keycode`. */
static int last_press(unsigned int keycode)
{
    int i;

    for (i = n_kd - 1; i >= 0; i--) {
        if (kd_events[i].value == KD_KEY_PRESS) {
            return kd_events[i].keycode == keycode;
        }
    }
    return 0;
}

/* Linux's key codes the rows press. */
#define KEY_A  30
#define KEY_B  48
#define KEY_C  46
#define KEY_UP 103

/** Both Shift keys alone and together, on one keyboard and across two. */
static void test_state(struct input_keyboard *kb1, struct input_keyboard *kb2)
{
    clear();
    key(kb1, KEY_LEFTSHIFT, 1);
    key(kb1, KEY_RIGHTSHIFT, 1);
    CHECK("both shifts held", input_modifiers() == INPUT_MOD_SHIFT);
    key(kb1, KEY_LEFTSHIFT, 0);
    CHECK("the right shift stays held after the left's release",
          input_modifiers() == INPUT_MOD_SHIFTR);
    key(kb1, KEY_A, 1);
    key(kb1, KEY_A, 0);
    CHECK("a key pressed under the right shift reaches the console's keyboard",
          last_press(KEY_A) && raw_is("\x2a\x36\xaa\x1e\x9e"));
    key(kb1, KEY_RIGHTSHIFT, 0);
    clear();
    key(kb1, KEY_A, 1);
    key(kb1, KEY_A, 0);
    CHECK("no shift after both are released", input_modifiers() == 0 && raw_is("\x1e\x9e"));

    clear();
    key(kb1, KEY_LEFTSHIFT, 1);
    key(kb2, KEY_A, 1);
    key(kb2, KEY_A, 0);
    CHECK("one keyboard's shift is held for another's key",
          input_modifiers() == INPUT_MOD_SHIFTL && last_press(KEY_A) && kd_events[1].device == kb2);
    key(kb2, KEY_LEFTSHIFT, 1);
    key(kb1, KEY_LEFTSHIFT, 0);
    CHECK("the left shift stays held while another keyboard holds it",
          input_modifiers() == INPUT_MOD_SHIFTL);
    key(kb2, KEY_LEFTSHIFT, 0);
    CHECK("and is released by the last", input_modifiers() == 0);

    clear();
    key(kb1, KEY_RIGHTCTRL, 1);
    key(kb1, KEY_RIGHTALT, 1);
    key(kb1, KEY_LEFTMETA, 1);
    CHECK("right Ctrl, AltGr and Super are their own sides",
          input_modifiers() == (INPUT_MOD_CTRLR | INPUT_MOD_ALTGR | INPUT_MOD_METAL));
    key(kb1, KEY_C, 1);
    key(kb1, KEY_C, 0);
    CHECK("a key under right Ctrl and AltGr reaches the console's keyboard",
          last_press(KEY_C) && raw_is("\xe0\x1d\xe0\x38\xe0\x5b\x2e\xae"));
    key(kb1, KEY_RIGHTCTRL, 0);
    key(kb1, KEY_RIGHTALT, 0);
    key(kb1, KEY_LEFTMETA, 0);

    clear();
    key(kb1, KEY_A, 1);
    key(kb1, KEY_A, 1);
    key(kb1, KEY_A, 0);
    key(kb1, KEY_A, 0);
    CHECK("a press of a held key and a release of a free one are dropped",
          n_kd == 2 && kd_count(KD_KEY_PRESS) == 1 && kd_count(KD_KEY_RELEASE) == 1);
    CHECK("the console's keyboard is told which keyboard", kd_events[0].device == kb1);
}

/** The set-1 decoder of each keyboard, with its own prefix. */
static void test_set1(struct input_keyboard *kb1, struct input_keyboard *kb2)
{
    clear();
    input_set1_event(kb1, 0xE0);
    input_set1_event(kb2, 0x48);
    input_set1_event(kb1, 0x48);
    CHECK("a prefix is kept for its own keyboard",
          n_kd == 2 && kd_events[0].keycode == 0x48 && kd_events[0].device == kb2 &&
              kd_events[1].keycode == KEY_UP && kd_events[1].device == kb1);
    CHECK("an extended key goes to the raw view after E0", raw_is("\x48\xe0\x48"));
    input_set1_event(kb2, 0xC8);
    input_set1_event(kb1, 0xE0);
    input_set1_event(kb1, 0xC8);

    clear();
    input_set1_event(kb1, 0x1E);
    input_set1_event(kb1, 0x1E);
    input_set1_event(kb1, 0x1E);
    input_set1_event(kb1, 0x9E);
    CHECK("a PS/2 keyboard's own repeat is dropped",
          n_kd == 2 && kd_count(KD_KEY_PRESS) == 1 && raw_is("\x1e\x9e"));

    clear();
    input_set1_event(kb1, 0xE0);
    input_set1_event(kb1, 0x2A);
    input_set1_event(kb1, 0xE0);
    input_set1_event(kb1, 0xAA);
    CHECK("the shift a keyboard wraps a cursor key in is no key",
          n_kd == 0 && input_modifiers() == 0);

    clear();
    input_set1_event(kb1, 0xE1);
    input_set1_event(kb1, 0x1D);
    input_set1_event(kb1, 0x45);
    input_set1_event(kb1, 0xE1);
    input_set1_event(kb1, 0x9D);
    input_set1_event(kb1, 0xC5);
    CHECK("Pause is pressed and released, and holds no Ctrl",
          n_kd == 2 && kd_events[0].keycode == KEY_PAUSE && kd_events[0].value == 1 &&
              kd_events[1].value == 0 && input_modifiers() == 0);

    clear();
    input_set1_event(kb1, 0x1E);
    CHECK("a press reaches the event device as MSC_SCAN, the key and SYN",
          n_ev == 3 && ev_events[0].type == EV_MSC && ev_events[0].value == 0x1E &&
              ev_events[1].type == EV_KEY && ev_events[1].code == KEY_A &&
              ev_events[1].value == 1 && ev_events[2].type == EV_SYN);
    input_set1_event(kb1, 0x9E);
}

/** The raw view of an event. */
static void test_raw(void)
{
    uint8_t b[3];

    CHECK("A pressed is 1E", input_raw_bytes(KEY_A, KD_KEY_PRESS, b) == 1 && b[0] == 0x1E);
    CHECK("A repeated is 1E again", input_raw_bytes(KEY_A, KD_KEY_REPEAT, b) == 1 && b[0] == 0x1E);
    CHECK("A released is 9E", input_raw_bytes(KEY_A, KD_KEY_RELEASE, b) == 1 && b[0] == 0x9E);
    CHECK("Up released is E0 C8",
          input_raw_bytes(KEY_UP, KD_KEY_RELEASE, b) == 2 && b[0] == 0xE0 && b[1] == 0xC8);
    CHECK("Print Screen is E0 37",
          input_raw_bytes(99, KD_KEY_PRESS, b) == 2 && b[0] == 0xE0 && b[1] == 0x37);
    CHECK("Pause pressed is E1 1D 45", input_raw_bytes(KEY_PAUSE, KD_KEY_PRESS, b) == 3 &&
                                           b[0] == 0xE1 && b[1] == 0x1D && b[2] == 0x45);
    CHECK("Pause released is E1 9D C5", input_raw_bytes(KEY_PAUSE, KD_KEY_RELEASE, b) == 3 &&
                                            b[0] == 0xE1 && b[1] == 0x9D && b[2] == 0xC5);
    CHECK("a key with no set-1 code has no bytes", input_raw_bytes(0x2F0, KD_KEY_PRESS, b) == 0);
}

/** The repeat, by ticks and by counts. */
static void test_repeat(struct input_keyboard *kb1, struct input_keyboard *kb2)
{
    int delay, period, i;

    input_get_repeat(&delay, &period);
    CHECK("the repeat starts at Linux's 250 and 33 ms", delay == 250 && period == 33);

    kernel_ticks = AT(0);
    clear();
    key(kb1, KEY_A, 1);
    tick_to(AT(240));
    CHECK("no repeat before the delay", kd_count(KD_KEY_REPEAT) == 0);
    tick_to(AT(250));
    CHECK("the first repeat at 250 ms", kd_count(KD_KEY_REPEAT) == 1);
    tick_by_10ms(AT(1250));
    /* Due at 250 + 33k ms: 31 by 1250 ms, at ticks 10 ms apart. A deadline
     * taken from the tick that sent the last repeat would give one every
     * 40 ms, 26. */
    CHECK("the deadlines accumulate: 31 repeats in 1250 ms", kd_count(KD_KEY_REPEAT) == 31);
    CHECK("a repeat reaches the raw view as the make code again",
          n_raw == 32 && raw_bytes[31] == 0x1E);
    CHECK("a repeat is EV_KEY 2 and SYN, without MSC_SCAN",
          ev_events[n_ev - 2].type == EV_KEY && ev_events[n_ev - 2].value == 2 &&
              ev_events[n_ev - 1].type == EV_SYN && ev_events[n_ev - 3].type == EV_SYN);

    clear();
    key(kb1, KEY_LEFTSHIFT, 1);
    tick_to(AT(1350));
    CHECK("a modifier pressed keeps the key repeating",
          kd_count(KD_KEY_REPEAT) == 3 && kd_events[n_kd - 1].keycode == KEY_A);
    key(kb1, KEY_LEFTSHIFT, 0);
    key(kb1, KEY_CAPSLOCK, 1);
    tick_to(AT(1450));
    key(kb1, KEY_CAPSLOCK, 0);
    for (i = 0; i < n_kd; i++) {
        CHECK("neither a modifier nor a lock repeats",
              kd_events[i].value != KD_KEY_REPEAT || kd_events[i].keycode == KEY_A);
    }

    clear();
    key(kb2, KEY_B, 1);
    tick_to(AT(1600));
    CHECK("another key's press takes the repeat", kd_count(KD_KEY_REPEAT) == 0 && n_kd == 1);
    tick_to(AT(1700));
    CHECK("and repeats after its own delay", kd_count(KD_KEY_REPEAT) == 1 &&
                                                 kd_events[n_kd - 1].keycode == KEY_B &&
                                                 kd_events[n_kd - 1].device == kb2);
    key(kb1, KEY_A, 0);
    clear();
    tick_to(AT(1800));
    CHECK("a release of another key leaves the repeat", kd_count(KD_KEY_REPEAT) == 3);
    key(kb2, KEY_B, 0);
    clear();
    tick_to(AT(3000));
    CHECK("the release of the repeating key stops it", n_kd == 0);

    clear();
    key(kb1, KEY_A, 1);
    tick_to(AT(3300));
    input_cancel_repeat();
    tick_to(AT(4000));
    CHECK("input_cancel_repeat stops it", kd_count(KD_KEY_REPEAT) == 2);
    key(kb1, KEY_A, 0);

    clear();
    key(kb1, KEY_A, 1);
    tick_to(AT(4250));
    kernel_ticks = AT(5250);
    input_tick();
    CHECK("a late tick sends at most four", kd_count(KD_KEY_REPEAT) == 5);
    tick_to(AT(5280));
    CHECK("and counts again from then", kd_count(KD_KEY_REPEAT) == 5);
    tick_to(AT(5290));
    CHECK("a period on", kd_count(KD_KEY_REPEAT) == 6);
    key(kb1, KEY_A, 0);

    clear();
    input_set_repeat(500, -1);
    input_get_repeat(&delay, &period);
    CHECK("a negative time is left as it is", delay == 500 && period == 33);
    CHECK("each keyboard's event device is told the times",
          rep_delay[slot_of(kb1->dev)] == 500 && rep_period[slot_of(kb2->dev)] == 33);
    key(kb1, KEY_A, 1);
    tick_to(AT(5780));
    CHECK("the set delay holds", kd_count(KD_KEY_REPEAT) == 0);
    tick_to(AT(5790));
    CHECK("the first repeat comes at the set delay", kd_count(KD_KEY_REPEAT) == 1);
    key(kb1, KEY_A, 0);
    input_set_repeat(250, 0);
    clear();
    key(kb1, KEY_A, 1);
    tick_to(AT(8000));
    CHECK("a period of 0 is no repeat", kd_count(KD_KEY_REPEAT) == 0);
    key(kb1, KEY_A, 0);
    input_set_repeat(250, 33);
}

/** A keyboard that goes. */
static void test_detach(struct input_keyboard *kb1, struct input_keyboard *kb2)
{
    input_set_leds(LED_CAP);
    CHECK("the LEDs reach every keyboard",
          leds[slot_of(kb1->dev)] == 1u << LED_CAPSL && leds[slot_of(kb2->dev)] == 1u << LED_CAPSL);
    clear();
    key(kb2, KEY_RIGHTSHIFT, 1);
    key(kb2, KEY_A, 1);
    input_keyboard_detach(kb2);
    CHECK("a keyboard that goes releases what it held",
          n_kd == 4 && kd_count(KD_KEY_RELEASE) == 2 && input_modifiers() == 0);
    CHECK("its event device is taken away", unregistered == 1 && kb2->dev == NULL);
    clear();
    tick_to((uint32_t)kernel_ticks + 1000u / TICK_MS);
    CHECK("and its repeat stops", n_kd == 0);
    input_set_leds(LED_NUM);
    CHECK("the LEDs reach the keyboards attached", leds[slot_of(kb1->dev)] == 1u << LED_NUML);
    CHECK("and not one detached", leds[1] == 1u << LED_CAPSL);
    input_set_leds(0);
}

int main(void)
{
    static struct input_keyboard kb1, kb2;

    input_keyboard_attach(&kb1, "one", "one/input0", EVDEV_BUS_I8042, 1, 1);
    input_keyboard_attach(&kb2, "two", "two/input0", EVDEV_BUS_USB, 2, 2);
    CHECK("an attached keyboard's event device has the repeat",
          rep_delay[0] == 250 && rep_period[1] == 33);

    test_state(&kb1, &kb2);
    test_set1(&kb1, &kb2);
    test_raw();
    test_repeat(&kb1, &kb2);
    test_detach(&kb1, &kb2);

    if (fails == 0) {
        printf("input-test: pass\n");
    }
    return fails ? 1 : 0;
}
