/* The walk sequence; the contract is in walkseq.h. */
#include "walkseq.h"

#include "bcache.h"
#include "dcache.h"
#include "debugfs.h"
#include "vfs.h"
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

/** Begin a change, for a thread or for the walk gate, which holds one for
 * a test (`gate_store`). */
static void seq_begin(void)
{
    uint32_t flags = spin_lock_irqsave(&seq_lock);

    seq_active++;
    seq_now++;
    spin_unlock_irqrestore(&seq_lock, flags);
}

/** End a change `seq_begin` began, and wake the waiters, each of which
 * tests whether the changes left under way are its own (`seq_calm`). */
static void seq_end(void)
{
    uint32_t flags = spin_lock_irqsave(&seq_lock);

    seq_active--;
    seq_now++;
    spin_unlock_irqrestore(&seq_lock, flags);
    wq_wake(&seq_wq, 0);
}

void vfs_seq_begin(void)
{
    seq_begin();
    if (current_proc) {
        current_proc->walk_tokens++;
        current_proc->walk_moves++;
    }
}

void vfs_seq_end(void)
{
    if (current_proc && current_proc->walk_tokens > 0) {
        current_proc->walk_tokens--;
        current_proc->walk_moves++;
    }
    seq_end();
}

int vfs_seq_mine(void)
{
    return current_proc && current_proc->walk_tokens > 0;
}

/** The changes under way the thread `p` has begun itself; 0 for none. */
static uint32_t own_tokens(const process_t *p)
{
    return p ? p->walk_tokens : 0;
}

/** The moves of the sequence the thread `p`'s own changes made; 0 for
 * none. */
static uint64_t own_moves(const process_t *p)
{
    return p ? p->walk_moves : 0;
}

int vfs_seq_read(uint64_t *seq)
{
    uint32_t mine = own_tokens(current_proc);
    uint64_t moves = own_moves(current_proc);
    uint32_t flags = spin_lock_irqsave(&seq_lock);
    int calm = seq_active == mine;

    *seq = seq_now - moves;
    spin_unlock_irqrestore(&seq_lock, flags);
    return calm;
}

int vfs_seq_still(uint64_t seq)
{
    uint32_t mine = own_tokens(current_proc);
    uint64_t moves = own_moves(current_proc);
    uint32_t flags = spin_lock_irqsave(&seq_lock);
    int still = seq_active == mine && seq_now - moves == seq;

    spin_unlock_irqrestore(&seq_lock, flags);
    return still;
}

int vfs_seq_begin_if(uint64_t seq)
{
    uint32_t mine = own_tokens(current_proc);
    uint64_t moves = own_moves(current_proc);
    uint32_t flags = spin_lock_irqsave(&seq_lock);
    int still = seq_active == mine && seq_now - moves == seq;

    if (still) {
        seq_active++;
        seq_now++;
    }
    spin_unlock_irqrestore(&seq_lock, flags);
    if (still && current_proc) {
        current_proc->walk_tokens++;
        current_proc->walk_moves++;
    }
    return still;
}

/** The wait condition of `vfs_seq_wait`: no change is under way but those
 * the waiting thread `arg` has begun itself, whose tokens are counted
 * there, since the condition may be tested by the thread that wakes it. */
static int seq_calm(void *arg)
{
    const process_t *p = (const process_t *)arg;
    uint32_t flags = spin_lock_irqsave(&seq_lock);
    int calm = seq_active == own_tokens(p);

    spin_unlock_irqrestore(&seq_lock, flags);
    return calm;
}

void vfs_seq_wait(uint64_t *seq)
{
    while (!vfs_seq_read(seq) && current_proc) {
        walk_gate(WALK_GATE_WAIT);
        wq_wait_event_nointr(&seq_wq, seq_calm, current_proc);
    }
}

void vfs_stamp_set(uint64_t seq, int on)
{
    if (current_proc) {
        current_proc->walk_stamp = seq;
        current_proc->walk_stamp_on = on;
    }
}

void vfs_stamp_get(struct vfs_stamp *st)
{
    st->seq = current_proc ? current_proc->walk_stamp : 0;
    st->on = current_proc ? current_proc->walk_stamp_on : 0;
}

int vfs_stamp_still(const struct vfs_stamp *st)
{
    if (!st->on || vfs_seq_still(st->seq)) {
        return 1;
    }
    walk_gate(WALK_GATE_RETRY);
    return 0;
}

int vfs_consume_still(void)
{
    return !current_proc || !current_proc->walk_consume ||
           vfs_stamp_still(current_proc->walk_consume);
}

int vfs_stamp_begin(const struct vfs_stamp *st)
{
    if (!st->on) {
        vfs_seq_begin();
        return 1;
    }
    if (vfs_seq_begin_if(st->seq)) {
        return 1;
    }
    walk_gate(WALK_GATE_RETRY);
    return 0;
}

