/*
 * kd.c: the console's keyboard as Linux's console keeps it: its key events,
 * its KD* requests and the state saved for the screen's owners (kd.h).
 */
#include "kd.h"

#include "display/fb.h"
#include "input.h"
#include "keymap.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "lib/spinlock.h"
#include "mm/kheap.h"
#include "mm/usercopy.h"
#include "proc/percpu.h"
#include "proc/process.h"
#include "tty/tty.h"

/* The process that holds /fb, 0 for none; syscall.c's, declared in
 * syscall_internal.h, which names the descriptors' types. */
extern volatile uint32_t fb_owner_pid;

/* The types of entry whose value KDSKBENT checks: KT_LATIN to KT_BRL. An
 * entry of type 15 or more is a code point's, as Linux reads it. */
#define KD_NR_TYPES 15

/* Entry 0 of a table this file allocated, as KDGKBENT reads it on Linux. */
#define KD_ALLOCATED KM_K(KM_T_SPEC, 126)

/* KDSETLED's value while the LEDs follow the lock flags. */
#define KD_LEDS_FOLLOW 0xFFu

/* The largest value of each type's entries, Linux's `max_vals`: the function
 * strings, the 20 actions of KT_SPEC, the keypad's 20 keys, the 27 accents,
 * the four cursor keys, the nine modifiers, the 26 digits of KT_ASCII, and
 * the eleven Braille dots and keys. */
static const unsigned char max_vals[KD_NR_TYPES] = {255, 255, 19, 19,  26, 255, 3, 8,
                                                    255, 25,  8,  255, 8,  255, 10};

/*
 * The console's keyboard. The mode, the meta setting and the LEDs are words
 * the key path reads in interrupt context; the tables, strings and accents
 * change under `kd_lock`, which the key path takes to read them, and are
 * freed only once out of reach.
 */
static struct spinlock kd_lock = SPINLOCK_INIT;
static volatile int kd_mode = K_UNICODE;
static int kd_meta = K_ESCPREFIX;
static unsigned int kd_leds = KD_LEDS_FOLLOW; /* KDSETLED's, or KD_LEDS_FOLLOW */
static unsigned int kd_default_flags;         /* KDSKBLED's bits 4-6 */
static struct keymap_state kd_kb;             /* the locks and their flags */

/* The keymap: KM_MAX_TABLES tables, a NULL one absent. A table is the
 * built-in one of keymap.c until it is first written, and then a copy of
 * KM_NR_KEYS entries this file allocated, which `mine` holds as well. */
static const unsigned short *tables[KM_MAX_TABLES];
static unsigned short *mine[KM_MAX_TABLES];

/* The function strings, NULL for an empty one, each the built-in string or
 * a copy this file allocated, which `funcs_mine` holds as well. */
static const char *funcs[KM_MAX_FUNC];
static char *funcs_mine[KM_MAX_FUNC];

/* The accent table, in code points. */
static struct keymap_diacr diacrs[KM_MAX_DIACR];
static unsigned int ndiacr;

static int kd_ready;

/** Fill the keymap, strings and accents from keymap.c's built-in ones, and
 * the locks from nothing, the first time any is needed. Process context. */
static void kd_init(void)
{
    uint32_t flags;
    unsigned int i;

    if (kd_ready) {
        return;
    }
    flags = spin_lock_irqsave(&kd_lock);
    if (!kd_ready) {
        for (i = 0; i < KM_MAX_TABLES; i++) {
            tables[i] = keymap_default_tables[i];
        }
        for (i = 0; i < KM_MAX_FUNC; i++) {
            funcs[i] = keymap_default_func[i];
        }
        ndiacr = keymap_default_ndiacr < KM_MAX_DIACR ? keymap_default_ndiacr : KM_MAX_DIACR;
        for (i = 0; i < ndiacr; i++) {
            diacrs[i] = keymap_default_diacr[i];
        }
        keymap_state_init(&kd_kb, 0);
        kd_ready = 1;
    }
    spin_unlock_irqrestore(&kd_lock, flags);
}

/** Whether the caller may set the keyboard's mode and LEDs: a process of the
 * console's session, the screen's owner, or a privileged one. */
static int kd_may_set(void)
{
    process_t *p = current_proc;

    if (proc_privileged(p)) {
        return 1;
    }
    if (console_tty.sid != 0 && (int)p->sid == console_tty.sid) {
        return 1;
    }
    return fb_owner_pid != 0 && fb_owner_pid == p->tgid;
}

