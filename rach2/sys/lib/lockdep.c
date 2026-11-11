/* The lock-order checker of the debug build, after Linux's lockdep. */
#include "lockdep.h"

#ifdef RACH2_LOCKDEP

#include "kirq.h"
#include "spinlock.h"
#include "../arch/arch.h"
#include "../klog.h"
#include "../panic.h"
#include "../mm/pressure.h"
#include "../proc/percpu.h"
#include "../proc/process.h"
#include "../proc/smp.h"

/*
 * Linux 6.12.115's kernel/locking/lockdep.c is the model, and its functions
 * are named where this follows them. The pools are static, in .bss, with
 * the sizes below; when one is full the checker stops and says so once,
 * and /proc/lockdep_stats says which (`ld_stop`).
 */
#define LOCKDEP_CLASSES 1024  /* lock classes, as MAX_LOCKDEP_KEYS */
#define LOCKDEP_EDGES   8192  /* orders between two classes, as MAX_LOCKDEP_ENTRIES */
#define LOCKDEP_CHAINS  16384 /* chains checked, a power of two, as MAX_LOCKDEP_CHAINS */
#define LOCKDEP_ONCE    512   /* reports remembered, so each is made once */
#define LOCKDEP_HASH    256   /* class hash buckets, a power of two */
#define LOCKDEP_PATH    8     /* orders a report prints of one path */
#define LOCKDEP_QUEUE   4     /* reports one hook may make */

/* No class, no edge. */
#define LD_NONE 0xffffu

/* A class's usage: taken in an interrupt, and held with interrupts on
 * (LOCK_USED_IN_HARDIRQ and LOCK_ENABLED_HARDIRQ). */
#define LD_USED_IRQ 1u
#define LD_ON_IRQ   2u

/* Why the checker stopped, 0 while it checks. */
#define LD_OFF_CLASSES 1
#define LD_OFF_EDGES   2
#define LD_OFF_CHAINS  3
#define LD_OFF_DEPTH   4

/* The kinds of report. */
#define LD_CYCLE        1  /* a new order closes a cycle (check_noncircular) */
#define LD_RECURSION    2  /* a class taken twice (check_deadlock) */
#define LD_INCONSISTENT 3  /* one class in an interrupt and with interrupts on */
#define LD_INVERSION    4  /* such classes joined by orders (check_irq_usage) */
#define LD_SLEEP_SPIN   5  /* a sleep or sleeping lock under a spinlock */
#define LD_SLEEP_IRQ    6  /* a sleep or sleeping lock in an interrupt */
#define LD_SLEEP_OFF    7  /* a sleep or sleeping lock with interrupts off */
#define LD_BKL_SPIN     8  /* the big lock under a spinlock */
#define LD_NO_CLASS     9  /* a lock never initialised */
#define LD_BAD_RELEASE  10 /* a lock let go that the context does not hold */
#define LD_NOT_HELD     11 /* lockdep_assert_held failed */
#define LD_OFF          12 /* a pool is full: the checker stops */

/* A class: the locks of one init site, or one static lock, at one
 * subclass (struct lock_class). */
struct ld_class {
    const void *key;      /* the init site's key, or the lock's address */
    const char *name;     /* or NULL, for a class printed by its key */
    uintptr_t first_ip;   /* where a lock of it was first taken */
    uintptr_t irq_ip;     /* where first taken in an interrupt */
    uintptr_t on_ip;      /* where first held with interrupts on */
    unsigned short hnext; /* the next class in its hash bucket, or LD_NONE */
    unsigned short fwd;   /* the first order from it, or LD_NONE */
    unsigned short bwd;   /* the first order to it, or LD_NONE */
    unsigned char subclass;
    unsigned char usage; /* LD_USED_IRQ, LD_ON_IRQ */
};

/* An order: `from` was held when `to` was taken (struct lock_list). */
struct ld_edge {
    unsigned short from, to;
    unsigned short fnext; /* the next order from `from`, or LD_NONE */
    unsigned short bnext; /* the next order to `to`, or LD_NONE */
    uintptr_t from_ip;    /* where `from` was taken */
    uintptr_t to_ip;      /* where `to` was then taken */
};

/* A report, filled under the graph's lock and printed after it is let go,
 * since klog's report lock comes before it in the order. Classes and orders
 * are never freed or changed once made, so the indices stay good. */
struct ld_report {
    int kind;
    unsigned short a, b; /* the classes it names, or LD_NONE */
    uintptr_t ip_a, ip_b;
    uintptr_t addr; /* the lock of LD_NO_CLASS, the reason of LD_OFF */
    int npath;
    int cut; /* the path had more orders than `path` holds */
    unsigned short path[LOCKDEP_PATH];
};

/* The reports one processor's hook has made and not yet printed. */
struct ld_queue {
    int n;
    struct ld_report r[LOCKDEP_QUEUE];
};

/* The graph's lock, taken without the checker (`lockdep_lock`). Classes,
 * orders, chains and the reports remembered are written under it. */
static struct spinlock graph_lock = SPINLOCK_INIT_NAMED("lockdep_graph");

static struct ld_class classes[LOCKDEP_CLASSES];
static volatile unsigned int nclasses;
static unsigned short class_hash[LOCKDEP_HASH];
static int class_hash_ready;

static struct ld_edge edges[LOCKDEP_EDGES];
static unsigned int nedges;

/* The chains checked, each two hashes of its classes in order, the second
 * never 0; a slot with both 0 is empty. Read without the lock: a slot is
 * written second hash first. */
static volatile uint32_t chains[LOCKDEP_CHAINS][2];
static unsigned int nchains;

/* The reports made, by kind and the two things they name. */
static struct {
    int kind;
    uintptr_t a, b;
} once[LOCKDEP_ONCE];
static unsigned int nonce;
static unsigned int reports, reports_lost;