/*
 * The walk gate, /sys/kernel/debug/vfs/walk_gate. The thread `gate_pid`
 * counts each WALK_GATE_ event it reaches, and the first time it reaches
 * `gate_stop` sleeps there until a write of `release`, so that a test can
 * change the tree at a known point of a walk and then let it go on; all
 * under `gate_lock`. `gate_stop` is -1 once that stop is used. The gate
 * also holds up to GATE_TOKENS changes open for a test (`gate_tokens`),
 * arms one fault of a type's hand-back for a thread (`gate_fault`), and
 * one failed read of the block cache for a thread (`gate_ioerr`).
 */
#define GATE_TOKENS 2

static struct spinlock gate_lock = SPINLOCK_INIT_NAMED("walkgate");
static int gate_pid;
static int gate_stop = -1;
static int gate_released;
static int gate_waiting;
static int gate_tokens;
static int gate_fault;
static int gate_ioerr;
static uint32_t gate_count[WALK_GATE_EVENTS];
static wait_queue_t gate_wq;

/* The events' names, in the order of their numbers. */
static const char *const gate_names[WALK_GATE_EVENTS] = {
    "full",  "unlocked", "miss", "lookup", "nslock", "restart", "handoff",
    "retry", "wait",     "load", "read",   "kept",   "link",    "effect"};

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

/** Whether the thread number `*armed` is the calling thread's, which this
 * disarms: the step of `walk_gate_fault` and `walk_gate_ioerr`. */
static int gate_once(int *armed)
{
    uint32_t flags;
    int hit;

    if (!*armed || !current_proc) {
        return 0;
    }
    flags = spin_lock_irqsave(&gate_lock);
    hit = *armed == (int)current_proc->pid;
    if (hit) {
        *armed = 0;
    }
    spin_unlock_irqrestore(&gate_lock, flags);
    return hit;
}

int walk_gate_fault(void)
{
    return gate_once(&gate_fault);
}

int walk_gate_ioerr(void)
{
    return gate_once(&gate_ioerr);
}

/** The block cache's check before a read from a device: the walk gate's
 * WALK_GATE_READ, and the failure `ioerr` arms (`walk_gate_ioerr`). */