/** Whether the caller may change the keymap, the strings, the accents and
 * the repeat, which are the whole system's: a privileged process alone. */
static int kd_may_configure(void)
{
    return proc_privileged(current_proc);
}

/** The LEDs lit: KDSETLED's, or the lock flags while they follow those. */
static unsigned int kd_leds_lit(void)
{
    return kd_leds == KD_LEDS_FOLLOW ? kd_kb.ledflags & 7u : kd_leds;
}

/** Store the byte `v` at the user address `arg`. 0, or -EFAULT. */
static int32_t put_byte(uintptr_t arg, unsigned int v)
{
    unsigned char b = (unsigned char)v;

    return copy_to_user((void __user *)arg, &b, 1) != 0 ? -EFAULT : 0;
}

/** Store the int `v` at the user address `arg`. 0, or -EFAULT. */
static int32_t put_int(uintptr_t arg, int v)
{
    return copy_to_user((void __user *)arg, &v, sizeof(v)) != 0 ? -EFAULT : 0;
}

/** Put the keyboard in mode `mode`, K_*, for KDSKBMODE and a restore: stop
 * a held key's repeat and discard the console's unread input, as Linux does
 * after the request. -EINVAL for a mode that is none of the five. */
static int32_t kd_set_mode(int mode)
{
    if (mode < K_RAW || mode > K_OFF) {
        return -EINVAL;
    }
    kd_mode = mode;
    input_cancel_repeat();
    tty_flush_input(&console_tty);
    return 0;
}

/**
 * KDGKBENT's entry of key `index` in table `table`: a table that does not
 * exist reads as K_NOSUCHMAP at key 0 and K_HOLE at every other, and a code
 * point's entry as K_HOLE in K_XLATE, which sends 8-bit characters. Linux
 * gives K_HOLE for a code point in every mode but K_UNICODE; Rach2 gives it
 * in K_RAW and K_MEDIUMRAW too, so that `term`, which reads key codes,
 * translates through the console's keymap.
 */
static unsigned int kd_get_entry(unsigned int table, unsigned int index)
{
    unsigned int v;
    uint32_t flags = spin_lock_irqsave(&kd_lock);

    if (tables[table] == NULL) {
        v = index != 0 ? KM_HOLE : KM_NOSUCHMAP;
    } else {
        v = tables[table][index];
    }
    spin_unlock_irqrestore(&kd_lock, flags);
    if (kd_mode == K_XLATE && KM_TYPE(v) >= KD_NR_TYPES) {
        v = KM_HOLE;
    }
    return v;
}

/**
 * KDSKBENT: set the entry of key `index` in table `table` to `value`. The
 * value K_NOSUCHMAP at key 0 frees the table, but never table 0; any other
 * value at key 0 is only checked, as on Linux. A typed value must be within
 * its type's range; a code point's is taken in every mode, where Linux takes
 * it only in K_UNICODE, so that `loadkeys` loads a Unicode keymap while
 * `term` holds the keyboard in K_MEDIUMRAW. A table that does not exist, or
 * is the built-in one, is first copied into one of this file's, whose entry
 * 0 is K_ALLOCATED, as Linux's KDGKBENT reads an allocated table's.
 * Returns 0, -EINVAL for a value out of range, or -ENOMEM.
 */
static int32_t kd_set_entry(unsigned int table, unsigned int index, unsigned int value)
{
    unsigned short *fresh = NULL;
    unsigned short *old = NULL;
    unsigned int i;
    uint32_t flags;

    if (index == 0 && value == KM_NOSUCHMAP) {
        flags = spin_lock_irqsave(&kd_lock);
        if (table != 0) {
            old = mine[table];
            tables[table] = NULL;
            mine[table] = NULL;
        }
        spin_unlock_irqrestore(&kd_lock, flags);
        kfree(old);
        return 0;
    }
    if (KM_TYPE(value) < KD_NR_TYPES && KM_VALUE(value) > max_vals[KM_TYPE(value)]) {
        return -EINVAL;
    }
    if (index == 0) {
        return 0;
    }
    for (;;) {
        if (mine[table] == NULL && fresh == NULL) {
            fresh = (unsigned short *)kmalloc(KM_NR_KEYS * sizeof(unsigned short));
            if (fresh == NULL) {
                return -ENOMEM;
            }
        }
        flags = spin_lock_irqsave(&kd_lock);
        if (mine[table] != NULL) {
            break;
        }
        if (fresh != NULL) {
            for (i = 0; i < KM_NR_KEYS; i++) {
                fresh[i] = tables[table] != NULL ? tables[table][i] : (unsigned short)KM_HOLE;
            }
            fresh[0] = (unsigned short)KD_ALLOCATED;
            tables[table] = fresh;
            mine[table] = fresh;
            fresh = NULL;
            break;
        }
        spin_unlock_irqrestore(&kd_lock, flags);
    }
    mine[table][index] = (unsigned short)value;
    spin_unlock_irqrestore(&kd_lock, flags);
    kfree(fresh);
    return 0;
}

