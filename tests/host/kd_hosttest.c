/**
 * Host test of the console's keyboard requests, kd.c, and of evdev's repeat
 * requests, evdev.c, against models of the caller, the console's terminal
 * and the input layer.
 *
 * host-deps: rach2/sys/keymap.c tests/host/compat/kheap_host.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * Both files are included whole. A row sets the caller, a process of the
 * console's session, the holder of /fb, a privileged process or none of
 * them, and sends Linux's requests with Linux's structures, as kbd's tools
 * do: each request's answer, its errors for a bad pointer, a value out of
 * range and a caller without the right, and what it changed in the model of
 * the input layer (repeat, LEDs) and of the terminal (flushes, termios).
 *
 * The keymap rows use table 200 and the function strings 200 and up, which
 * no built-in keymap fills, and free or set them first. The two departures
 * from Linux are rows of their own: a code point's entry is read and
 * written in K_MEDIUMRAW, and reads as K_HOLE in K_XLATE alone.
 *
 * In K_MEDIUMRAW a key event gives the terminal Linux's one or three bytes,
 * in K_RAW a set-1 byte is given as it came, and the console translates in
 * K_XLATE and K_UNICODE alone: the built-in US keymap's keys reach the
 * terminal, or the debug shell when no session and no reader has the console,
 * with the LEDs following the locks. The state saved for an owner of the
 * screen is put back whole: mode, meta, locks, LEDs, repeat and termios.
 */
#include "kd.c"
#include "evdev.c"

extern int printf(const char *format, ...);

static int failures;

#define CHECK(condition, detail)                   \
    do {                                           \
        if (!(condition)) {                        \
            printf("kd-test: fail: %s\n", detail); \
            failures++;                            \
        }                                          \
    } while (0)

/* ---- the models ---- */

struct percpu cpu_table[NR_CPUS];
gfx_info_t gfx_current;
volatile uint32_t fb_owner_pid;
struct tty console_tty;

static process_t caller;

/* A byte of user memory a copy fails at, as an unmapped page would; NULL
 * for none. */
static const void *fault_at;

/* What the terminal was given and asked. */
static unsigned char input[64];
static int ninput, flushes, termios_sets;
static struct ktermios termios_set;

/* The input layer's repeat and LEDs, and what was asked of them. */
static int repeat_delay = 250, repeat_period = 33;
static int set_delay, set_period, repeat_sets, repeat_cancels;
static unsigned int leds_lit;
static int led_sets;

/* The holds of a spinlock not yet given back. */
static int locks_held;

/** Whether the `n` bytes at `p` hold the byte `fault_at`. */
static int faults(const void *p, size_t n)
{
    const char *c = (const char *)p;

    if (p == NULL) {
        return 1;
    }
    return fault_at != NULL && (const char *)fault_at >= c && (const char *)fault_at < c + n;
}

/** Every call here is an x86-64 program's. */
int in_compat_syscall(void)
{
    return 0;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if (faults(dst, n)) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (faults(src, n)) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    uint32_t i;

    if (n == 0) {
        return -EINVAL;
    }
    for (i = 0; i < n; i++) {
        if (faults(src + i, 1)) {
            return -EFAULT;
        }
        dst[i] = src[i];
        if (src[i] == '\0') {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;
}

uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    (void)lock;
    locks_held++;
    return 0;
}

void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    (void)lock;
    (void)flags;
    locks_held--;
}

void tty_input_char(struct tty *t, unsigned char c)
{
    CHECK(t == &console_tty, "key bytes go to the console");
    if (ninput < (int)sizeof(input)) {
        input[ninput++] = c;
    }
}

void tty_flush_input(struct tty *t)
{
    CHECK(t == &console_tty, "the console's input is flushed");
    flushes++;
}

void tty_set_termios(struct tty *t, const struct ktermios *kt, int flush_input)
{
    CHECK(t == &console_tty && !flush_input, "a restore sets the console's termios");
    termios_set = *kt;
    termios_sets++;
}

void input_set_repeat(int delay_ms, int period_ms)
{
    set_delay = delay_ms;
    set_period = period_ms;
    repeat_sets++;
    if (delay_ms >= 0) {
        repeat_delay = delay_ms;
    }
    if (period_ms >= 0) {
        repeat_period = period_ms;
    }
}

void input_get_repeat(int *delay_ms, int *period_ms)
{
    *delay_ms = repeat_delay;
    *period_ms = repeat_period;
}

/** No row writes to an event device. */
void input_written(uint16_t type, uint16_t code, int32_t value)
{
    (void)type;
    (void)code;
    (void)value;
}

void input_cancel_repeat(void)
{
    repeat_cancels++;
}

void input_set_leds(unsigned int mask)
{
    leds_lit = mask;
    led_sets++;
}

void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return -EINTR;
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* The console's reader, the debug shell and the screen's scrollback, as the
 * translating rows set and read them. */
static int reader_parked, shell_on, interrupts, scrolls_up, scrolls_down, scroll_ends;
static char shell_line[32];
static int nshell;
static unsigned int key_modes;

int wq_active(const wait_queue_t *wq)
{
    CHECK(wq == &console_tty.input_wq, "the console's readers are asked");
    return reader_parked;
}

int shell_reads_input(void)
{
    return shell_on;
}