static int gate_read_check(void)
{
    walk_gate(WALK_GATE_READ);
    return walk_gate_ioerr();
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

/** The gate's state as one line: `pid P stop E waiting W`, the count of
 * each event by its name, E being `-` once the stop is used or released,
 * `tokens T active A`, the changes the gate holds open and all those under
 * way, and `keep K`, where the armed thread's newest walk keep lies in the
 * KRFFS boot root (`vfs_walk_keep_place`). May sleep. */
static int32_t gate_show(void *data, char *buf, uint32_t cap)
{
    struct gate_text t;
    uint32_t flags = spin_lock_irqsave(&gate_lock);
    uint32_t count[WALK_GATE_EVENTS];
    int pid = gate_pid, stop = gate_stop, waiting = gate_waiting, tokens = gate_tokens;
    uint32_t active;
    int i;

    for (i = 0; i < WALK_GATE_EVENTS; i++) {
        count[i] = gate_count[i];
    }
    spin_unlock_irqrestore(&gate_lock, flags);
    flags = spin_lock_irqsave(&seq_lock);
    active = seq_active;
    spin_unlock_irqrestore(&seq_lock, flags);
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
    gate_num(&t, "tokens", (uint32_t)tokens);
    gate_num(&t, "active", active);
    gate_num(&t, "keep", pid > 0 ? vfs_walk_keep_place((uint32_t)pid) : 0u);
    gate_str(&t, "\n");
    return (int32_t)t.len;
}

/**
 * Begin a change the gate holds open, with `open` set, or end the last it
 * holds: 0, or -EBUSY past GATE_TOKENS and -ENOENT with none to end. A
 * test holds changes open so to show that a walk takes no snapshot while
 * one is under way, nor while one of two overlapping is.
 */
static int gate_token(int open)
{
    uint32_t flags = spin_lock_irqsave(&gate_lock);
    int ok = open ? gate_tokens < GATE_TOKENS : gate_tokens > 0;

    if (ok) {
        gate_tokens += open ? 1 : -1;
    }
    spin_unlock_irqrestore(&gate_lock, flags);
    if (!ok) {
        return open ? -EBUSY : -ENOENT;
    }
    if (open) {
        seq_begin();
    } else {
        seq_end();
    }
    return 0;
}

/**
 * Move the sequence by 2^32 with no change under way, for a test that a
 * stamp read before compares unequal, as it would not in 32 bits: the
 * sequence only grows, so this takes no value back.
 */
static void gate_advance(void)
{
    uint32_t flags = spin_lock_irqsave(&seq_lock);

    seq_now += (uint64_t)0x10000u * 0x10000u;
    spin_unlock_irqrestore(&seq_lock, flags);
}

/** Let a thread stopped at the gate go on, and stop no more: `off` clears
 * the gate as well, its fault and the changes it holds open, `release`
 * keeps its counts. */
static void gate_let_go(int off)
{
    uint32_t flags = spin_lock_irqsave(&gate_lock);

    gate_released = 1;
    gate_stop = -1;
    if (off) {
        gate_pid = 0;
        gate_fault = 0;
        gate_ioerr = 0;
    }
    spin_unlock_irqrestore(&gate_lock, flags);
    wq_wake(&gate_wq, 0);
    while (off && gate_token(0) == 0) {
    }
}

/** The thread number in `line` from `*i` on, and `*i` past it; 0 for
 * none. */
static uint32_t gate_pid_at(const char *line, uint32_t *i)
{
    uint32_t pid = 0;

    while (line[*i] >= '0' && line[*i] <= '9' && pid < 100000000u) {
        pid = pid * 10u + (uint32_t)(line[(*i)++] - '0');
    }
    return pid;
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

/** Whether `line` begins with the word `w` and a space. */
static int gate_verb(const char *line, const char *w)
{
    while (*w && *line == *w) {
        line++;
        w++;
    }
    return *w == '\0' && *line == ' ';
}

/** `fault PID` or `ioerr PID` of `line`, `n` bytes, into `*armed`: 0, or
 * -EINVAL. */
static int gate_once_text(const char *line, uint32_t n, int *armed)
{
    uint32_t i = 6, pid = gate_pid_at(line, &i);
    uint32_t flags;

    if (pid == 0 || i != n) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&gate_lock);
    *armed = (int)pid;
    spin_unlock_irqrestore(&gate_lock, flags);
    return 0;
}

/**
 * Leave the next walk of the root mount to read the disk: the dirty blocks
 * of every mount written (`vfs_sync_all`), the root mount's names forgotten,
 * which puts back the nodes only they held, and every clean block the cache
 * holds for no one given back. A test's way to make a lookup cold, as
 * Linux's `echo 3 > /proc/sys/vm/drop_caches` does. May sleep.
 */
static void gate_cold(void)
{
    struct vfs_mount *root = vfs_root_mount();
    uint32_t written = 0;

    (void)vfs_sync_all();
    if (root) {
        dcache_forget_mount(root);
    }
    (void)bcache_shrink(0xFFFFFFFFu, 0, &written);
}

/** `arm PID EVENT` of `line`, `n` bytes: 0, or -EINVAL. */
static int gate_arm_text(const char *line, uint32_t n)
{
    uint32_t i = 4, pid = gate_pid_at(line, &i);
    uint32_t flags;
    int ev = 0;

    if (pid == 0 || i >= n || line[i] != ' ') {
        return -EINVAL;
    }
    while (ev < WALK_GATE_EVENTS && !gate_word(line + i + 1, gate_names[ev])) {
        ev++;
    }
    if (ev == WALK_GATE_EVENTS && gate_word(line + i + 1, "count")) {
        ev = -1;
    } else if (ev == WALK_GATE_EVENTS) {
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
    return 0;
}

/**
 * A write to the gate: `arm PID EVENT` counts the events of the thread PID
 * from zero and stops it the first time it reaches EVENT, one of the names
 * of `gate_names`, or nowhere for "count"; `release` lets it go on; `off`
 * lets it go, stops counting and ends the changes the gate holds; `begin`
 * and `end` begin and end a change the gate holds (`gate_token`);
 * `advance` moves the sequence by 2^32 (`gate_advance`); `fault PID` arms
 * one fault of a hand-back for the thread PID (`walk_gate_fault`); `ioerr
 * PID` one failed read of the block cache for it (`walk_gate_ioerr`); and
 * `cold` leaves the next walk to read the disk (`gate_cold`). `len`, or
 * -EINVAL for another text, `gate_token`'s error, or -EFAULT.
 */
static int32_t gate_store(void *data, const char __user *ubuf, uint32_t len)
{
    char line[40];
    uint32_t n = len;
    int rc = -EINVAL;

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
    if (gate_word(line, "release") || gate_word(line, "off")) {
        gate_let_go(line[0] == 'o');
        rc = 0;
    } else if (gate_word(line, "begin") || gate_word(line, "end")) {
        rc = gate_token(line[0] == 'b');
    } else if (gate_word(line, "advance")) {
        gate_advance();
        rc = 0;
    } else if (gate_word(line, "cold")) {
        gate_cold();
        rc = 0;
    } else if (gate_verb(line, "fault")) {
        rc = gate_once_text(line, n, &gate_fault);
    } else if (gate_verb(line, "ioerr")) {
        rc = gate_once_text(line, n, &gate_ioerr);
    } else if (gate_verb(line, "arm")) {
        rc = gate_arm_text(line, n);
    }
    return rc != 0 ? rc : (int32_t)len;
}

static const struct debugfs_text_ops gate_ops = {gate_show, gate_store};

void walk_gate_init(void)
{
    if (debugfs_create_text("vfs/walk_gate", 0600u, &gate_ops, NULL) != 0) {
        klog(KLOG_WARN, "vfs", "debugfs: walk_gate not registered");
        return;
    }
    bcache_set_read_check(gate_read_check);
}