/** KDGKBENT and KDSKBENT on the struct kbentry at `arg`. */
static int32_t kd_entry_request(uint32_t request, uintptr_t arg)
{
    struct kbentry kbe;
    unsigned short v;

    if (request == KDSKBENT && !kd_may_configure()) {
        return -EPERM;
    }
    if (copy_from_user(&kbe, (const void __user *)arg, sizeof(kbe)) != 0) {
        return -EFAULT;
    }
    if (request == KDSKBENT) {
        return kd_set_entry(kbe.kb_table, kbe.kb_index, kbe.kb_value);
    }
    v = (unsigned short)kd_get_entry(kbe.kb_table, kbe.kb_index);
    if (copy_to_user((void __user *)&((struct kbentry __user *)arg)->kb_value, &v, sizeof(v)) !=
        0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * KDGKBSENT and KDSKBSENT on the struct kbsentry at `arg`. A string read is
 * copied with its null, and an empty one is "". A string set must end within
 * the 512 bytes of `kb_string`, -EINVAL otherwise, as Linux has it.
 */
static int32_t kd_string_request(uint32_t request, uintptr_t arg)
{
    struct kbsentry __user *u = (struct kbsentry __user *)arg;
    unsigned char func;
    char *buf, *old = NULL;
    const char *s;
    uint32_t flags;
    int32_t rc;
    int n;

    if (request == KDSKBSENT && !kd_may_configure()) {
        return -EPERM;
    }
    if (copy_from_user(&func, (const void __user *)&u->kb_func, 1) != 0) {
        return -EFAULT;
    }
    buf = (char *)kmalloc(KM_FUNC_BYTES);
    if (buf == NULL) {
        return -ENOMEM;
    }
    if (request == KDGKBSENT) {
        flags = spin_lock_irqsave(&kd_lock);
        s = funcs[func] != NULL ? funcs[func] : "";
        n = (int)k_strlen(s);
        k_memcpy(buf, s, (size_t)n + 1);
        spin_unlock_irqrestore(&kd_lock, flags);
        rc = copy_to_user((void __user *)u->kb_string, buf, (size_t)n + 1) != 0 ? -EFAULT : 0;
        kfree(buf);
        return rc;
    }
    n = strncpy_from_user(buf, (const char __user *)u->kb_string, KM_FUNC_BYTES);
    if (n < 0) {
        kfree(buf);
        return n == -ENAMETOOLONG ? -EINVAL : n;
    }
    flags = spin_lock_irqsave(&kd_lock);
    old = funcs_mine[func];
    funcs[func] = buf;
    funcs_mine[func] = buf;
    spin_unlock_irqrestore(&kd_lock, flags);
    kfree(old);
    return 0;
}

/** A code point in K_XLATE's 8 bits: itself below 256, else 0xFF, Linux's
 * -1 for a code point the console's 8-bit map does not hold. */
static unsigned char kd_to_8bit(unsigned long u)
{
    return u < 256 ? (unsigned char)u : 0xFF;
}

/**
 * KDGKBDIACR and KDGKBDIACRUC: the accent table, its count and then its
 * entries, at `arg`, in 8 bits or in code points.
 */
static int32_t kd_get_diacrs(uint32_t request, uintptr_t arg)
{
    struct kbdiacruc *uc;
    struct kbdiacr *b;
    unsigned int i, n;
    uint32_t flags;
    int32_t rc = 0;

    uc = (struct kbdiacruc *)kmalloc(KM_MAX_DIACR * sizeof(*uc));
    b = (struct kbdiacr *)kmalloc(KM_MAX_DIACR * sizeof(*b));
    if (uc == NULL || b == NULL) {
        kfree(uc);
        kfree(b);
        return -ENOMEM;
    }
    flags = spin_lock_irqsave(&kd_lock);
    n = ndiacr;
    for (i = 0; i < n; i++) {
        uc[i].diacr = (unsigned int)diacrs[i].diacr;
        uc[i].base = (unsigned int)diacrs[i].base;
        uc[i].result = (unsigned int)diacrs[i].result;
        b[i].diacr = kd_to_8bit(diacrs[i].diacr);
        b[i].base = kd_to_8bit(diacrs[i].base);
        b[i].result = kd_to_8bit(diacrs[i].result);
    }
    spin_unlock_irqrestore(&kd_lock, flags);
    if (copy_to_user((void __user *)arg, &n, sizeof(n)) != 0) {
        rc = -EFAULT;
    } else if (request == KDGKBDIACRUC) {
        if (n != 0 && copy_to_user((void __user *)(arg + sizeof(n)), uc, n * sizeof(*uc)) != 0) {
            rc = -EFAULT;
        }
    } else if (n != 0 && copy_to_user((void __user *)(arg + sizeof(n)), b, n * sizeof(*b)) != 0) {
        rc = -EFAULT;
    }
    kfree(uc);
    kfree(b);
    return rc;
}

/**
 * KDSKBDIACR and KDSKBDIACRUC: replace the accent table with the `kb_cnt`
 * entries at `arg`, fewer than 256 as Linux allows, -EINVAL otherwise. An
 * 8-bit entry's bytes are Latin-1 code points. The table is left as it was
 * when the entries cannot be read.
 */
static int32_t kd_set_diacrs(uint32_t request, uintptr_t arg)
{
    struct keymap_diacr *fresh;
    unsigned int ct, i, size;
    uint32_t flags;
    void *raw;

    if (!kd_may_configure()) {
        return -EPERM;
    }
    if (copy_from_user(&ct, (const void __user *)arg, sizeof(ct)) != 0) {
        return -EFAULT;
    }
    if (ct >= KM_MAX_DIACR) {
        return -EINVAL;
    }
    size = request == KDSKBDIACRUC ? sizeof(struct kbdiacruc) : sizeof(struct kbdiacr);
    fresh = (struct keymap_diacr *)kmalloc(KM_MAX_DIACR * sizeof(*fresh));
    raw = kmalloc(KM_MAX_DIACR * sizeof(struct kbdiacruc));
    if (fresh == NULL || raw == NULL) {
        kfree(fresh);
        kfree(raw);
        return -ENOMEM;
    }
    if (ct != 0 && copy_from_user(raw, (const void __user *)(arg + sizeof(ct)), ct * size) != 0) {
        kfree(fresh);
        kfree(raw);
        return -EFAULT;
    }
    for (i = 0; i < ct; i++) {
        if (request == KDSKBDIACRUC) {
            const struct kbdiacruc *e = (const struct kbdiacruc *)raw + i;

            fresh[i].diacr = e->diacr;
            fresh[i].base = e->base;
            fresh[i].result = e->result;
        } else {
            const struct kbdiacr *e = (const struct kbdiacr *)raw + i;

            fresh[i].diacr = e->diacr;
            fresh[i].base = e->base;
            fresh[i].result = e->result;
        }
    }
    flags = spin_lock_irqsave(&kd_lock);
    for (i = 0; i < ct; i++) {
        diacrs[i] = fresh[i];
    }
    ndiacr = ct;
    spin_unlock_irqrestore(&kd_lock, flags);
    kfree(fresh);
    kfree(raw);
    return 0;
}

/**
 * KDKBDREP: set the repeat from the struct kbd_repeat at `arg`, a time of 0
 * or less left as it is, and store there the times in force.
 */
static int32_t kd_repeat_request(uintptr_t arg)
{
    struct kbd_repeat r;

    if (!kd_may_configure()) {
        return -EPERM;
    }
    if (copy_from_user(&r, (const void __user *)arg, sizeof(r)) != 0) {
        return -EFAULT;
    }
    input_set_repeat(r.delay > 0 ? r.delay : -1, r.period > 0 ? r.period : -1);
    input_get_repeat(&r.delay, &r.period);
    return copy_to_user((void __user *)arg, &r, sizeof(r)) != 0 ? -EFAULT : 0;
}

/**
 * The requests that set the keyboard's mode, LEDs, lock flags and meta from
 * the value `arg`, open to `kd_may_set`'s callers. -ENOTTY for another.
 */
static int32_t kd_set_request(uint32_t request, uintptr_t arg)
{
    uint32_t flags;

    switch (request) {
    case KDSKBMODE:
        return kd_set_mode((int)arg);
    case KDSKBMETA:
        if (arg != K_METABIT && arg != K_ESCPREFIX) {
            return -EINVAL;
        }
        kd_meta = (int)arg;
        return 0;
    case KDSETLED:
        kd_leds = (arg & ~7u) == 0 ? (unsigned int)arg : KD_LEDS_FOLLOW;
        input_set_leds(kd_leds_lit());
        return 0;
    case KDSKBLED:
        if (arg & ~0x77u) {
            return -EINVAL;
        }
        flags = spin_lock_irqsave(&kd_lock);
        kd_kb.ledflags = (unsigned int)arg & 7u;
        kd_default_flags = ((unsigned int)arg >> 4) & 7u;
        spin_unlock_irqrestore(&kd_lock, flags);
        input_set_leds(kd_leds_lit());
        return 0;
    default:
        return -ENOTTY;
    }
}

int32_t kd_ioctl(uint32_t request, uintptr_t arg)
{
    kd_init();
    switch (request) {
    case KDGKBTYPE:
        return put_byte(arg, KB_101);
    case KDGKBMODE:
        return put_int(arg, kd_mode);
    case KDGKBMETA:
        return put_int(arg, kd_meta);
    case KDGETLED:
        return put_byte(arg, kd_leds_lit());
    case KDGKBLED:
        return put_byte(arg, kd_kb.ledflags | kd_default_flags << 4);
    case KDGETMODE:
        return put_int(arg, gfx_is_active() ? KD_GRAPHICS : KD_TEXT);
    case KDSKBMODE:
    case KDSKBMETA:
    case KDSETLED:
    case KDSKBLED:
        if (!kd_may_set()) {
            return -EPERM;
        }
        return kd_set_request(request, arg);
    case KDGKBENT:
    case KDSKBENT:
        return kd_entry_request(request, arg);
    case KDGKBSENT:
    case KDSKBSENT:
        return kd_string_request(request, arg);
    case KDGKBDIACR:
    case KDGKBDIACRUC:
        return kd_get_diacrs(request, arg);
    case KDSKBDIACR:
    case KDSKBDIACRUC:
        return kd_set_diacrs(request, arg);
    case KDKBDREP:
        return kd_repeat_request(arg);
    default:
        return -ENOTTY;
    }
}

int kd_translates(void)
{
    return kd_mode == K_XLATE || kd_mode == K_UNICODE;
}

void kd_raw_code(uint8_t code)
{
    if (kd_mode == K_RAW) {
        tty_input_char(&console_tty, code);
    }
}

void kd_key_event(const void *device, unsigned int keycode, int value)
{
    (void)device;
    if (kd_mode != K_MEDIUMRAW || keycode > 0x3FFFu) {
        return;
    }
    if (keycode < 128) {
        tty_input_char(&console_tty,
                       (unsigned char)(keycode | (value == KD_KEY_RELEASE ? 0x80u : 0u)));
        return;
    }
    tty_input_char(&console_tty, value == KD_KEY_RELEASE ? 0x80 : 0x00);
    tty_input_char(&console_tty, (unsigned char)((keycode >> 7) | 0x80u));
    tty_input_char(&console_tty, (unsigned char)(keycode | 0x80u));
}

void kd_state_save(struct kd_state *st)
{
    uint32_t flags;

    kd_init();
    flags = spin_lock_irqsave(&kd_lock);
    st->kbmode = kd_mode;
    st->meta = kd_meta;
    st->locks = kd_kb.locks;
    st->slocks = kd_kb.slocks;
    st->ledflags = kd_kb.ledflags | kd_default_flags << 4;
    st->leds = kd_leds;
    spin_unlock_irqrestore(&kd_lock, flags);
    input_get_repeat(&st->repeat_delay, &st->repeat_period);
    st->termios = console_tty.ldisc.tio;
}

void kd_state_restore(const struct kd_state *st)
{
    uint32_t flags;

    kd_init();
    flags = spin_lock_irqsave(&kd_lock);
    kd_meta = st->meta;
    kd_kb.locks = st->locks;
    kd_kb.slocks = st->slocks;
    kd_kb.ledflags = st->ledflags & 7u;
    kd_default_flags = (st->ledflags >> 4) & 7u;
    kd_leds = st->leds;
    spin_unlock_irqrestore(&kd_lock, flags);
    input_set_leds(kd_leds_lit());
    input_set_repeat(st->repeat_delay, st->repeat_period);
    tty_set_termios(&console_tty, &st->termios, 0);
    kd_set_mode(st->kbmode);
}