/* The breadth-first search's marks, the order it reached each class by,
 * and its queue (`__bfs`). */
static unsigned int bfs_gen;
static unsigned int bfs_mark[LOCKDEP_CLASSES];
static unsigned short bfs_parent[LOCKDEP_CLASSES];
static unsigned short bfs_queue[LOCKDEP_CLASSES];

static volatile int ld_off;

static struct ld_queue queues[NR_CPUS];

/* Each processor's list of the locks its context holds, for a check. */
static struct lockdep_held_lock *lists[NR_CPUS][2 * LOCKDEP_DEPTH];

/* Each processor's line of a report or of /proc text. */
static char lines[NR_CPUS][KLOG_RECORD_MAX];

/* ---- text ---- */

/* A buffer text is appended to, cut at its capacity. */
struct ld_text {
    char *buf;
    int cap; /* bytes, the NUL included */
    int len;
};

/** Append `c` to `t`, unless it is full. */
static void put_ch(struct ld_text *t, char c)
{
    if (t->len + 1 < t->cap) {
        t->buf[t->len++] = c;
        t->buf[t->len] = '\0';
    }
}

/** Append the string `s` to `t`. */
static void put_str(struct ld_text *t, const char *s)
{
    while (*s) {
        put_ch(t, *s++);
    }
}

/** Append `v` in decimal to `t`. */
static void put_uint(struct ld_text *t, unsigned long v)
{
    char d[24];
    int n = 0;

    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    while (n > 0) {
        put_ch(t, d[--n]);
    }
}

/** Append `v` in hexadecimal, after `0x`, to `t`. */
static void put_hex(struct ld_text *t, uintptr_t v)
{
    char d[2 * sizeof(uintptr_t)];
    int n = 0;

    do {
        d[n++] = "0123456789abcdef"[v & 0xfu];
        v >>= 4;
    } while (v != 0);
    put_str(t, "0x");
    while (n > 0) {
        put_ch(t, d[--n]);
    }
}

/** Append class `c`'s name, with its subclass after a slash when not 0. */
static void put_class(struct ld_text *t, unsigned int c)
{
    if (c == LD_NONE) {
        put_str(t, "?");
        return;
    }
    if (classes[c].name) {
        put_str(t, classes[c].name);
    } else {
        put_str(t, "lock@");
        put_hex(t, (uintptr_t)classes[c].key);
    }
    if (classes[c].subclass != 0) {
        put_ch(t, '/');
        put_uint(t, classes[c].subclass);
    }
}

/** Append " at IP". */
static void put_at(struct ld_text *t, uintptr_t ip)
{
    put_str(t, " at ");
    put_hex(t, ip);
}

/** Start this processor's line. */
static struct ld_text line_start(void)
{
    struct ld_text t;

    t.buf = lines[arch_cpu_id()];
    t.cap = KLOG_RECORD_MAX - 16;
    t.len = 0;
    t.buf[0] = '\0';
    return t;
}

/* ---- the context ---- */

/** The sleeping locks the running context holds: its thread's, or before
 * the first process its processor's. */
static struct lockdep_held *sleep_stack(struct percpu *cpu)
{
    return cpu->current ? &cpu->current->lockdep : &cpu->lockdep_sleep;
}

/**
 * Fill `out` with the locks of the running context that a lock taken the
 * way `how` says follows, outermost first, and return how many. A sleeping
 * lock follows its thread's sleeping locks. A spinlock follows the
 * spinlocks its processor took in the same context, in an interrupt or
 * out, and outside one also the thread's sleeping locks, which come
 * before any spinlock (the irq_context of check_prevs_add, lockdep.c:3304).
 */
static int ld_context(struct percpu *cpu, int how, int irq, struct lockdep_held_lock **out)
{
    struct lockdep_held *sp = &cpu->lockdep_spin, *s = sleep_stack(cpu);
    unsigned int i, j;
    int n = 0;

    if (how == LOCKDEP_SLEEPING) {
        for (j = 0; j < s->depth; j++) {
            out[n++] = &s->locks[j];
        }
        return n;
    }
    i = sp->depth;
    while (i > 0 && ((sp->locks[i - 1].flags & LOCKDEP_HELD_IRQ) != 0) == (irq != 0)) {
        i--;
    }
    if (i == 0 && !irq) {
        for (j = 0; j < s->depth; j++) {
            out[n++] = &s->locks[j];
        }
    }
    for (j = i; j < sp->depth; j++) {
        out[n++] = &sp->locks[j];
    }
    return n;
}

/* ---- reports ---- */

/** 1 when the report of `kind` naming `a` and `b` is new, which it then
 * remembers; 0 when made before, or when no more can be remembered, in
 * which case it is counted as lost. Under the graph's lock. */
static int ld_once(int kind, uintptr_t a, uintptr_t b)
{
    unsigned int i;

    for (i = 0; i < nonce; i++) {
        if (once[i].kind == kind && once[i].a == a && once[i].b == b) {
            return 0;
        }
    }
    if (nonce == LOCKDEP_ONCE) {
        reports_lost++;
        return 0;
    }
    once[nonce].kind = kind;
    once[nonce].a = a;
    once[nonce].b = b;
    nonce++;
    reports++;
    return 1;
}

/** A blank report of `kind` in this processor's queue, or NULL when the
 * queue is full; `ld_commit` keeps it. */
static struct ld_report *ld_new(int kind)
{
    struct ld_queue *q = &queues[arch_cpu_id()];
    struct ld_report *r;

