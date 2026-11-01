/* rfkill.c: the radio switches, their events on /dev/rfkill and their
 * attributes under /sys/class/rfkill, as Linux's net/rfkill gives them. */
#include "rfkill.h"
#include "io/poll.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "lib/spinlock.h"
#include "mm/kheap.h"
#include "mm/usercopy.h"
#include "proc/wait_queue.h"

struct rfkill {
    char name[RFKILL_NAME_MAX];
    unsigned type;  /* RFKILL_TYPE_*, never RFKILL_TYPE_ALL */
    unsigned index; /* the N of rfkillN */
    int phy;        /* the radio phyN it belongs to, or -1 */
    unsigned soft;  /* 0 or 1, changed only through set_block */
    unsigned hard;  /* 0 or 1, as the owner last reported */
    unsigned used;
    const struct rfkill_ops *ops;
    void *data;
};

/* An event as it waits in an open's queue. */
struct rfkill_event_rec {
    uint32_t index;
    uint8_t type, op, soft, hard, reasons;
};

/* An open of /dev/rfkill: a ring of `count` events from `head`. */
struct rfkill_client {
    struct rfkill_client *next;
    struct rfkill_event_rec queue[RFKILL_QUEUE];
    unsigned head, count;
    uint32_t size; /* bytes of an event this open reads and writes */
};

static const char *const type_names[RFKILL_NUM_TYPES] = {
    0, "wlan", "bluetooth", "ultrawideband", "wimax", "wwan", "gps", "fm", "nfc"};

static struct rfkill switches[RFKILL_MAX];
static unsigned next_index;
/* The soft block a new switch of each type starts with: the last
 * RFKILL_OP_CHANGE_ALL's, as Linux's rfkill_global_states. */
static unsigned type_soft[RFKILL_NUM_TYPES];
static struct rfkill_client *clients;
/* Guards the switches' states, the clients and their queues; taken with
 * interrupts off, since a driver reports a hard block from its handler. */
static struct spinlock rfkill_lock = SPINLOCK_INIT;
static wait_queue_t rfkill_wq; /* readers wait here; static storage starts it empty */

/** Append `op` for `rf` to `c`'s queue, dropping the oldest event when it is
 * full; rfkill_lock held. */
static void client_push(struct rfkill_client *c, const struct rfkill *rf, unsigned op)
{
    struct rfkill_event_rec *ev;

    if (c->count == RFKILL_QUEUE) {
        c->head = (c->head + 1) % RFKILL_QUEUE;
        c->count--;
    }
    ev = &c->queue[(c->head + c->count) % RFKILL_QUEUE];
    ev->index = rf->index;
    ev->type = (uint8_t)rf->type;
    ev->op = (uint8_t)op;
    ev->soft = (uint8_t)rf->soft;
    ev->hard = (uint8_t)rf->hard;
    ev->reasons = rf->hard ? RFKILL_HARD_BLOCK_SIGNAL : 0;
    c->count++;
}

/** Queue `op` for `rf` to every open; rfkill_lock held. Returns whether
 * there was one, for the caller to wake the readers once the lock is given
 * back. */
static int broadcast(const struct rfkill *rf, unsigned op)
{
    struct rfkill_client *c;

    for (c = clients; c; c = c->next)
        client_push(c, rf, op);
    return clients != 0;
}

/** The registered switch of index `index`, or NULL. */
static struct rfkill *find(int index)
{
    unsigned i;

    for (i = 0; i < RFKILL_MAX; ++i)
        if (switches[i].used && (int)switches[i].index == index)
            return &switches[i];
    return 0;
}

