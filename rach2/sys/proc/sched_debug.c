/*
 * sched_debug.c: the scheduler's files in /sys/kernel/debug/sched, which
 * read and set the values the fair class runs by, as Linux 6.12's
 * kernel/sched/debug.c registers them.
 */

#include "sched_debug.h"
#include "fair.h"
#include "sched.h"
#include "../block/diskinfo.h"
#include "../fs/debugfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstd.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"

/* A text being written: `len` bytes so far, of which those below `cap`
 * are at `buf`, as debugfs's `show` sizes a text with a NULL buffer. */
struct out {
    char *buf;
    uint32_t cap, len;
};

/** Append `s` to `o`. */
static void put_str(struct out *o, const char *s)
{
    while (*s) {
        if (o->len < o->cap) {
            o->buf[o->len] = *s;
        }
        o->len++;
        s++;
    }
}

/** Append `v` to `o` in decimal. */
static void put_u64(struct out *o, uint64_t v)
{
    char d[24];

    (void)diskinfo_u64_dec(d, (int)sizeof(d), v);
    put_str(o, d);
}

/**
 * Read the number a write of `len` bytes at `buf` gives a file Linux makes
 * with debugfs_create_u32, as `simple_attr_write` reads it: the first 23
 * bytes, by kstrtoull with base 0, an optional `+`, then hexadecimal after
 * `0x`, octal after another leading 0, or decimal, and at most one newline.
 * 0 with the number in `*v`; -EINVAL for text that is no such number,
 * -ERANGE for one past 63 bits, or -EFAULT.
 */
static int32_t read_u64(const char __user *buf, uint32_t len, uint64_t *v)
{
    char text[24];
    uint32_t n = len < sizeof(text) - 1u ? len : (uint32_t)sizeof(text) - 1u;
    long long got;
    int rc;

    if (copy_from_user(text, buf, n) != 0) {
        return -EFAULT;
    }
    text[n] = '\0';
    if (text[0] == '-') {
        return -EINVAL;
    }
    rc = k_strtoll(text, n, &got);
    if (rc != 0) {
        return rc;
    }
    *v = (uint64_t)got;
    return 0;
}

/** Whether the strings `a` and `b` are the same. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** base_slice_ns: the base slice the scheduler gives an ordinary thread
 * without a slice of its own, `sysctl_sched_base_slice`, in decimal. */
static int32_t base_slice_show(void *data, char *buf, uint32_t cap)
{
    struct out o;
    uint64_t v;
    uint32_t flags;

    (void)data;
    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    flags = sched_lock_take();
    v = sysctl_sched_base_slice;
    sched_lock_give(flags);
    put_u64(&o, v);
    put_str(&o, "\n");
    return (int32_t)o.len;
}

/** Set the base slice to the low 32 bits of the number written, as
 * Linux's `debugfs_u32_set` truncates it: the next deadline each thread
 * takes without a slice of its own is that far ahead. `len`, or
 * `read_u64`'s error. */
static int32_t base_slice_store(void *data, const char __user *buf, uint32_t len)
{
    uint64_t v;
    uint32_t flags;
    int32_t rc = read_u64(buf, len, &v);

    (void)data;
    if (rc != 0) {
        return rc;
    }
    flags = sched_lock_take();
    sysctl_sched_base_slice = (uint32_t)v;
    sched_lock_give(flags);
    return (int32_t)len;
}

static const struct debugfs_text_ops base_slice_ops = {base_slice_show, base_slice_store};

/** features: each scheduler feature, in features.h's order, its name with
 * NO_ before it when it is off, each followed by a space, then a newline,
 * as Linux's `sched_feat_show`. */
static int32_t features_show(void *data, char *buf, uint32_t cap)
{
    struct out o;
    uint32_t set = sched_features;
    int i;

    (void)data;
    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    for (i = 0; i < FEAT_COUNT; i++) {
        if (!((set >> i) & 1u)) {
            put_str(&o, "NO_");
        }
        put_str(&o, sched_feature_names[i]);
        put_str(&o, " ");
    }
    put_str(&o, "\n");
    return (int32_t)o.len;
}

/**
 * Turn one feature on, written as its name, or off, written as NO_ and its
 * name, as Linux's `sched_feat_write` does: the first 63 bytes, with the
 * white space around them taken off, must name a feature whole. `len`,
 * -EINVAL for no such feature, or -EFAULT.
 */
static int32_t features_store(void *data, const char __user *buf, uint32_t len)
{
    char text[64];
    uint32_t n = len < sizeof(text) - 1u ? len : (uint32_t)sizeof(text) - 1u;
    uint32_t flags;
    char *cmp = text, *end;
    int i, neg = 0;

    (void)data;
    if (copy_from_user(text, buf, n) != 0) {
        return -EFAULT;
    }
    text[n] = '\0';
    while (*cmp == ' ' || (*cmp >= '\t' && *cmp <= '\r')) {
        cmp++;
    }
    end = cmp + k_strlen(cmp);
    while (end > cmp && (end[-1] == ' ' || (end[-1] >= '\t' && end[-1] <= '\r'))) {
        *--end = '\0';
    }
    if (cmp[0] == 'N' && cmp[1] == 'O' && cmp[2] == '_') {
        neg = 1;
        cmp += 3;
    }
    for (i = 0; i < FEAT_COUNT && !same(cmp, sched_feature_names[i]); i++) {
    }
    if (i == FEAT_COUNT) {
        return -EINVAL;
    }
    flags = sched_lock_take();
    if (neg) {
        sched_features &= ~(1u << i);
    } else {
        sched_features |= 1u << i;
    }
    sched_lock_give(flags);
    return (int32_t)len;
}

static const struct debugfs_text_ops features_ops = {features_show, features_store};

/* The files, Linux's names and modes (debug.c:501-537). */
static const struct {
    const char *path;
    uint32_t mode;
    const struct debugfs_text_ops *ops;
} files[] = {{"sched/features", 0644u, &features_ops},
             {"sched/base_slice_ns", 0644u, &base_slice_ops}};

void sched_debugfs_init(void)
{
    uint32_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (debugfs_create_text(files[i].path, files[i].mode, files[i].ops, NULL) != 0) {
            klog(KLOG_WARN, "sched", "debugfs: %s not registered", files[i].path);
        }
    }
}