    if (q->n == LOCKDEP_QUEUE) {
        reports_lost++;
        return 0;
    }
    r = &q->r[q->n];
    r->kind = kind;
    r->a = r->b = LD_NONE;
    r->ip_a = r->ip_b = 0;
    r->addr = 0;
    r->npath = 0;
    r->cut = 0;
    return r;
}

/** Keep the report `ld_new` gave. */
static void ld_commit(void)
{
    queues[arch_cpu_id()].n++;
}

/** Queue the report of `kind` naming classes `a` and `b` and the sites
 * `ip_a` and `ip_b`; the caller has asked `ld_once`. Under the graph's
 * lock. */
static void ld_report2(int kind, unsigned int a, unsigned int b, uintptr_t ip_a, uintptr_t ip_b)
{
    struct ld_report *r = ld_new(kind);

    if (!r) {
        return;
    }
    r->a = (unsigned short)a;
    r->b = (unsigned short)b;
    r->ip_a = ip_a;
    r->ip_b = ip_b;
    ld_commit();
}

/** Add order `e` to the path of report `r`. */
static void ld_path_add(struct ld_report *r, unsigned int e)
{
    if (r->npath < LOCKDEP_PATH) {
        r->path[r->npath++] = (unsigned short)e;
    } else {
        r->cut = 1;
    }
}

/** Stop checking for `why`, once, with a report. */
static void ld_stop(int why)
{
    struct ld_report *r;

    if (ld_off) {
        return;
    }
    ld_off = why;
    r = ld_new(LD_OFF);
    if (r) {
        r->addr = (uintptr_t)why;
        ld_commit();
    }
}

/* ---- classes ---- */

/* The kernel's static data, from the link script. */
extern char _data[], _image_end[];

/** Whether `p` lies in the kernel's static data, where a lock that was not
 * initialised is keyed by its address (`static_obj`, lockdep.c:824). */
static int ld_static(const void *p)
{
    return (const char *)p >= _data && (const char *)p < _image_end;
}

/** The bucket of the class keyed by `key` at `subclass`. */
static unsigned int class_bucket(const void *key, unsigned int subclass)
{
    uintptr_t k = (uintptr_t)key;

    return (unsigned int)((k >> 3) ^ (k >> 12) ^ (subclass * 0x9e37u)) & (LOCKDEP_HASH - 1u);
}

/**
 * The class of the lock of `map` at `subclass`, made when new, or LD_NONE
 * when the pool is full (`register_lock_class`, lockdep.c:1276). A map
 * that was never initialised is reported once and keyed by its address.
 * Under the graph's lock.
 */
static unsigned int ld_class_of(struct lockdep_map *map, unsigned int subclass, uintptr_t ip)
{
    const void *key;
    unsigned int b, c;
    struct ld_report *r;

    if (subclass == 0 && map->cls != 0) {
        return map->cls - 1u;
    }
    if (!class_hash_ready) {
        for (b = 0; b < LOCKDEP_HASH; b++) {
            class_hash[b] = LD_NONE;
        }
        class_hash_ready = 1;
    }
    if ((map->flags & LOCKDEP_STATIC) || (!map->key && ld_static(map))) {
        key = map;
    } else if (map->key) {
        key = map->key;
    } else {
        key = map;
        map->flags |= LOCKDEP_STATIC;
        if (ld_once(LD_NO_CLASS, (uintptr_t)map, 0) && (r = ld_new(LD_NO_CLASS)) != 0) {
            r->addr = (uintptr_t)map;
            r->ip_a = ip;
            ld_commit();
        }
    }
    b = class_bucket(key, subclass);
    for (c = class_hash[b]; c != LD_NONE; c = classes[c].hnext) {
        if (classes[c].key == key && classes[c].subclass == subclass) {
            break;
        }
    }
    if (c == LD_NONE) {
        if (nclasses == LOCKDEP_CLASSES || subclass > 255u) {
            ld_stop(LD_OFF_CLASSES);
            return LD_NONE;
        }
        c = nclasses;
        classes[c].key = key;
        classes[c].name = map->name;
        classes[c].first_ip = ip;
        classes[c].irq_ip = classes[c].on_ip = 0;
        classes[c].fwd = classes[c].bwd = LD_NONE;
        classes[c].subclass = (unsigned char)subclass;
        classes[c].usage = 0;
        classes[c].hnext = class_hash[b];
        class_hash[b] = (unsigned short)c;
        k_barrier();
        nclasses = c + 1u;
    }
    if (subclass == 0) {
        k_barrier();
        map->cls = (unsigned short)(c + 1u);
    }
    return c;
}

/* ---- the search ---- */

/* What a search looks for: a class, or a usage bit. */
#define LD_SEEK_CLASS 0
#define LD_SEEK_USAGE 1

/**
 * Search the orders from `start` (`forward`) or to it, breadth first, for
 * the class `want` (LD_SEEK_CLASS) or a class with usage bit `want`
 * (LD_SEEK_USAGE), `start` included; the class found, or LD_NONE. Each
 * class reached keeps in `bfs_parent` the order it was reached by
 * (`__bfs`, lockdep.c:1705). Under the graph's lock.
 */
static unsigned int ld_bfs(unsigned int start, int forward, int seek, unsigned int want)
{
    unsigned int head = 0, tail = 0, c, e, next;

    if (++bfs_gen == 0) {
        for (c = 0; c < LOCKDEP_CLASSES; c++) {
            bfs_mark[c] = 0;
        }
        bfs_gen = 1;
    }
    bfs_mark[start] = bfs_gen;
    bfs_parent[start] = LD_NONE;
    bfs_queue[tail++] = (unsigned short)start;
    while (head < tail) {
        c = bfs_queue[head++];
        if (seek == LD_SEEK_CLASS ? c == want : (classes[c].usage & want) != 0) {
            return c;
        }
        for (e = forward ? classes[c].fwd : classes[c].bwd; e != LD_NONE;
             e = forward ? edges[e].fnext : edges[e].bnext) {
            next = forward ? edges[e].to : edges[e].from;
            if (bfs_mark[next] != bfs_gen) {
                bfs_mark[next] = bfs_gen;
                bfs_parent[next] = (unsigned short)e;
                bfs_queue[tail++] = (unsigned short)next;
            }
        }
    }
    return LD_NONE;
}