void shell_input_char(char c)
{
    if (nshell < (int)sizeof(shell_line)) {
        shell_line[nshell++] = c;
    }
}

void shell_interrupt(void)
{
    interrupts++;
}

void k_scrollback_page_up(void)
{
    scrolls_up++;
}

void k_scrollback_page_down(void)
{
    scrolls_down++;
}

void k_scrollback_home(void)
{}

void k_scrollback_end(void)
{
    scroll_ends++;
}

bool k_scrollback_is_active(void)
{
    return 0;
}

unsigned int kio_key_modes(void)
{
    return key_modes;
}

void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 0;
    *nanoseconds = 0;
}

void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 0;
    *nanoseconds = 0;
}

/** Whether the `n` bytes at `a` and `b` differ. */
static int differ(const void *a, const void *b, unsigned int n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    unsigned int i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return 1;
        }
    }
    return 0;
}

/* ---- the callers ---- */

#define AS_STRANGER 0 /* none of the below */
#define AS_SESSION  1 /* a process of the console's session */
#define AS_FBOWNER  2 /* the holder of /fb */
#define AS_ROOT     3 /* a privileged process outside the session */

/** Make the caller `who`, AS_*, and clear what the models recorded. */
static void be(int who)
{
    k_memset(&caller, 0, sizeof(caller));
    caller.pid = caller.tgid = 40;
    caller.sid = 40;
    caller.euid = 1000;
    console_tty.sid = 7;
    fb_owner_pid = 0;
    if (who == AS_SESSION) {
        caller.sid = 7;
    } else if (who == AS_FBOWNER) {
        fb_owner_pid = 40;
    } else if (who == AS_ROOT) {
        caller.euid = 0;
    }
    current_proc = &caller;
    ninput = flushes = termios_sets = repeat_sets = repeat_cancels = led_sets = 0;
    reader_parked = shell_on = interrupts = scrolls_up = scrolls_down = scroll_ends = nshell = 0;
    key_modes = 0;
    fault_at = NULL;
}

/** Send KDSKBENT for `table`, `index` and `value`. */
static int32_t set_entry(unsigned int table, unsigned int index, unsigned int value)
{
    struct kbentry e;

    e.kb_table = (unsigned char)table;
    e.kb_index = (unsigned char)index;
    e.kb_value = (unsigned short)value;
    return kd_ioctl(KDSKBENT, (uintptr_t)&e);
}

/** KDGKBENT's entry for `table` and `index`, or 0xDEAD when it fails. */
static unsigned int get_entry(unsigned int table, unsigned int index)
{
    struct kbentry e;

    e.kb_table = (unsigned char)table;
    e.kb_index = (unsigned char)index;
    e.kb_value = 0;
    return kd_ioctl(KDGKBENT, (uintptr_t)&e) == 0 ? e.kb_value : 0xDEADu;
}

/** Put the keyboard in `mode` as root. */
static void mode(int m)
{
    be(AS_ROOT);
    CHECK(kd_ioctl(KDSKBMODE, (uintptr_t)m) == 0, "root sets the keyboard's mode");
}

/* ---- the rows ---- */

/** KDGKBTYPE is KB_101 in a byte, and KDGETMODE follows the screen. */
static void type_and_screen(void)
{
    unsigned char b[2] = {0, 0x5A};
    int m = -1;

    be(AS_STRANGER);
    CHECK(kd_ioctl(KDGKBTYPE, (uintptr_t)b) == 0 && b[0] == KB_101 && b[1] == 0x5A,
          "KDGKBTYPE writes KB_101 in one byte");
    CHECK(kd_ioctl(KDGKBTYPE, 0) == -EFAULT, "KDGKBTYPE to a bad pointer is -EFAULT");
    gfx_current.mode = GFX_MODE_NONE;
    CHECK(kd_ioctl(KDGETMODE, (uintptr_t)&m) == 0 && m == KD_TEXT, "KDGETMODE in text");
    gfx_current.mode = GFX_MODE_VGA_13H;
    CHECK(kd_ioctl(KDGETMODE, (uintptr_t)&m) == 0 && m == KD_GRAPHICS, "KDGETMODE while /fb draws");
    gfx_current.mode = GFX_MODE_NONE;
    CHECK(kd_ioctl(KDSETMODE, KD_TEXT) == -ENOTTY, "KDSETMODE is not the keyboard's");
    CHECK(kd_ioctl(0x5401, 0) == -ENOTTY, "a terminal request goes on");
}

/** KDGKBMODE and KDSKBMODE: K_UNICODE first, set by the session, /fb's
 * holder and root alone, each set flushing input and stopping repeat. */