int rfkill_register(const char *name, unsigned type, int phy, const struct rfkill_ops *ops,
                    void *data, struct rfkill **out)
{
    struct rfkill *rf = 0;
    unsigned i;
    size_t len;
    uint32_t flags;
    int queued;

    if (!out)
        return -EINVAL;
    *out = 0;
    if (!name || !ops || !ops->set_block || type == RFKILL_TYPE_ALL || type >= RFKILL_NUM_TYPES)
        return -EINVAL;
    len = k_strlen(name);
    if (len == 0 || len >= RFKILL_NAME_MAX)
        return -EINVAL;
    for (i = 0; i < RFKILL_MAX && !rf; ++i)
        if (!switches[i].used)
            rf = &switches[i];
    if (!rf)
        return -ENOSPC;
    flags = spin_lock_irqsave(&rfkill_lock);
    k_memset(rf, 0, sizeof(*rf));
    k_memcpy(rf->name, name, len + 1);
    rf->type = type;
    rf->index = next_index++;
    rf->phy = phy;
    rf->soft = type_soft[type];
    rf->ops = ops;
    rf->data = data;
    rf->used = 1;
    queued = broadcast(rf, RFKILL_OP_ADD);
    spin_unlock_irqrestore(&rfkill_lock, flags);
    if (queued)
        wq_wake(&rfkill_wq, WQ_DATA);
    *out = rf;
    return 0;
}

void rfkill_unregister(struct rfkill *rf)
{
    uint32_t flags;
    int queued;

    if (!rf || !rf->used)
        return;
    flags = spin_lock_irqsave(&rfkill_lock);
    queued = broadcast(rf, RFKILL_OP_DEL);
    rf->used = 0;
    spin_unlock_irqrestore(&rfkill_lock, flags);
    if (queued)
        wq_wake(&rfkill_wq, WQ_DATA);
}

int rfkill_set_hw_state(struct rfkill *rf, int blocked)
{
    uint32_t flags;
    unsigned hard = blocked ? 1u : 0u;
    int queued = 0, now;

    if (!rf)
        return 0;
    flags = spin_lock_irqsave(&rfkill_lock);
    if (rf->used && rf->hard != hard) {
        rf->hard = hard;
        queued = broadcast(rf, RFKILL_OP_CHANGE);
    }
    now = rf->soft || rf->hard;
    spin_unlock_irqrestore(&rfkill_lock, flags);
    if (queued)
        wq_wake(&rfkill_wq, WQ_DATA);
    return now;
}

int rfkill_soft_blocked(const struct rfkill *rf)
{
    return rf && rf->soft;
}

unsigned rfkill_index(const struct rfkill *rf)
{
    return rf ? rf->index : 0;
}

/** Set the soft block of `rf`: the owner's `set_block` first, then the new
 * state and its RFKILL_OP_CHANGE, as Linux's rfkill_set_block. An unchanged
 * block calls nothing. Returns 0 or the owner's error. */
static int set_soft(struct rfkill *rf, unsigned blocked)
{
    uint32_t flags;
    int rc, queued = 0;

    if (!rf->used || rf->soft == blocked)
        return 0;
    rc = rf->ops->set_block(rf->data, (int)blocked);
    if (rc)
        return rc;
    flags = spin_lock_irqsave(&rfkill_lock);
    if (rf->used && rf->soft != blocked) {
        rf->soft = blocked;
        queued = broadcast(rf, RFKILL_OP_CHANGE);
    }
    spin_unlock_irqrestore(&rfkill_lock, flags);
    if (queued)
        wq_wake(&rfkill_wq, WQ_DATA);
    return 0;
}

int rfkill_nth(int n)
{
    unsigned i;
    int below = -1, best;

    /* The switches by rising index: the least index above the last one. */
    for (; n >= 0; --n) {
        best = -1;
        for (i = 0; i < RFKILL_MAX; ++i)
            if (switches[i].used && (int)switches[i].index > below &&
                (best < 0 || (int)switches[i].index < best))
                best = (int)switches[i].index;
        if (best < 0)
            return -1;
        below = best;
    }
    return below;
}

int rfkill_present(int index)
{
    return find(index) != 0;
}

int rfkill_lookup(const char *name, int len)
{
    static const char prefix[] = "rfkill";
    int i, index = 0;

    if (len < 7 || len > 15 || (len > 7 && name[6] == '0'))
        return -1;
    for (i = 0; i < 6; ++i)
        if (name[i] != prefix[i])
            return -1;
    for (i = 6; i < len; ++i) {
        if (name[i] < '0' || name[i] > '9')
            return -1;
        index = index * 10 + (name[i] - '0');
    }
    return find(index) ? index : -1;
}

int rfkill_phy(int index)
{
    const struct rfkill *rf = find(index);

    return rf ? rf->phy : -1;
}

/* A bounded writer of attribute text. */
struct rfkill_text {
    char *out;
    int cap, len;
};