/** Add to `r` the path of orders from `start` to `found`, which a forward
 * search from `start` reached. */
static void ld_path_forward(struct ld_report *r, unsigned int start, unsigned int found)
{
    unsigned int n = 0, c = found, i;

    while (c != start) {
        bfs_queue[n++] = bfs_parent[c];
        c = edges[bfs_parent[c]].from;
    }
    for (i = n; i > 0; i--) {
        ld_path_add(r, bfs_queue[i - 1]);
    }
}

/** Add to `r` the path of orders from `found` to `start`, which a backward
 * search from `start` reached. */
static void ld_path_backward(struct ld_report *r, unsigned int start, unsigned int found)
{
    unsigned int c = found;

    while (c != start) {
        ld_path_add(r, bfs_parent[c]);
        c = edges[bfs_parent[c]].to;
    }
}

/* ---- the checks ---- */

/**
 * Note that class `c` has usage `bit`, first at `ip`, and report a class
 * taken both in an interrupt and with interrupts on (`mark_lock_irq`,
 * `valid_state`, lockdep.c:4024), or an order that joins a class taken in
 * an interrupt to one taken with interrupts on (`check_usage_forwards` and
 * `check_usage_backwards`, lockdep.c:3966-4021). Under the graph's lock.
 */
static void ld_mark(unsigned int c, unsigned int bit, uintptr_t ip)
{
    struct ld_report *r;
    unsigned int other;

    if (classes[c].usage & bit) {
        return;
    }
    classes[c].usage |= (unsigned char)bit;
    if (bit == LD_USED_IRQ) {
        classes[c].irq_ip = ip;
    } else {
        classes[c].on_ip = ip;
    }
    if ((classes[c].usage & (LD_USED_IRQ | LD_ON_IRQ)) == (LD_USED_IRQ | LD_ON_IRQ)) {
        if (ld_once(LD_INCONSISTENT, c, c)) {
            ld_report2(LD_INCONSISTENT, c, c, classes[c].irq_ip, classes[c].on_ip);
        }
        return;
    }
    if (bit == LD_USED_IRQ) {
        other = ld_bfs(c, 1, LD_SEEK_USAGE, LD_ON_IRQ);
        if (other == LD_NONE || !ld_once(LD_INVERSION, c, other) || !(r = ld_new(LD_INVERSION))) {
            return;
        }
        r->a = (unsigned short)c;
        r->b = (unsigned short)other;
        ld_path_forward(r, c, other);
    } else {
        other = ld_bfs(c, 0, LD_SEEK_USAGE, LD_USED_IRQ);
        if (other == LD_NONE || !ld_once(LD_INVERSION, other, c) || !(r = ld_new(LD_INVERSION))) {
            return;
        }
        r->a = (unsigned short)other;
        r->b = (unsigned short)c;
        ld_path_backward(r, c, other);
    }
    r->ip_a = classes[r->a].irq_ip;
    r->ip_b = classes[r->b].on_ip;
    ld_commit();
}

/**
 * Add the order `p`, taken at `pip`, before `n`, taken at `nip`, unless
 * it closes a cycle, which it reports (`check_prev_add`, lockdep.c:3146,
 * and `check_noncircular`, :2208); then report an interrupt inversion the
 * order makes (`check_irq_usage`, :2839). 0 for a cycle or a full pool,
 * which end the context's checks, else 1. Under the graph's lock.
 */
static int ld_add_edge(unsigned int p, uintptr_t pip, unsigned int n, uintptr_t nip)
{
    struct ld_report *r;
    unsigned int e, irq_class, on_class;

    for (e = classes[p].fwd; e != LD_NONE; e = edges[e].fnext) {
        if (edges[e].to == n) {
            return 1;
        }
    }
    if (ld_bfs(n, 1, LD_SEEK_CLASS, p) != LD_NONE) {
        if (ld_once(LD_CYCLE, p, n) && (r = ld_new(LD_CYCLE)) != 0) {
            r->a = (unsigned short)n;
            r->b = (unsigned short)p;
            r->ip_a = nip;
            r->ip_b = pip;
            ld_path_forward(r, n, p);
            ld_commit();
        }
        return 0;
    }
    if (nedges == LOCKDEP_EDGES) {
        ld_stop(LD_OFF_EDGES);
        return 0;
    }
    e = nedges;
    edges[e].from = (unsigned short)p;
    edges[e].to = (unsigned short)n;
    edges[e].from_ip = pip;
    edges[e].to_ip = nip;
    edges[e].fnext = classes[p].fwd;
    edges[e].bnext = classes[n].bwd;
    classes[p].fwd = (unsigned short)e;
    classes[n].bwd = (unsigned short)e;
    nedges = e + 1u;

    irq_class = ld_bfs(p, 0, LD_SEEK_USAGE, LD_USED_IRQ);
    if (irq_class == LD_NONE || !(r = ld_new(LD_INVERSION))) {
        return 1;
    }
    ld_path_backward(r, p, irq_class);
    ld_path_add(r, e);
    on_class = ld_bfs(n, 1, LD_SEEK_USAGE, LD_ON_IRQ);
    if (on_class == LD_NONE || !ld_once(LD_INVERSION, irq_class, on_class)) {
        return 1; /* the report is not committed */
    }
    ld_path_forward(r, n, on_class);
    r->a = (unsigned short)irq_class;
    r->b = (unsigned short)on_class;
    r->ip_a = classes[irq_class].irq_ip;
    r->ip_b = classes[on_class].on_ip;
    ld_commit();
    return 1;
}