static void modes(void)
{
    int m = -1;

    be(AS_STRANGER);
    CHECK(kd_ioctl(KDGKBMODE, (uintptr_t)&m) == 0 && m == K_UNICODE, "K_UNICODE at boot");
    CHECK(kd_ioctl(KDSKBMODE, K_RAW) == -EPERM, "a stranger may not set the mode");
    CHECK(kd_mode == K_UNICODE && flushes == 0, "a refused mode changes nothing");
    be(AS_SESSION);
    CHECK(kd_ioctl(KDSKBMODE, K_MEDIUMRAW) == 0, "the console's session sets the mode");
    CHECK(flushes == 1 && repeat_cancels == 1, "a mode set flushes input and stops repeat");
    CHECK(kd_ioctl(KDGKBMODE, (uintptr_t)&m) == 0 && m == K_MEDIUMRAW, "KDGKBMODE reads it");
    CHECK(kd_ioctl(KDSKBMODE, 5) == -EINVAL && kd_mode == K_MEDIUMRAW,
          "a mode out of range is -EINVAL");
    be(AS_FBOWNER);
    CHECK(kd_ioctl(KDSKBMODE, K_RAW) == 0 && kd_mode == K_RAW, "/fb's holder sets the mode");
    be(AS_ROOT);
    CHECK(kd_ioctl(KDSKBMODE, K_OFF) == 0 && kd_mode == K_OFF, "root sets the mode");
    CHECK(kd_ioctl(KDGKBMODE, 0) == -EFAULT, "KDGKBMODE to a bad pointer is -EFAULT");
    mode(K_UNICODE);
}

/** KDGKBMETA and KDSKBMETA: ESC before an Alt key at boot. */
static void meta(void)
{
    int m = -1;

    be(AS_STRANGER);
    CHECK(kd_ioctl(KDGKBMETA, (uintptr_t)&m) == 0 && m == K_ESCPREFIX, "K_ESCPREFIX at boot");
    CHECK(kd_ioctl(KDSKBMETA, K_METABIT) == -EPERM, "a stranger may not set meta");
    be(AS_SESSION);
    CHECK(kd_ioctl(KDSKBMETA, K_METABIT) == 0, "the session sets meta");
    CHECK(kd_ioctl(KDGKBMETA, (uintptr_t)&m) == 0 && m == K_METABIT, "KDGKBMETA reads it");
    CHECK(kd_ioctl(KDSKBMETA, 2) == -EINVAL, "a meta setting out of range is -EINVAL");
    CHECK(kd_ioctl(KDSKBMETA, K_ESCPREFIX) == 0, "the session sets K_ESCPREFIX back");
}

/** KDGKBLED, KDSKBLED, KDGETLED and KDSETLED: the LEDs follow the lock
 * flags until KDSETLED lights its own, and again after a higher bit. */
static void leds(void)
{
    unsigned char b = 0xEE;

    be(AS_STRANGER);
    CHECK(kd_ioctl(KDGETLED, (uintptr_t)&b) == 0 && b == 0, "no LED at boot");
    CHECK(kd_ioctl(KDSKBLED, K_CAPSLOCK) == -EPERM, "a stranger may not set the locks");
    CHECK(kd_ioctl(KDSETLED, LED_NUM) == -EPERM, "a stranger may not light an LED");
    be(AS_SESSION);
    CHECK(kd_ioctl(KDSKBLED, 0x25) == 0, "the session sets the locks and their defaults");
    CHECK(led_sets == 1 && leds_lit == (LED_CAP | LED_SCR), "the LEDs follow the locks");
    CHECK(kd_ioctl(KDGKBLED, (uintptr_t)&b) == 0 && b == 0x25, "KDGKBLED reads both");
    CHECK(kd_ioctl(KDGETLED, (uintptr_t)&b) == 0 && b == (LED_CAP | LED_SCR),
          "KDGETLED reads the LEDs lit");
    CHECK(kd_ioctl(KDSKBLED, 0x08) == -EINVAL && kd_ioctl(KDSKBLED, 0x80) == -EINVAL,
          "a lock flag out of range is -EINVAL");
    CHECK(kd_ioctl(KDSETLED, LED_NUM) == 0 && leds_lit == LED_NUM, "KDSETLED lights its own");
    CHECK(kd_ioctl(KDGETLED, (uintptr_t)&b) == 0 && b == LED_NUM, "KDGETLED reads them");
    CHECK(kd_ioctl(KDSKBLED, K_NUMLOCK) == 0 && leds_lit == LED_NUM && kd_leds_lit() == LED_NUM,
          "a lock change leaves KDSETLED's LEDs");
    CHECK(kd_ioctl(KDSETLED, 0xFF) == 0 && leds_lit == LED_NUM,
          "a higher bit follows the locks again");
    CHECK(kd_ioctl(KDSKBLED, 0) == 0 && leds_lit == 0, "and they follow a change");
    be(AS_FBOWNER);
    CHECK(kd_ioctl(KDSETLED, LED_CAP) == 0 && leds_lit == LED_CAP, "/fb's holder lights one");
    CHECK(kd_ioctl(KDSETLED, 8) == 0 && leds_lit == 0, "and gives them back to the locks");
}

/** KDGKBENT and KDSKBENT on table 200: allocated on demand, with Linux's
 * bounds, entry 0 and freeing, and root's alone to set. */
