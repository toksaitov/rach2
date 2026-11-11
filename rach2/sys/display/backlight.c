/*
 * backlight.c: the backlight class (backlight.h), the panels' backlights
 * that drivers register and /sys/class/backlight reads and sets.
 */

#include "backlight.h"

#include "../lib/kdef.h"
#include "../lib/kerrno.h"
#include "../lib/kstd.h"
#include "../lib/spinlock.h"

/* A registered backlight: its driver, its highest level, the level root set
 * and bl_power as root wrote it. */
struct backlight {
    char name[32];
    const struct backlight_ops *ops;
    void *ctx;
    int max;
    int level;
    int power; /* BACKLIGHT_ON or BACKLIGHT_OFF */
};

static struct backlight lights[BACKLIGHT_MAX];
static int nlights;

/* Whether the console is blank; under `lock` with the lights' state. */
static int console_blank;
static struct spinlock lock = SPINLOCK_INIT;

/** Give backlight `b` its level and turn it on or off, by its driver's
 * switch or else by the level 0, with `lock` held. */
static void apply(struct backlight *b)
{
    int on = b->power == BACKLIGHT_ON && !console_blank;

    if (b->ops->power != NULL) {
        b->ops->set(b->ctx, b->level);
        b->ops->power(b->ctx, on);
    } else {
        b->ops->set(b->ctx, on ? b->level : 0);
    }
}

int backlight_register(const char *name, int max, const struct backlight_ops *ops, void *ctx)
{
    struct backlight *b;
    uint32_t flags;
    int i, level;

    if (nlights >= BACKLIGHT_MAX || max <= 0) {
        return -1;
    }
    for (i = 0; name[i] != '\0'; i++) {
        if (i >= (int)sizeof(b->name) - 1) {
            return -1;
        }
    }
    flags = spin_lock_irqsave(&lock);
    b = &lights[nlights];
    for (i = 0; name[i] != '\0'; i++) {
        b->name[i] = name[i];
    }
    b->name[i] = '\0';
    b->ops = ops;
    b->ctx = ctx;
    b->max = max;
    level = ops->get(ctx);
    b->level = level < 0 ? 0 : level > max ? max : level;
    b->power = BACKLIGHT_ON;
    i = nlights++;
    spin_unlock_irqrestore(&lock, flags);
    return i;
}

int backlight_rebind(const char *name, int max, const struct backlight_ops *ops, void *ctx)
{
    struct backlight *b;
    uint32_t flags;
    int i, len, level;

    for (len = 0; name[len] != '\0'; len++) {
    }
    i = backlight_find(name, len);
    if (i < 0 || max <= 0) {
        return -1;
    }
    flags = spin_lock_irqsave(&lock);
    b = &lights[i];
    b->ops = ops;
    b->ctx = ctx;
    b->max = max;
    level = ops->get(ctx);
    b->level = level < 0 ? 0 : level > max ? max : level;
    spin_unlock_irqrestore(&lock, flags);
    return i;
}

void backlight_init(void)
{
    intelbl_init();
}

void backlight_console_blank(int blank)
{
    uint32_t flags = spin_lock_irqsave(&lock);
    int i;

    console_blank = blank != 0;
    for (i = 0; i < nlights; i++) {
        apply(&lights[i]);
    }
    spin_unlock_irqrestore(&lock, flags);
}

void backlight_panic_unblank(void)
{
    int i;

    console_blank = 0;
    for (i = 0; i < nlights; i++) {
        apply(&lights[i]);
    }
}

int backlight_count(void)
{
    return nlights;
}

const char *backlight_name(int i)
{
    return i >= 0 && i < nlights ? lights[i].name : NULL;
}

int backlight_find(const char *name, int len)
{
    int i, j;

    for (i = 0; i < nlights; i++) {
        for (j = 0; j < len && lights[i].name[j] == name[j]; j++) {
        }
        if (j == len && lights[i].name[j] == '\0') {
            return i;
        }
    }
    return -1;
}

/** Append `v`, not negative, in decimal at `out[*pos]`, within `cap`. */
static void put_number(char *out, int cap, int *pos, int v)
{
    char digits[12];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (n > 0 && *pos < cap - 1) {
        out[(*pos)++] = digits[--n];
    }
}

int backlight_show(int i, int attr, char *out, int cap)
{
    struct backlight *b;
    uint32_t flags;
    int pos = 0, v;

    if (cap > 0) {
        out[0] = '\0';
    }
    if (i < 0 || i >= nlights || attr < 0 || attr >= BACKLIGHT_NATTR || cap < 2) {
        return -ENODEV;
    }
    b = &lights[i];
    if (attr == BACKLIGHT_TYPE) {
        static const char raw[] = "raw";

        for (v = 0; raw[v] != '\0' && pos < cap - 1; v++) {
            out[pos++] = raw[v];
        }
    } else {
        flags = spin_lock_irqsave(&lock);
        v = attr == BACKLIGHT_ACTUAL       ? b->ops->get(b->ctx)
            : attr == BACKLIGHT_POWER      ? b->power
            : attr == BACKLIGHT_BRIGHTNESS ? b->level
                                           : b->max;
        spin_unlock_irqrestore(&lock, flags);
        put_number(out, cap, &pos, v < 0 ? 0 : v);
    }
    if (pos < cap - 1) {
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    return pos;
}

int backlight_store(int i, int attr, const char *text, int len)
{
    struct backlight *b;
    unsigned long n;
    uint32_t flags;
    int v, rc;

    if (i < 0 || i >= nlights || (attr != BACKLIGHT_BRIGHTNESS && attr != BACKLIGHT_POWER)) {
        return -ENODEV;
    }
    b = &lights[i];
    rc = k_strtoul(text, len < 0 ? 0 : (size_t)len, &n);
    if (rc != 0) {
        return rc;
    }
    if (attr == BACKLIGHT_BRIGHTNESS ? n > (unsigned long)b->max
                                     : n != BACKLIGHT_ON && n != BACKLIGHT_OFF) {
        return -EINVAL;
    }
    v = (int)n;
    flags = spin_lock_irqsave(&lock);
    if (attr == BACKLIGHT_BRIGHTNESS) {
        b->level = v;
    } else {
        b->power = v;
    }
    apply(b);
    spin_unlock_irqrestore(&lock, flags);
    return 0;
}
