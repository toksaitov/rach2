/* The walk sequence; the contract is in walkseq.h. */
#include "walkseq.h"

#include "debugfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/spinlock.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"

/* The sequence and the count of changes under way, under `seq_lock`; the
 * waiters of `vfs_seq_wait` on `seq_wq`, which a zeroed queue serves. */
static struct spinlock seq_lock = SPINLOCK_INIT_NAMED("walkseq");
static uint64_t seq_now;
static uint32_t seq_active;
static wait_queue_t seq_wq;

void vfs_seq_begin(void)
{
    uint32_t flags = spin_lock_irqsave(&seq_lock);

    seq_active++;
    seq_now++;
    spin_unlock_irqrestore(&seq_lock, flags);
}

void vfs_seq_end(void)
{
    uint32_t flags = spin_lock_irqsave(&seq_lock);
    int idle;

    seq_active--;
    seq_now++;
    idle = seq_active == 0;
    spin_unlock_irqrestore(&seq_lock, flags);
    if (idle) {
        wq_wake(&seq_wq, 0);
    }
}

int vfs_seq_read(uint64_t *seq)
{
    uint32_t flags = spin_lock_irqsave(&seq_lock);
    int calm = seq_active == 0;

    *seq = seq_now;
    spin_unlock_irqrestore(&seq_lock, flags);
    return calm;
}

int vfs_seq_still(uint64_t seq)
{
    uint32_t flags = spin_lock_irqsave(&seq_lock);
    int still = seq_active == 0 && seq_now == seq;

    spin_unlock_irqrestore(&seq_lock, flags);
    return still;
}

/** The wait condition of `vfs_seq_wait`: no change is under way. */
static int seq_calm(void *arg)
{
    uint64_t seq;

    (void)arg;
    return vfs_seq_read(&seq);
}

void vfs_seq_wait(uint64_t *seq)
{
    while (!vfs_seq_read(seq) && current_proc) {
        wq_wait_event_nointr(&seq_wq, seq_calm, NULL);
    }
}

/*
 * The walk gate, /sys/kernel/debug/vfs/walk_gate. The thread `gate_pid`
 * counts each WALK_GATE_ event it reaches, and the first time it reaches
 * `gate_stop` sleeps there until a write of `release`, so that a test can
 * change the tree at a known point of a walk and then let it go on; all
 * under `gate_lock`. `gate_stop` is -1 once that stop is used.
 */
static struct spinlock gate_lock = SPINLOCK_INIT_NAMED("walkgate");
static int gate_pid;
static int gate_stop = -1;
static int gate_released;
static int gate_waiting;
static uint32_t gate_count[WALK_GATE_EVENTS];
static wait_queue_t gate_wq;

/* The events' names, in the order of their numbers. */
static const char *const gate_names[WALK_GATE_EVENTS] = {"full", "unlocked", "miss"};

/** The wait condition of `walk_gate`: the stop is released. */
static int gate_open(void *arg)
{
    (void)arg;
    return gate_released;
}

void walk_gate(int event)
{
    uint32_t flags;
    int stop;

    if (!gate_pid || !current_proc || event < 0 || event >= WALK_GATE_EVENTS) {
        return;
    }
    flags = spin_lock_irqsave(&gate_lock);
    stop = gate_pid == (int)current_proc->pid;
    if (stop) {
        gate_count[event]++;
        stop = event == gate_stop;
    }
    if (stop) {
        gate_stop = -1;
        gate_waiting = 1;
    }
    spin_unlock_irqrestore(&gate_lock, flags);
    if (stop) {
        (void)wq_wait_event_as(&gate_wq, gate_open, NULL, "walk_gate");
        flags = spin_lock_irqsave(&gate_lock);
        gate_waiting = 0;
        spin_unlock_irqrestore(&gate_lock, flags);
    }
}

/* A line of the gate's text being made: `len` counts every byte, and those
 * past `cap` are not stored. */
struct gate_text {
    char *buf;
    uint32_t cap;
    uint32_t len;
};

/** Add the string `s` to `t`. */
static void gate_str(struct gate_text *t, const char *s)
{
    while (*s) {
        if (t->len < t->cap) {
            t->buf[t->len] = *s;
        }
        t->len++;
        s++;
    }
}