static void entries(void)
{
    unsigned int was;

    be(AS_ROOT);
    CHECK(set_entry(200, 0, KM_NOSUCHMAP) == 0, "K_NOSUCHMAP at key 0 frees a table");
    CHECK(get_entry(200, 0) == KM_NOSUCHMAP, "an absent table's key 0 is K_NOSUCHMAP");
    CHECK(get_entry(200, 30) == KM_HOLE, "an absent table's other keys are K_HOLE");
    CHECK(set_entry(200, 30, KM_K(KM_T_LATIN, 'a')) == 0, "root sets an entry");
    CHECK(get_entry(200, 30) == KM_K(KM_T_LATIN, 'a'), "KDGKBENT reads it");
    CHECK(get_entry(200, 31) == KM_HOLE, "a new table's other keys are K_HOLE");
    CHECK(get_entry(200, 0) == KD_ALLOCATED, "a new table's key 0 is K_ALLOCATED");
    CHECK(set_entry(200, 0, KM_K(KM_T_LATIN, 'b')) == 0 && get_entry(200, 0) == KD_ALLOCATED,
          "a value at key 0 is only checked");
    CHECK(set_entry(200, 0, KM_K(KM_T_CUR, 4)) == -EINVAL, "and a bad one refused");
    CHECK(set_entry(200, 31, KM_K(KM_T_CUR, 4)) == -EINVAL, "a cursor key past 3");
    CHECK(set_entry(200, 31, KM_K(KM_T_SPEC, 20)) == -EINVAL, "an action past 19");
    CHECK(set_entry(200, 31, KM_K(KM_T_DEAD, 27)) == -EINVAL, "an accent past 26");
    CHECK(set_entry(200, 31, KM_K(KM_T_LOCK, 9)) == -EINVAL, "a lock past 8");
    CHECK(set_entry(200, 31, KM_K(KM_T_BRL, 11)) == -EINVAL, "a Braille key past 10");
    CHECK(get_entry(200, 31) == KM_HOLE, "a refused value leaves the entry");
    CHECK(set_entry(200, 31, KM_BARENUMLOCK) == 0 && set_entry(200, 32, KM_K(KM_T_SHIFT, 8)) == 0,
          "the last of each range is taken");
    be(AS_SESSION);
    CHECK(set_entry(200, 30, KM_K(KM_T_LATIN, 'z')) == -EPERM, "the session may not set one");
    CHECK(get_entry(200, 30) == KM_K(KM_T_LATIN, 'a'), "and anyone reads one");
    CHECK(kd_ioctl(KDGKBENT, 0) == -EFAULT, "KDGKBENT on a bad pointer");
    be(AS_ROOT);
    CHECK(kd_ioctl(KDSKBENT, 0) == -EFAULT, "KDSKBENT on a bad pointer");
    was = get_entry(0, 30);
    CHECK(set_entry(0, 30, KM_K(KM_T_LATIN, 'a')) == 0 && get_entry(0, 30) == KM_K(KM_T_LATIN, 'a'),
          "root sets an entry of table 0");
    CHECK(set_entry(0, 30, was) == 0, "and puts it back");
    CHECK(set_entry(0, 0, KM_NOSUCHMAP) == 0 && get_entry(0, 0) != KM_NOSUCHMAP,
          "table 0 is never freed");
    CHECK(set_entry(200, 0, KM_NOSUCHMAP) == 0 && get_entry(200, 30) == KM_HOLE &&
              get_entry(200, 0) == KM_NOSUCHMAP,
          "a freed table is absent");
    CHECK(locks_held == 0, "every lock is given back");
}

/** A code point's entry is taken and read in K_MEDIUMRAW, and reads as
 * K_HOLE in K_XLATE alone. */
static void unicode_entries(void)
{
    unsigned int a = KM_U(0x0430u);

    mode(K_MEDIUMRAW);
    CHECK(set_entry(200, 30, a) == 0, "K_MEDIUMRAW takes a code point's entry");
    CHECK(get_entry(200, 30) == a, "K_MEDIUMRAW reads it");
    mode(K_RAW);
    CHECK(get_entry(200, 30) == a, "K_RAW reads it");
    mode(K_XLATE);
    CHECK(get_entry(200, 30) == KM_HOLE, "K_XLATE reads it as K_HOLE");
    CHECK(set_entry(200, 31, KM_U(0x0431u)) == 0, "K_XLATE takes one too");
    mode(K_UNICODE);
    CHECK(get_entry(200, 30) == a && get_entry(200, 31) == KM_U(0x0431u), "K_UNICODE reads both");
    CHECK(set_entry(200, 0, KM_NOSUCHMAP) == 0, "the table is freed");
}

/** KDGKBSENT and KDSKBSENT: a string set and read back, the 512-byte bound,
 * and root's alone to set. */