/** The two hashes of the chain of the `n` locks in `list` and then class
 * `c`, in a context in an interrupt or not (`iterate_chain_key`). */
static void ld_chain_key(struct lockdep_held_lock **list, int n, unsigned int c, int irq,
                         uint32_t *h1, uint32_t *h2)
{
    uint32_t a = 1u + (uint32_t)(irq != 0), b = 2166136261u;
    int i;

    for (i = 0; i <= n; i++) {
        uint32_t v = (i < n ? list[i]->cls : c) + 1u;

        a = a * 31u + v;
        b = (b ^ v) * 16777619u;
    }
    *h1 = a;
    *h2 = b | 1u;
}

/** 1 when the chain of hashes `h1` and `h2` was checked before; read
 * without the lock, so a chain added at the same moment may read as new. */
static int ld_chain_find(uint32_t h1, uint32_t h2)
{
    unsigned int i = h1 & (LOCKDEP_CHAINS - 1u);

    while (chains[i][1] != 0u) {
        if (chains[i][0] == h1 && chains[i][1] == h2) {
            return 1;
        }
        i = (i + 1u) & (LOCKDEP_CHAINS - 1u);
    }
    return 0;
}

/** Remember the chain `h1` and `h2`; 0 when the table is three quarters
 * full, which stops the checker. Under the graph's lock. */
static int ld_chain_add(uint32_t h1, uint32_t h2)
{
    unsigned int i = h1 & (LOCKDEP_CHAINS - 1u);

    if (nchains >= LOCKDEP_CHAINS / 4u * 3u) {
        ld_stop(LD_OFF_CHAINS);
        return 0;
    }
    while (chains[i][1] != 0u) {
        i = (i + 1u) & (LOCKDEP_CHAINS - 1u);
    }
    chains[i][0] = h1;
    k_barrier();
    chains[i][1] = h2;
    nchains++;
    return 1;
}

/**
 * Check class `c`, taken at `ip`, against the `n` locks the context holds
 * in `list`: a class taken twice (`check_deadlock`, lockdep.c:3081), then
 * the order from each lock down to the first not taken by a trylock
 * (`check_prevs_add`, :3285). Under the graph's lock.
 */
static void ld_validate(struct lockdep_held_lock **list, int n, unsigned int c, uintptr_t ip)
{
    int i;

    for (i = 0; i < n; i++) {
        if (list[i]->cls == c) {
            if (ld_once(LD_RECURSION, c, c)) {
                ld_report2(LD_RECURSION, c, c, ip, list[i]->ip);
            }
            return;
        }
    }
    for (i = n - 1; i >= 0; i--) {
        if (!ld_add_edge(list[i]->cls, list[i]->ip, c, ip)) {
            return;
        }
        if (!(list[i]->flags & LOCKDEP_HELD_TRY)) {
            break;
        }
    }
}

/**
 * Report a sleep at `ip`, or the sleeping lock of class `c` taken there,
 * under a spinlock, in an interrupt, or with interrupts off, which `on`
 * says were on or not (`__might_sleep`, kernel/sched/core.c:8577, and the
 * wait-type check, lockdep.c:4793). Under the graph's lock.
 */
static void ld_check_sleep(struct percpu *cpu, unsigned int c, uintptr_t ip, int on, int irq)
{
    struct lockdep_held *sp = &cpu->lockdep_spin;
    uintptr_t what = c == LD_NONE ? ip : (uintptr_t)c;

    if (sp->depth != 0) {
        struct lockdep_held_lock *top = &sp->locks[sp->depth - 1];

        if (ld_once(LD_SLEEP_SPIN, top->cls, what)) {
            ld_report2(LD_SLEEP_SPIN, c, top->cls, ip, top->ip);
        }
    } else if (irq) {
        if (ld_once(LD_SLEEP_IRQ, what, 0)) {
            ld_report2(LD_SLEEP_IRQ, c, LD_NONE, ip, 0);
        }
    } else if (!on) {
        if (ld_once(LD_SLEEP_OFF, what, 0)) {
            ld_report2(LD_SLEEP_OFF, c, LD_NONE, ip, 0);
        }
    }
}

/* ---- printing ---- */

/** Print the locks the running context holds, outermost first. */
static void ld_print_held(int how, struct percpu *cpu)
{
    struct lockdep_held_lock **list = lists[arch_cpu_id()];
    int i, n = ld_context(cpu, LOCKDEP_SPIN, in_interrupt(), list);
    struct ld_text t;

    for (i = 0; i < n; i++) {
        t = line_start();
        put_str(&t, "held: ");
        put_class(&t, list[i]->cls);
        put_at(&t, list[i]->ip);
        klog_report(how, KLOG_ERR, "lockdep", "%s", t.buf);
    }
}

/** Print report `r`'s path of orders, one line each. */
static void ld_print_path(int how, const struct ld_report *r)
{
    struct ld_text t;
    int i;

    for (i = 0; i < r->npath; i++) {
        const struct ld_edge *e = &edges[r->path[i]];

        t = line_start();
        put_str(&t, "order: ");
        put_class(&t, e->from);
        put_at(&t, e->from_ip);
        put_str(&t, ", then ");
        put_class(&t, e->to);
        put_at(&t, e->to_ip);
        klog_report(how, KLOG_ERR, "lockdep", "%s", t.buf);
    }
    if (r->cut) {
        klog_report(how, KLOG_ERR, "lockdep", "order: more orders not shown");
    }
}