/** Append the NUL-terminated `s`, counting what does not fit. */
static void put_str(struct rfkill_text *t, const char *s)
{
    for (; *s; ++s, ++t->len)
        if (t->len < t->cap)
            t->out[t->len] = *s;
}

/** Append `v` in decimal. */
static void put_uint(struct rfkill_text *t, unsigned v)
{
    char digits[12];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n > 0) {
        if (t->len < t->cap)
            t->out[t->len] = digits[n - 1];
        --n;
        ++t->len;
    }
}

/** The value of `state`: hard blocked, soft blocked or unblocked. */
static unsigned user_state(const struct rfkill *rf)
{
    return rf->hard   ? RFKILL_STATE_HARD_BLOCKED
           : rf->soft ? RFKILL_STATE_SOFT_BLOCKED
                      : RFKILL_STATE_UNBLOCKED;
}

int rfkill_show(int index, int attr, char *out, int cap)
{
    struct rfkill_text t;
    struct rfkill rf;
    struct rfkill *live = find(index);
    uint32_t flags;

    if (!live || attr < 0 || attr >= RFKILL_ATTRS)
        return -ENOENT;
    flags = spin_lock_irqsave(&rfkill_lock);
    rf = *live;
    spin_unlock_irqrestore(&rfkill_lock, flags);
    t.out = out;
    t.cap = cap;
    t.len = 0;
    switch (attr) {
    case RFKILL_ATTR_HARD:
        put_uint(&t, rf.hard);
        break;
    case RFKILL_ATTR_REASONS:
        put_str(&t, rf.hard ? "0x1" : "0x0");
        break;
    case RFKILL_ATTR_INDEX:
        put_uint(&t, rf.index);
        break;
    case RFKILL_ATTR_NAME:
        put_str(&t, rf.name);
        break;
    case RFKILL_ATTR_PERSIST:
        put_str(&t, "0");
        break;
    case RFKILL_ATTR_SOFT:
        put_uint(&t, rf.soft);
        break;
    case RFKILL_ATTR_STATE:
        put_uint(&t, user_state(&rf));
        break;
    case RFKILL_ATTR_TYPE:
        put_str(&t, type_names[rf.type]);
        break;
    default:
        put_str(&t, "RFKILL_NAME=");
        put_str(&t, rf.name);
        put_str(&t, "\nRFKILL_TYPE=");
        put_str(&t, type_names[rf.type]);
        put_str(&t, "\nRFKILL_STATE=");
        put_uint(&t, user_state(&rf));
        put_str(&t, rf.hard ? "\nRFKILL_HW_BLOCK_REASON=0x1" : "\nRFKILL_HW_BLOCK_REASON=0x0");
        break;
    }
    put_str(&t, "\n");
    return t.len > cap ? -ENOSPC : t.len;
}

int rfkill_store(int index, int attr, const char *text, int len)
{
    struct rfkill *rf = find(index);
    unsigned v;

    if (!rf)
        return -ENOENT;
    if ((attr != RFKILL_ATTR_SOFT && attr != RFKILL_ATTR_STATE) || len != 1 ||
        (text[0] != '0' && text[0] != '1'))
        return -EINVAL;
    v = (unsigned)(text[0] - '0');
    /* `state` is the user state: 0 soft blocked, 1 unblocked. */
    return set_soft(rf, attr == RFKILL_ATTR_SOFT ? v : !v);
}

int32_t rfkill_open(fd_entry_t *e)
{
    struct rfkill_client *c = (struct rfkill_client *)kzalloc(sizeof(*c));
    uint32_t flags;
    int n, index;

    if (!c)
        return -ENOMEM;
    c->size = RFKILL_EVENT_SIZE_V1;
    flags = spin_lock_irqsave(&rfkill_lock);
    for (n = 0; (index = rfkill_nth(n)) >= 0; ++n)
        client_push(c, find(index), RFKILL_OP_ADD);
    c->next = clients;
    clients = c;
    spin_unlock_irqrestore(&rfkill_lock, flags);
    e->priv = c;
    return 0;
}

/** The wait condition of a read: an event is queued. */
static int rfkill_readable(void *arg)
{
    return ((struct rfkill_client *)arg)->count != 0;
}