static void strings(void)
{
    static struct kbsentry s;
    unsigned int i;

    be(AS_ROOT);
    s.kb_func = 200;
    k_memcpy(s.kb_string, "\033[15~", 6);
    CHECK(kd_ioctl(KDSKBSENT, (uintptr_t)&s) == 0, "root sets a string");
    k_memset(s.kb_string, 'x', sizeof(s.kb_string));
    CHECK(kd_ioctl(KDGKBSENT, (uintptr_t)&s) == 0 && k_strlen((char *)s.kb_string) == 5 &&
              !differ(s.kb_string, "\033[15~", 6) && s.kb_string[6] == 'x',
          "KDGKBSENT copies the string and its null alone");
    s.kb_func = 201;
    k_memset(s.kb_string, 'y', sizeof(s.kb_string));
    CHECK(kd_ioctl(KDSKBSENT, (uintptr_t)&s) == -EINVAL, "a string of 512 bytes is -EINVAL");
    s.kb_string[511] = '\0';
    CHECK(kd_ioctl(KDSKBSENT, (uintptr_t)&s) == 0, "a string of 511 is taken");
    k_memset(s.kb_string, 0, sizeof(s.kb_string));
    CHECK(kd_ioctl(KDGKBSENT, (uintptr_t)&s) == 0 && k_strlen((char *)s.kb_string) == 511,
          "and read back whole");
    for (i = 0; i < 511 && s.kb_string[i] == 'y'; i++) {
    }
    CHECK(i == 511, "with every byte");
    s.kb_func = 200;
    s.kb_string[0] = '\0';
    CHECK(kd_ioctl(KDSKBSENT, (uintptr_t)&s) == 0, "root empties a string");
    s.kb_string[0] = 'q';
    CHECK(kd_ioctl(KDGKBSENT, (uintptr_t)&s) == 0 && s.kb_string[0] == '\0',
          "an empty string reads as its null");
    s.kb_func = 202;
    s.kb_string[0] = 'q';
    CHECK(kd_ioctl(KDGKBSENT, (uintptr_t)&s) == 0 && s.kb_string[0] == '\0',
          "a string never set reads as its null");
    be(AS_SESSION);
    CHECK(kd_ioctl(KDSKBSENT, (uintptr_t)&s) == -EPERM, "the session may not set one");
    CHECK(kd_ioctl(KDGKBSENT, 0) == -EFAULT, "KDGKBSENT on a bad pointer");
    be(AS_ROOT);
    fault_at = &s.kb_string[3];
    s.kb_func = 200;
    k_memcpy(s.kb_string, "abcdef", 7);
    CHECK(kd_ioctl(KDSKBSENT, (uintptr_t)&s) == -EFAULT, "a string it cannot read");
    fault_at = NULL;
    CHECK(kd_ioctl(KDGKBSENT, (uintptr_t)&s) == 0 && s.kb_string[0] == '\0',
          "leaves the string as it was");
}

/** KDGKBDIACRUC, KDSKBDIACRUC, KDGKBDIACR and KDSKBDIACR: the table in
 * code points and in 8 bits, its bound, and root's alone to set. */
static void accents(void)
{
    static struct kbdiacrsuc uc;
    static struct kbdiacrs b;

    be(AS_ROOT);
    uc.kb_cnt = 3;
    uc.kbdiacruc[0].diacr = '\'';
    uc.kbdiacruc[0].base = 'e';
    uc.kbdiacruc[0].result = 0xE9;
    uc.kbdiacruc[1].diacr = 0x0301;
    uc.kbdiacruc[1].base = 0x0430;
    uc.kbdiacruc[1].result = 0x0430;
    uc.kbdiacruc[2].diacr = '^';
    uc.kbdiacruc[2].base = 'o';
    uc.kbdiacruc[2].result = 0xF4;
    CHECK(kd_ioctl(KDSKBDIACRUC, (uintptr_t)&uc) == 0, "root sets the accents");
    k_memset(&uc, 0, sizeof(uc));
    CHECK(kd_ioctl(KDGKBDIACRUC, (uintptr_t)&uc) == 0 && uc.kb_cnt == 3 &&
              uc.kbdiacruc[1].diacr == 0x0301 && uc.kbdiacruc[2].result == 0xF4,
          "KDGKBDIACRUC reads them in code points");
    k_memset(&b, 0, sizeof(b));
    CHECK(kd_ioctl(KDGKBDIACR, (uintptr_t)&b) == 0 && b.kb_cnt == 3 &&
              b.kbdiacr[0].result == 0xE9 && b.kbdiacr[1].diacr == 0xFF &&
              b.kbdiacr[1].base == 0xFF && b.kbdiacr[2].base == 'o',
          "KDGKBDIACR reads them in 8 bits, 0xFF for a code point past 255");
    b.kb_cnt = 1;
    b.kbdiacr[0].diacr = '"';
    b.kbdiacr[0].base = 'u';
    b.kbdiacr[0].result = 0xFC;
    CHECK(kd_ioctl(KDSKBDIACR, (uintptr_t)&b) == 0, "root sets them in 8 bits");
    CHECK(kd_ioctl(KDGKBDIACRUC, (uintptr_t)&uc) == 0 && uc.kb_cnt == 1 &&
              uc.kbdiacruc[0].diacr == '"' && uc.kbdiacruc[0].result == 0xFC,
          "as Latin-1 code points");
    uc.kb_cnt = 256;
    CHECK(kd_ioctl(KDSKBDIACRUC, (uintptr_t)&uc) == -EINVAL, "256 accents are -EINVAL");
    b.kb_cnt = 256;
    CHECK(kd_ioctl(KDSKBDIACR, (uintptr_t)&b) == -EINVAL, "in 8 bits too");
    uc.kb_cnt = 255;
    fault_at = &uc.kbdiacruc[200];
    CHECK(kd_ioctl(KDSKBDIACRUC, (uintptr_t)&uc) == -EFAULT, "entries it cannot read");
    fault_at = NULL;
    CHECK(kd_ioctl(KDGKBDIACRUC, (uintptr_t)&uc) == 0 && uc.kb_cnt == 1,
          "leave the table as it was");
    uc.kb_cnt = 255;
    CHECK(kd_ioctl(KDSKBDIACRUC, (uintptr_t)&uc) == 0 && ndiacr == 255, "255 are taken");
    uc.kb_cnt = 0;
    CHECK(kd_ioctl(KDSKBDIACRUC, (uintptr_t)&uc) == 0 && ndiacr == 0, "and none");
    CHECK(kd_ioctl(KDGKBDIACR, (uintptr_t)&b) == 0 && b.kb_cnt == 0, "an empty table reads so");
    be(AS_SESSION);
    CHECK(kd_ioctl(KDSKBDIACRUC, (uintptr_t)&uc) == -EPERM &&
              kd_ioctl(KDSKBDIACR, (uintptr_t)&b) == -EPERM,
          "the session may not set them");
    CHECK(kd_ioctl(KDGKBDIACRUC, 0) == -EFAULT, "KDGKBDIACRUC on a bad pointer");
    CHECK(locks_held == 0, "every lock is given back");
}