/** The reason `why` the checker stopped, as /proc/lockdep_stats and the
 * report say it. */
static const char *ld_off_reason(int why)
{
    switch (why) {
    case LD_OFF_CLASSES:
        return "the class pool is full";
    case LD_OFF_EDGES:
        return "the order pool is full";
    case LD_OFF_CHAINS:
        return "the chain table is full";
    case LD_OFF_DEPTH:
        return "a context holds too many locks";
    default:
        return "on";
    }
}

/** Print report `r` through klog's report path `how`. */
static void ld_print_one(int how, struct percpu *cpu, const struct ld_report *r)
{
    struct ld_text t = line_start();
    int held = 1;

    switch (r->kind) {
    case LD_CYCLE:
        put_str(&t, "possible deadlock: ");
        put_class(&t, r->a);
        put_at(&t, r->ip_a);
        put_str(&t, " under ");
        put_class(&t, r->b);
        put_at(&t, r->ip_b);
        put_str(&t, ", which is taken under it");
        break;
    case LD_RECURSION:
        put_str(&t, "recursive lock: ");
        put_class(&t, r->a);
        put_at(&t, r->ip_a);
        put_str(&t, ", already held");
        put_at(&t, r->ip_b);
        break;
    case LD_INCONSISTENT:
        put_str(&t, "lock taken in an interrupt and with interrupts on: ");
        put_class(&t, r->a);
        put_str(&t, " in an interrupt");
        put_at(&t, r->ip_a);
        put_str(&t, ", with interrupts on");
        put_at(&t, r->ip_b);
        held = 0;
        break;
    case LD_INVERSION:
        put_str(&t, "interrupt inversion: ");
        put_class(&t, r->a);
        put_str(&t, ", taken in an interrupt");
        put_at(&t, r->ip_a);
        put_str(&t, ", is taken before ");
        put_class(&t, r->b);
        put_str(&t, ", held with interrupts on");
        put_at(&t, r->ip_b);
        held = 0;
        break;
    case LD_SLEEP_SPIN:
    case LD_SLEEP_IRQ:
    case LD_SLEEP_OFF:
        if (r->a == LD_NONE) {
            put_str(&t, "sleep");
        } else {
            put_str(&t, "sleeping lock ");
            put_class(&t, r->a);
        }
        put_at(&t, r->ip_a);
        if (r->kind == LD_SLEEP_SPIN) {
            put_str(&t, " under spinlock ");
            put_class(&t, r->b);
            put_at(&t, r->ip_b);
        } else {
            put_str(&t, r->kind == LD_SLEEP_IRQ ? " in an interrupt" : " with interrupts off");
        }
        break;
    case LD_BKL_SPIN:
        put_str(&t, "big lock");
        put_at(&t, r->ip_a);
        put_str(&t, " under spinlock ");
        put_class(&t, r->b);
        put_at(&t, r->ip_b);
        break;
    case LD_NO_CLASS:
        put_str(&t, "lock without a class: ");
        put_hex(&t, r->addr);
        put_str(&t, " taken");
        put_at(&t, r->ip_a);
        put_str(&t, ", keyed by its address");
        held = 0;
        break;
    case LD_BAD_RELEASE:
        put_str(&t, "release of a lock not held: ");
        put_class(&t, r->a);
        put_at(&t, r->ip_a);
        break;
    case LD_NOT_HELD:
        put_str(&t, "lock not held: ");
        put_class(&t, r->a);
        put_at(&t, r->ip_a);
        break;
    default:
        put_str(&t, "off: ");
        put_str(&t, ld_off_reason((int)r->addr));
        held = 0;
        break;
    }
    put_str(&t, ", cpu ");
    put_uint(&t, arch_cpu_id());
    put_str(&t, " pid ");
    put_uint(&t, cpu->current ? cpu->current->pid : 0u);
    klog_report(how, KLOG_ERR, "lockdep", "%s", t.buf);
    ld_print_path(how, r);
    if (held) {
        ld_print_held(how, cpu);
    }
}

/** Print the reports this processor's hook made, after the graph's lock
 * is let go. */
static void ld_print(struct percpu *cpu)
{
    struct ld_queue *q = &queues[arch_cpu_id()];
    int how, i;

    if (q->n == 0) {
        return;
    }
    how = klog_report_begin();
    for (i = 0; i < q->n; i++) {
        ld_print_one(how, cpu, &q->r[i]);
    }
    klog_report_end(how);
    q->n = 0;
}

/* ---- the hooks ---- */

/** Enter the checker on this processor with interrupts off, saved in
 * `*flags`: 1, or 0 when it is off, in an NMI or already in the checker,
 * where nothing is checked (`lockdep_recursion`). */
static int ld_enter(uint32_t *flags)
{
    struct percpu *cpu;

    *flags = kirq_save();
    cpu = this_cpu();
    if (ld_off || cpu->in_nmi || cpu->lockdep_busy) {
        kirq_restore(*flags);
        return 0;
    }
    cpu->lockdep_busy++;
    return 1;
}

/** Print what the hook found and leave the checker. */
static void ld_leave(uint32_t flags)
{
    struct percpu *cpu = this_cpu();

    ld_print(cpu);
    cpu->lockdep_busy--;
    kirq_restore(flags);
}

void lockdep_init_map(struct lockdep_map *map, const char *name, const struct lockdep_key *key)
{
    map->name = name;
    map->key = key;
    map->cls = 0;
    map->flags = 0;
}

/**
 * The lock is noted as held once checked, so a lock that deadlocks is
 * reported before the wait; a report does not stop the take.
 */