int32_t rfkill_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct rfkill_client *c = (struct rfkill_client *)e->priv;
    struct rfkill_event_rec ev;
    uint8_t out[RFKILL_EVENT_SIZE_EXT];
    uint32_t flags, n;

    for (;;) {
        flags = spin_lock_irqsave(&rfkill_lock);
        if (c->count) {
            ev = c->queue[c->head];
            c->head = (c->head + 1) % RFKILL_QUEUE;
            c->count--;
            spin_unlock_irqrestore(&rfkill_lock, flags);
            break;
        }
        spin_unlock_irqrestore(&rfkill_lock, flags);
        if (e->ofd->status_flags & OFD_NONBLOCK)
            return -EAGAIN;
        if (wq_wait_event(&rfkill_wq, rfkill_readable, c) < 0)
            return -EINTR;
    }
    k_memcpy(out, &ev.index, 4); /* host order, as Linux's __u32 */
    out[4] = ev.type;
    out[5] = ev.op;
    out[6] = ev.soft;
    out[7] = ev.hard;
    out[8] = ev.reasons;
    n = count < c->size ? count : c->size;
    if (n > sizeof(out))
        n = sizeof(out);
    /* Like Linux, the event is gone even when the copy faults. */
    return copy_to_user(buf, out, n) != 0 ? -EFAULT : (int32_t)n;
}

int32_t rfkill_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct rfkill_client *c = (struct rfkill_client *)e->priv;
    uint8_t ev[RFKILL_EVENT_SIZE_EXT];
    uint32_t index;
    unsigned type, soft, i;

    if (count < RFKILL_EVENT_SIZE_V1 - 1)
        return -EINVAL;
    if (count > sizeof(ev))
        count = sizeof(ev);
    if (count > c->size)
        count = c->size;
    k_memset(ev, 0, sizeof(ev));
    if (copy_from_user(ev, buf, count) != 0)
        return -EFAULT;
    k_memcpy(&index, ev, 4);
    type = ev[4];
    soft = ev[6] ? 1u : 0u;
    if (type >= RFKILL_NUM_TYPES)
        return -EINVAL;
    if (ev[5] == RFKILL_OP_CHANGE_ALL) {
        for (i = 0; i < RFKILL_NUM_TYPES; ++i)
            if (type == RFKILL_TYPE_ALL || i == type)
                type_soft[i] = soft;
    } else if (ev[5] != RFKILL_OP_CHANGE) {
        return -EINVAL;
    }
    /* An owner's refusal leaves its switch as it was, and the write goes
     * on to the rest, as Linux's does. */
    for (i = 0; i < RFKILL_MAX; ++i)
        if (switches[i].used && (type == RFKILL_TYPE_ALL || switches[i].type == type) &&
            (ev[5] == RFKILL_OP_CHANGE_ALL || switches[i].index == index))
            (void)set_soft(&switches[i], soft);
    return (int32_t)count;
}

short rfkill_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct rfkill_client *c = (struct rfkill_client *)e->priv;

    *wq = &rfkill_wq;
    return (short)(events & ((c->count ? POLLIN : 0) | POLLOUT));
}

int32_t rfkill_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct rfkill_client *c = (struct rfkill_client *)e->priv;
    uint32_t size;

    if (request == RFKILL_IOCTL_NOINPUT)
        return 0;
    if (request != RFKILL_IOCTL_MAX_SIZE)
        return -ENOTTY;
    if (copy_from_user(&size, (const void __user *)arg, sizeof(size)) != 0)
        return -EFAULT;
    if (size < RFKILL_EVENT_SIZE_V1 || size > 255)
        return -EINVAL;
    c->size = size;
    return 0;
}

void rfkill_close(fd_entry_t *e)
{
    struct rfkill_client *c = (struct rfkill_client *)e->priv;
    struct rfkill_client **p;
    uint32_t flags;

    if (!c)
        return;
    flags = spin_lock_irqsave(&rfkill_lock);
    for (p = &clients; *p; p = &(*p)->next) {
        if (*p == c) {
            *p = c->next;
            break;
        }
    }
    spin_unlock_irqrestore(&rfkill_lock, flags);
    e->priv = 0;
    kfree(c);
}