/** KDKBDREP: root's alone; a time of 0 or less left; the times in force
 * returned. */
static void repeat(void)
{
    struct kbd_repeat r;

    be(AS_SESSION);
    r.delay = 500;
    r.period = 0;
    CHECK(kd_ioctl(KDKBDREP, (uintptr_t)&r) == -EPERM && repeat_sets == 0,
          "the session may not set the repeat");
    be(AS_ROOT);
    repeat_delay = 250;
    repeat_period = 33;
    CHECK(kd_ioctl(KDKBDREP, (uintptr_t)&r) == 0, "root sets the repeat");
    CHECK(set_delay == 500 && set_period == -1, "a period of 0 is left as it is");
    CHECK(r.delay == 500 && r.period == 33, "and the times in force come back");
    r.delay = -3;
    r.period = 50;
    CHECK(kd_ioctl(KDKBDREP, (uintptr_t)&r) == 0 && set_delay == -1 && set_period == 50 &&
              r.delay == 500 && r.period == 50,
          "a negative delay is left as it is");
    CHECK(kd_ioctl(KDKBDREP, 0) == -EFAULT, "KDKBDREP on a bad pointer");
}

/** The key bytes of each mode: K_MEDIUMRAW's codes, K_RAW's set-1 bytes,
 * and nothing in the translating modes, which `kd_translates` names. */
static void raw_modes(void)
{
    static const unsigned char tall[] = {0x00, 0x83, 0xD0, 0x80, 0x83, 0xD0};

    mode(K_MEDIUMRAW);
    CHECK(!kd_translates(), "K_MEDIUMRAW does not translate");
    ninput = 0;
    kd_key_event(&caller, 30, KD_KEY_PRESS);
    kd_key_event(&caller, 30, KD_KEY_REPEAT);
    kd_key_event(&caller, 30, KD_KEY_RELEASE);
    CHECK(ninput == 3 && input[0] == 30 && input[1] == 30 && input[2] == (30 | 0x80),
          "a code below 128 is one byte, a repeat a press");
    ninput = 0;
    kd_key_event(&caller, 0x1D0, KD_KEY_PRESS);
    kd_key_event(&caller, 0x1D0, KD_KEY_RELEASE);
    CHECK(ninput == 6 && !differ(input, tall, 6), "a higher code is three bytes");
    ninput = 0;
    kd_raw_code(0x1E);
    CHECK(ninput == 0, "K_MEDIUMRAW sends no set-1 byte");
    mode(K_RAW);
    CHECK(!kd_translates(), "K_RAW does not translate");
    ninput = 0;
    kd_raw_code(0xE0);
    kd_raw_code(0x48);
    kd_key_event(&caller, 103, KD_KEY_PRESS);
    CHECK(ninput == 2 && input[0] == 0xE0 && input[1] == 0x48, "K_RAW sends the set-1 bytes alone");
    mode(K_OFF);
    CHECK(!kd_translates(), "K_OFF does not translate");
    ninput = 0;
    kd_raw_code(0x1E);
    kd_key_event(&caller, 30, KD_KEY_PRESS);
    CHECK(ninput == 0, "K_OFF sends nothing");
    mode(K_XLATE);
    CHECK(kd_translates(), "K_XLATE translates");
    mode(K_UNICODE);
    CHECK(kd_translates(), "K_UNICODE translates");
    ninput = 0;
    kd_raw_code(0x1E);
    kd_key_event(&caller, 30, KD_KEY_PRESS);
    kd_key_event(&caller, 30, KD_KEY_RELEASE);
    CHECK(ninput == 1 && input[0] == 'a', "a translating mode takes no raw byte, and translates");
}

/** Press and release the key `code`. */
static void tap(unsigned int code)
{
    kd_key_event(&caller, code, KD_KEY_PRESS);
    kd_key_event(&caller, code, KD_KEY_RELEASE);
}

/** Whether the terminal took exactly the `n` bytes of `s`, and forget them. */
static int typed(const char *s, int n)
{
    int same = ninput == n && !differ(input, s, (unsigned int)n);

    ninput = 0;
    return same;
}

/**
 * The translating modes: the built-in US keymap through the translator to
 * the terminal, with the cursor mode the console gives, the locks lighting
 * the LEDs, the scrollback's keys spent, K_XLATE's 8 bits and K_METABIT's
 * top bit; and with no session and no reader, the debug shell's keys and
 * its Ctrl+C.
 */