void lockdep_acquire(struct lockdep_map *map, unsigned int subclass, int how, uintptr_t ip)
{
    struct lockdep_held_lock **list;
    struct lockdep_held_lock *h;
    struct lockdep_held *ctx;
    struct percpu *cpu;
    uint32_t flags, h1, h2;
    unsigned int c, want;
    int on = kirq_enabled(), irq, n;

    if (!ld_enter(&flags)) {
        return;
    }
    cpu = this_cpu();
    irq = in_interrupt();
    ctx = how == LOCKDEP_SLEEPING ? sleep_stack(cpu) : &cpu->lockdep_spin;
    if (ctx->depth == LOCKDEP_DEPTH) {
        raw_spin_lock(&graph_lock);
        ld_stop(LD_OFF_DEPTH);
        raw_spin_unlock(&graph_lock);
        ld_leave(flags);
        return;
    }
    if (subclass == 0 && map->cls != 0) {
        c = map->cls - 1u;
    } else {
        raw_spin_lock(&graph_lock);
        c = ld_class_of(map, subclass, ip);
        raw_spin_unlock(&graph_lock);
        if (c == LD_NONE) {
            ld_leave(flags);
            return;
        }
    }
    if (how == LOCKDEP_SLEEPING && (cpu->lockdep_spin.depth != 0 || irq || !on)) {
        raw_spin_lock(&graph_lock);
        ld_check_sleep(cpu, c, ip, on, irq);
        raw_spin_unlock(&graph_lock);
    }
    want = (irq ? LD_USED_IRQ : 0u) | (on ? LD_ON_IRQ : 0u);
    if ((classes[c].usage & want) != want) {
        raw_spin_lock(&graph_lock);
        if (irq) {
            ld_mark(c, LD_USED_IRQ, ip);
        }
        if (on) {
            ld_mark(c, LD_ON_IRQ, ip);
        }
        raw_spin_unlock(&graph_lock);
    }
    if (how != LOCKDEP_TRY) {
        list = lists[arch_cpu_id()];
        n = ld_context(cpu, how, irq, list);
        ld_chain_key(list, n, c, irq, &h1, &h2);
        if (!ld_chain_find(h1, h2)) {
            raw_spin_lock(&graph_lock);
            if (!ld_chain_find(h1, h2) && ld_chain_add(h1, h2)) {
                ld_validate(list, n, c, ip);
            }
            raw_spin_unlock(&graph_lock);
        }
    }
    h = &ctx->locks[ctx->depth];
    h->map = map;
    h->ip = ip;
    h->cls = (unsigned short)c;
    h->subclass = (unsigned char)subclass;
    h->flags = (unsigned char)((how == LOCKDEP_TRY ? LOCKDEP_HELD_TRY : 0u) |
                               (irq ? LOCKDEP_HELD_IRQ : 0u) |
                               (how == LOCKDEP_SLEEPING ? LOCKDEP_HELD_SLEEP : 0u));
    ctx->depth++;
    ld_leave(flags);
}

/** Take the newest entry for `map` out of `s`: 1, or 0 when it has none. */
static int ld_pop(struct lockdep_held *s, const struct lockdep_map *map)
{
    unsigned int i, j;

    for (i = s->depth; i > 0; i--) {
        if (s->locks[i - 1].map == map) {
            for (j = i; j < s->depth; j++) {
                s->locks[j - 1] = s->locks[j];
            }
            s->depth--;
            return 1;
        }
    }
    return 0;
}

/**
 * A lock is looked for among the spinlocks first and then the sleeping
 * locks, by its map, whatever the context: `schedule` lets `sched_lock` go
 * after it has made the next thread current (proc/sched.c), and in an
 * interrupt's switch the new thread may not be in one.
 */
void lockdep_release(struct lockdep_map *map, uintptr_t ip)
{
    struct percpu *cpu;
    uint32_t flags;

    if (!ld_enter(&flags)) {
        return;
    }
    cpu = this_cpu();
    if (!ld_pop(&cpu->lockdep_spin, map) && !ld_pop(sleep_stack(cpu), map) &&
        !ld_pop(&cpu->lockdep_sleep, map)) {
        raw_spin_lock(&graph_lock);
        if (ld_once(LD_BAD_RELEASE, (uintptr_t)map, 0)) {
            ld_report2(LD_BAD_RELEASE, map->cls ? map->cls - 1u : LD_NONE, LD_NONE, ip, 0);
        }
        raw_spin_unlock(&graph_lock);
    }
    ld_leave(flags);
}

void lockdep_might_sleep(uintptr_t ip)
{
    struct percpu *cpu;
    uint32_t flags;
    int on = kirq_enabled(), irq;

    if (!ld_enter(&flags)) {
        return;
    }
    cpu = this_cpu();
    irq = in_interrupt();
    if (cpu->lockdep_spin.depth != 0 || irq || !on) {
        raw_spin_lock(&graph_lock);
        ld_check_sleep(cpu, LD_NONE, ip, on, irq);
        raw_spin_unlock(&graph_lock);
    }
    ld_leave(flags);
}

void lockdep_bkl_acquire(uintptr_t ip)
{
    struct lockdep_held *sp;
    struct percpu *cpu;
    uint32_t flags;

    if (!ld_enter(&flags)) {
        return;
    }
    cpu = this_cpu();
    sp = &cpu->lockdep_spin;
    if (sp->depth != 0) {
        raw_spin_lock(&graph_lock);
        if (ld_once(LD_BKL_SPIN, sp->locks[sp->depth - 1].cls, ip)) {
            ld_report2(LD_BKL_SPIN, LD_NONE, sp->locks[sp->depth - 1].cls, ip,
                       sp->locks[sp->depth - 1].ip);
        }
        raw_spin_unlock(&graph_lock);
    }
    ld_leave(flags);
}

/**
 * Called by `kirq_restore` just before interrupts come on, with them still
 * off, so it saves and restores no flags of its own, which would call it
 * again.
 */