/** Add ` NAME N` to `t`, with `n` in decimal. */
static void gate_num(struct gate_text *t, const char *name, uint32_t n)
{
    char digits[12];
    int i = (int)sizeof(digits) - 1;

    digits[i] = '\0';
    do {
        digits[--i] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n != 0);
    gate_str(t, " ");
    gate_str(t, name);
    gate_str(t, " ");
    gate_str(t, digits + i);
}

/** The gate's state as one line: `pid P stop E waiting W` and the count of
 * each event by its name, E being `-` once the stop is used or released. */
static int32_t gate_show(void *data, char *buf, uint32_t cap)
{
    struct gate_text t;
    uint32_t flags = spin_lock_irqsave(&gate_lock);
    uint32_t count[WALK_GATE_EVENTS];
    int pid = gate_pid, stop = gate_stop, waiting = gate_waiting;
    int i;

    for (i = 0; i < WALK_GATE_EVENTS; i++) {
        count[i] = gate_count[i];
    }
    spin_unlock_irqrestore(&gate_lock, flags);
    (void)data;
    t.buf = buf;
    t.cap = cap;
    t.len = 0;
    gate_num(&t, "pid", (uint32_t)pid);
    gate_str(&t, " stop ");
    gate_str(&t, stop < 0 ? "-" : gate_names[stop]);
    gate_num(&t, "waiting", (uint32_t)waiting);
    for (i = 0; i < WALK_GATE_EVENTS; i++) {
        gate_num(&t, gate_names[i], count[i]);
    }
    gate_str(&t, "\n");
    return (int32_t)t.len;
}

/** Let a thread stopped at the gate go on, and stop no more: `off` clears
 * the gate as well, `release` keeps its counts. */
static void gate_let_go(int off)
{
    uint32_t flags = spin_lock_irqsave(&gate_lock);

    gate_released = 1;
    gate_stop = -1;
    if (off) {
        gate_pid = 0;
    }
    spin_unlock_irqrestore(&gate_lock, flags);
    wq_wake(&gate_wq, 0);
}

/** Whether the string `s` is the word `w`. */
static int gate_word(const char *s, const char *w)
{
    while (*w && *s == *w) {
        s++;
        w++;
    }
    return *s == '\0' && *w == '\0';
}

/**
 * A write to the gate: `arm PID EVENT` counts the events of the thread PID
 * from zero and stops it the first time it reaches EVENT, one of "full",
 * "unlocked" and "miss"; `release` lets it go on; `off` lets it go and stops
 * counting. `len`, or -EINVAL for another text, or -EFAULT.
 */
static int32_t gate_store(void *data, const char __user *ubuf, uint32_t len)
{
    char line[40];
    uint32_t n = len, i = 4, pid = 0;
    uint32_t flags;
    int ev;

    (void)data;
    if (len == 0 || len >= sizeof(line)) {
        return -EINVAL;
    }
    if (copy_from_user(line, ubuf, len) != 0) {
        return -EFAULT;
    }
    if (line[n - 1] == '\n') {
        n--;
    }
    line[n] = '\0';
    if (gate_word(line, "release")) {
        gate_let_go(0);
        return (int32_t)len;
    }
    if (gate_word(line, "off")) {
        gate_let_go(1);
        return (int32_t)len;
    }
    if (n < 4 || line[0] != 'a' || line[1] != 'r' || line[2] != 'm' || line[3] != ' ') {
        return -EINVAL;
    }
    while (i < n && line[i] >= '0' && line[i] <= '9' && pid < 100000000u) {
        pid = pid * 10u + (uint32_t)(line[i++] - '0');
    }
    if (pid == 0 || i >= n || line[i] != ' ') {
        return -EINVAL;
    }
    ev = 0;
    while (ev < WALK_GATE_EVENTS && !gate_word(line + i + 1, gate_names[ev])) {
        ev++;
    }
    if (ev == WALK_GATE_EVENTS) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&gate_lock);
    for (i = 0; i < WALK_GATE_EVENTS; i++) {
        gate_count[i] = 0;
    }
    gate_pid = (int)pid;
    gate_stop = ev;
    gate_released = 0;
    spin_unlock_irqrestore(&gate_lock, flags);
    return (int32_t)len;
}

static const struct debugfs_text_ops gate_ops = {gate_show, gate_store};

void walk_gate_init(void)
{
    if (debugfs_create_text("vfs/walk_gate", 0600u, &gate_ops, NULL) != 0) {
        klog(KLOG_WARN, "vfs", "debugfs: walk_gate not registered");
    }
}