static void translating(void)
{
    unsigned int was;

    mode(K_UNICODE);
    be(AS_ROOT);
    tap(30);
    CHECK(typed("a", 1), "A types a");
    kd_key_event(&caller, 30, KD_KEY_PRESS);
    kd_key_event(&caller, 30, KD_KEY_REPEAT);
    kd_key_event(&caller, 30, KD_KEY_RELEASE);
    CHECK(typed("aa", 2), "a repeat types the key again");
    tap(28);
    CHECK(typed("\r", 1), "Enter sends CR");
    tap(14);
    CHECK(typed("\177", 1), "Backspace sends DEL");
    kd_key_event(&caller, 42, KD_KEY_PRESS);
    tap(30);
    kd_key_event(&caller, 42, KD_KEY_RELEASE);
    CHECK(typed("A", 1), "Shift with A types A");
    kd_key_event(&caller, 29, KD_KEY_PRESS);
    tap(46);
    kd_key_event(&caller, 29, KD_KEY_RELEASE);
    CHECK(typed("\003", 1) && interrupts == 0, "Ctrl+C in a session is ETX for the terminal");
    tap(103);
    CHECK(typed("\033[A", 3), "Up in the normal cursor mode");
    key_modes = KM_MODE_CURSOR;
    tap(103);
    CHECK(typed("\033OA", 3), "Up in DECCKM");
    key_modes = 0;

    tap(58);
    CHECK(ninput == 0 && led_sets == 1 && leds_lit == LED_CAP, "Caps Lock lights its LED alone");
    tap(30);
    CHECK(typed("A", 1), "Caps Lock shifts a letter");
    tap(58);
    CHECK(led_sets == 2 && leds_lit == 0, "Caps Lock again puts the LED out");
    be(AS_ROOT);
    CHECK(kd_ioctl(KDSETLED, LED_SCR) == 0, "root lights the LEDs its own way");
    led_sets = 0;
    tap(58);
    CHECK(led_sets == 0, "a lock leaves LEDs that KDSETLED lit");
    tap(58);
    CHECK(kd_ioctl(KDSETLED, 0xFF) == 0, "the LEDs follow the flags again");

    kd_key_event(&caller, 42, KD_KEY_PRESS);
    tap(104);
    tap(109);
    kd_key_event(&caller, 42, KD_KEY_RELEASE);
    CHECK(ninput == 0 && scrolls_up == 1 && scrolls_down == 1,
          "Shift with Page Up and Page Down scrolls the screen and sends nothing");
    tap(104);
    CHECK(typed("\033[5~", 4), "Page Up alone is the terminal's");

    was = get_entry(0, 30);
    CHECK(set_entry(0, 30, KM_U(0x430)) == 0, "A's plain entry is set to a Cyrillic letter");
    tap(30);
    CHECK(typed("\320\260", 2), "K_UNICODE sends it in UTF-8");
    mode(K_XLATE);
    tap(30);
    CHECK(ninput == 0, "K_XLATE sends nothing for a letter past Latin-1");
    CHECK(set_entry(0, 30, KM_U(0xE9)) == 0, "and a Latin-1 one");
    tap(30);
    CHECK(typed("\351", 1), "K_XLATE sends a Latin-1 letter as its byte");
    mode(K_UNICODE);
    tap(30);
    CHECK(typed("\303\251", 2), "which K_UNICODE sends in UTF-8");
    CHECK(set_entry(0, 30, was) == 0 && get_entry(0, 30) == was, "A's entry is put back");

    kd_key_event(&caller, 56, KD_KEY_PRESS);
    tap(30);
    CHECK(typed("\033a", 2), "Alt puts ESC before a character");
    CHECK(kd_ioctl(KDSKBMETA, K_METABIT) == 0, "root sets K_METABIT");
    tap(30);
    CHECK(typed("\341", 1), "K_METABIT sets the character's top bit");
    kd_key_event(&caller, 56, KD_KEY_RELEASE);
    CHECK(kd_ioctl(KDSKBMETA, K_ESCPREFIX) == 0, "root puts ESC back");

    console_tty.sid = 0;
    reader_parked = 1;
    tap(30);
    CHECK(typed("a", 1), "a parked reader takes the keys with no session");
    reader_parked = 0;
    shell_on = 1;
    tap(30);
    tap(14);
    tap(103);
    tap(28);
    CHECK(ninput == 0 && nshell == 3 && shell_line[0] == 'a' && shell_line[1] == '\b' &&
              shell_line[2] == '\r',
          "the debug shell takes text, DEL as Backspace and CR, and no sequence");
    tap(104);
    CHECK(scrolls_up == 1 && nshell == 3, "Page Up scrolls the debug shell's screen");
    kd_key_event(&caller, 97, KD_KEY_PRESS);
    tap(46);
    shell_on = 0;
    tap(46);
    kd_key_event(&caller, 97, KD_KEY_RELEASE);
    CHECK(interrupts == 2 && nshell == 3, "Ctrl+C is the debug shell's interrupt, reading or not");
    tap(30);
    CHECK(ninput == 0 && nshell == 3, "a key nobody reads is dropped");
    be(AS_ROOT);
    CHECK(locks_held == 0, "the translating rows give every lock back");
}