void lockdep_irqs_on(uintptr_t ip)
{
    struct lockdep_held *sp;
    struct percpu *cpu = this_cpu();
    unsigned int i;

    if (ld_off || cpu->in_nmi || cpu->lockdep_busy || cpu->lockdep_spin.depth == 0) {
        return;
    }
    cpu->lockdep_busy++;
    sp = &cpu->lockdep_spin;
    for (i = 0; i < sp->depth; i++) {
        if (!(classes[sp->locks[i].cls].usage & LD_ON_IRQ)) {
            raw_spin_lock(&graph_lock);
            ld_mark(sp->locks[i].cls, LD_ON_IRQ, ip);
            raw_spin_unlock(&graph_lock);
        }
    }
    ld_print(cpu);
    cpu->lockdep_busy--;
}

int lockdep_is_held(const struct lockdep_map *map)
{
    struct lockdep_held *s;
    struct percpu *cpu;
    uint32_t flags = kirq_save();
    unsigned int i;
    int held = ld_off != 0;

    cpu = this_cpu();
    s = &cpu->lockdep_spin;
    for (i = 0; i < s->depth && !held; i++) {
        held = s->locks[i].map == map;
    }
    s = sleep_stack(cpu);
    for (i = 0; i < s->depth && !held; i++) {
        held = s->locks[i].map == map;
    }
    kirq_restore(flags);
    return held;
}

void lockdep_assert_held_map(const struct lockdep_map *map)
{
    uintptr_t ip = (uintptr_t)(const char *)__builtin_return_address(0);
    uint32_t flags;

    if (lockdep_is_held(map) || !ld_enter(&flags)) {
        return;
    }
    raw_spin_lock(&graph_lock);
    if (ld_once(LD_NOT_HELD, ip, 0)) {
        ld_report2(LD_NOT_HELD, map->cls ? map->cls - 1u : LD_NONE, LD_NONE, ip, 0);
    }
    raw_spin_unlock(&graph_lock);
    ld_leave(flags);
}

void bkl_assert_held_here(void)
{
    if (!smp_serialized()) {
        k_panic("lockdep: big lock not held where it is needed");
    }
}

/* ---- /proc ---- */

/** Append class `c`'s usage, as Linux's four marks are, to `t`. */
static void put_usage(struct ld_text *t, unsigned int c)
{
    put_ch(t, (classes[c].usage & LD_USED_IRQ) ? '+' : '.');
    put_ch(t, (classes[c].usage & LD_ON_IRQ) ? '-' : '.');
}

/** The orders from (`forward`) or to class `c`. */
static unsigned int ld_degree(unsigned int c, int forward)
{
    unsigned int e, n = 0;

    for (e = forward ? classes[c].fwd : classes[c].bwd; e != LD_NONE;
         e = forward ? edges[e].fnext : edges[e].bnext) {
        n++;
    }
    return n;
}

/**
 * A line for each class, as Linux's /proc/lockdep has it: its key, its
 * orders forward and backward, its usage, `+` for taken in an interrupt
 * and `-` for held with interrupts on, and its name; then a line for each
 * class taken under it.
 */
int lockdep_proc_text(char *buf, int cap)
{
    struct ld_text t;
    uint32_t flags;
    unsigned int c, e, n;

    t.buf = buf;
    t.cap = cap;
    t.len = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
    flags = kirq_save();
    raw_spin_lock(&graph_lock);
    put_str(&t, "all lock classes:\n");
    n = nclasses;
    for (c = 0; c < n; c++) {
        put_hex(&t, (uintptr_t)classes[c].key);
        put_str(&t, " FD: ");
        put_uint(&t, ld_degree(c, 1));
        put_str(&t, " BD: ");
        put_uint(&t, ld_degree(c, 0));
        put_ch(&t, ' ');
        put_usage(&t, c);
        put_str(&t, ": ");
        put_class(&t, c);
        put_ch(&t, '\n');
        for (e = classes[c].fwd; e != LD_NONE; e = edges[e].fnext) {
            put_str(&t, " -> ");
            put_class(&t, edges[e].to);
            put_ch(&t, '\n');
        }
    }
    raw_spin_unlock(&graph_lock);
    kirq_restore(flags);
    return t.len;
}

int lockdep_stats_text(char *buf, int cap)
{
    struct ld_text t;
    uint32_t flags;

    t.buf = buf;
    t.cap = cap;
    t.len = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
    flags = kirq_save();
    raw_spin_lock(&graph_lock);
    put_str(&t, " lock-classes:                  ");
    put_uint(&t, nclasses);
    put_str(&t, " [max: ");
    put_uint(&t, LOCKDEP_CLASSES);
    put_str(&t, "]\n direct dependencies:           ");
    put_uint(&t, nedges);
    put_str(&t, " [max: ");
    put_uint(&t, LOCKDEP_EDGES);
    put_str(&t, "]\n dependency chains:             ");
    put_uint(&t, nchains);
    put_str(&t, " [max: ");
    put_uint(&t, LOCKDEP_CHAINS / 4u * 3u);
    put_str(&t, "]\n reports:                       ");
    put_uint(&t, reports);
    put_str(&t, "\n reports not made:              ");
    put_uint(&t, reports_lost);
    put_str(&t, "\n checking:                      ");
    put_str(&t, ld_off ? "off: " : "");
    put_str(&t, ld_off_reason(ld_off));
    put_ch(&t, '\n');
    raw_spin_unlock(&graph_lock);
    kirq_restore(flags);
    return t.len;
}

#else /* !RACH2_LOCKDEP */

/* ISO C has no empty translation unit. */
typedef int lockdep_unused;

#endif /* RACH2_LOCKDEP */