/** `kd_state_save` and `kd_state_restore` keep the mode, meta, locks, LEDs,
 * repeat and termios across another owner's changes. */
static void save_restore(void)
{
    struct kd_state st;
    struct ktermios tio;

    be(AS_ROOT);
    CHECK(kd_ioctl(KDSKBMODE, K_MEDIUMRAW) == 0 && kd_ioctl(KDSKBMETA, K_METABIT) == 0 &&
              kd_ioctl(KDSKBLED, 0x13) == 0 && kd_ioctl(KDSETLED, LED_CAP) == 0,
          "the first owner sets its keyboard");
    kd_kb.locks = 1u << KM_G_SHIFTL;
    repeat_delay = 400;
    repeat_period = 20;
    console_tty.ldisc.tio.c_lflag = 0x1234;
    kd_state_save(&st);
    CHECK(st.kbmode == K_MEDIUMRAW && st.meta == K_METABIT && st.ledflags == 0x13 &&
              st.leds == LED_CAP && st.locks == (1u << KM_G_SHIFTL) && st.repeat_delay == 400 &&
              st.repeat_period == 20 && st.termios.c_lflag == 0x1234,
          "the state is saved whole");
    tio = console_tty.ldisc.tio;
    CHECK(kd_ioctl(KDSKBMODE, K_UNICODE) == 0 && kd_ioctl(KDSKBMETA, K_ESCPREFIX) == 0 &&
              kd_ioctl(KDSKBLED, 0) == 0 && kd_ioctl(KDSETLED, 0xFF) == 0,
          "the next owner changes it");
    kd_kb.locks = 0;
    repeat_delay = 250;
    repeat_period = 33;
    console_tty.ldisc.tio.c_lflag = 0;
    be(AS_ROOT);
    kd_state_restore(&st);
    CHECK(kd_mode == K_MEDIUMRAW && kd_meta == K_METABIT && kd_kb.ledflags == 3 &&
              kd_default_flags == 1 && kd_leds == LED_CAP && kd_kb.locks == (1u << KM_G_SHIFTL),
          "the keyboard is put back");
    CHECK(repeat_delay == 400 && repeat_period == 20, "the repeat is put back");
    CHECK(led_sets >= 1 && leds_lit == LED_CAP, "the LEDs are lit again");
    CHECK(termios_sets == 1 && termios_set.c_lflag == tio.c_lflag, "the termios is put back");
    CHECK(flushes == 1 && repeat_cancels == 1, "the mode's change flushes and stops repeat");
    mode(K_UNICODE);
    CHECK(locks_held == 0, "every lock is given back");
}

/** evdev's EVIOCGREP and EVIOCSREP on a keyboard, and -ENOSYS on a device
 * that is no keyboard. */
static void evdev_repeat(void)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    struct evdev *kb, *button;
    fd_entry_t e, f;
    unsigned int rep[2];

    k_memset(keys, 0, sizeof(keys));
    kb = evdev_register("kb", EVDEV_BUS_I8042, 1, 1, keys);
    evdev_describe(kb, "isa0060/serio0/input0", EVDEV_SCAN | EVDEV_KEYBOARD);
    button = evdev_register("button", EVDEV_BUS_HOST, 0, 1, keys);
    evdev_describe(button, "LNXPWRBN/button/input0", 0);
    k_memset(&e, 0, sizeof(e));
    k_memset(&f, 0, sizeof(f));
    CHECK(evdev_open(&e, 0) == 0 && evdev_open(&f, 1) == 0, "both devices open");
    be(AS_ROOT);
    repeat_delay = 250;
    repeat_period = 33;
    CHECK(evdev_ioctl(&e, EVIOCGREP, (uintptr_t)rep) == 0 && rep[0] == 250 && rep[1] == 33,
          "EVIOCGREP reads the repeat");
    rep[0] = 400;
    rep[1] = 0;
    CHECK(evdev_ioctl(&e, EVIOCSREP, (uintptr_t)rep) == 0 && set_delay == 400 && set_period == 0,
          "EVIOCSREP sets it, a period of 0 turning repeat off");
    rep[0] = 0x80000000u;
    rep[1] = 25;
    CHECK(evdev_ioctl(&e, EVIOCSREP, (uintptr_t)rep) == 0 && repeat_delay == 400 &&
              repeat_period == 25,
          "a delay past INT_MAX is left as it is");
    CHECK(evdev_ioctl(&e, EVIOCGREP, 0) == -EFAULT && evdev_ioctl(&e, EVIOCSREP, 0) == -EFAULT,
          "the repeat's requests on a bad pointer");
    CHECK(evdev_ioctl(&f, EVIOCGREP, (uintptr_t)rep) == -ENOSYS &&
              evdev_ioctl(&f, EVIOCSREP, (uintptr_t)rep) == -ENOSYS,
          "a device that does not repeat is -ENOSYS");
    evdev_close(&e);
    evdev_close(&f);
}

int main(void)
{
    type_and_screen();
    modes();
    meta();
    leds();
    entries();
    unicode_entries();
    strings();
    accents();
    repeat();
    raw_modes();
    translating();
    save_restore();
    evdev_repeat();
    CHECK(locks_held == 0, "every lock is given back at the end");
    printf("kd-test: result: %d failures\n", failures);
    return failures ? 1 : 0;
}
