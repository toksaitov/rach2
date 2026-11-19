/*
 * sched_debug.c: the scheduler's files in /sys/kernel/debug/sched, which
 * read and set the values the fair class runs by, as Linux 6.12's
 * kernel/sched/debug.c registers them.
 */

#include "sched_debug.h"
#include "autogroup.h"
#include "balance.h"
#include "fair.h"
#include "percpu.h"
#include "process.h"
#include "sched.h"
#include "../block/diskinfo.h"
#include "../fs/debugfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstd.h"
#include "../lib/kstring.h"
#include "../lib/rbtree.h"
#include "../mm/usercopy.h"
#include "../platform.h"
#include "../rtc.h"
#include "../syscall.h"
#include "../timer.h"

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

/** Append `s`, then spaces up to `width` bytes in all, as printf's `%-*s`. */
static void put_left(struct out *o, const char *s, int width)
{
    int n = (int)k_strlen(s);

    put_str(o, s);
    while (n++ < width) {
        put_str(o, " ");
    }
}

/** Append `v` in decimal right-aligned in `width` bytes, as `%*lld`. */
static void put_s64_width(struct out *o, int64_t v, int width)
{
    char d[24];
    int n;

    if (v < 0) {
        d[0] = '-';
        (void)diskinfo_u64_dec(d + 1, (int)sizeof(d) - 1, 0u - (uint64_t)v);
    } else {
        (void)diskinfo_u64_dec(d, (int)sizeof(d), (uint64_t)v);
    }
    for (n = (int)k_strlen(d); n < width; n++) {
        put_str(o, " ");
    }
    put_str(o, d);
}

/**
 * Append the nanoseconds `ns` as milliseconds with six decimals, the
 * whole part right-aligned in `width` bytes, as Linux's `%Ld.%06ld` of
 * SPLIT_NS does: the sign goes on the whole part, so a value above -1 ms
 * shows none (kernel/sched/debug.c:17-33).
 */
static void put_ns_width(struct out *o, int64_t ns, int width)
{
    uint64_t mag = ns < 0 ? 0u - (uint64_t)ns : (uint64_t)ns, rem, ms;
    char low[8];
    int i;

    ms = tick_div64(mag, 1000000u, &rem);
    put_s64_width(o, ns < 0 ? -(int64_t)ms : (int64_t)ms, width);
    for (i = 5; i >= 0; i--) {
        low[i] = (char)('0' + (int)(rem % 10u));
        rem /= 10u;
    }
    low[6] = '\0';
    put_str(o, ".");
    put_str(o, low);
}

/** Append a line of debug.c's form "  .NAME: V", the name padded to
 * `width`, with `v` in decimal. */
static void put_row(struct out *o, const char *name, int width, int64_t v)
{
    put_str(o, "  .");
    put_left(o, name, width);
    put_str(o, ": ");
    put_s64_width(o, v, 0);
    put_str(o, "\n");
}

/** Append a line of debug.c's form "  .NAME: MS.NS", the name padded to
 * `width`. */
static void put_row_ns(struct out *o, const char *name, int width, int64_t ns)
{
    put_str(o, "  .");
    put_left(o, name, width);
    put_str(o, ": ");
    put_ns_width(o, ns, 0);
    put_str(o, "\n");
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

/**
 * A file Linux makes with debugfs_create_u32 on the tunable `data`, an
 * unsigned int the scheduler reads under sched_lock: the value in decimal
 * and a newline.
 */
static int32_t u32_show(void *data, char *buf, uint32_t cap)
{
    struct out o;
    unsigned int v;
    uint32_t flags;

    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    flags = sched_lock_take();
    v = *(unsigned int *)data;
    sched_lock_give(flags);
    put_u64(&o, v);
    put_str(&o, "\n");
    return (int32_t)o.len;
}

/** Set the tunable `data` to the low 32 bits of the number written, as
 * Linux's `debugfs_u32_set` does. `len`, or `read_u64`'s error. */
static int32_t u32_store(void *data, const char __user *buf, uint32_t len)
{
    uint64_t v;
    uint32_t flags;
    int32_t rc = read_u64(buf, len, &v);

    if (rc != 0) {
        return rc;
    }
    flags = sched_lock_take();
    *(unsigned int *)data = (unsigned int)v;
    sched_lock_give(flags);
    return (int32_t)len;
}

static const struct debugfs_text_ops u32_ops = {u32_show, u32_store};

/* What a fair_server file of a processor shows and sets: its runtime or
 * its period, Linux's DL_RUNTIME and DL_PERIOD. The file's data is
 * cpu * 2 + which. */
#define SERVER_RUNTIME 0
#define SERVER_PERIOD  1

/** fair_server/cpuN/runtime or period: the server's budget or period in
 * nanoseconds, in decimal, as Linux's `sched_fair_server_show`. */
static int32_t server_show(void *data, char *buf, uint32_t cap)
{
    uintptr_t at = (uintptr_t)data;
    uint64_t runtime, period;
    struct out o;

    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    sched_fair_server((int)(at / 2u), &runtime, &period);
    put_u64(&o, at % 2u == SERVER_RUNTIME ? runtime : period);
    put_str(&o, "\n");
    return (int32_t)o.len;
}

/**
 * Set the runtime or the period of processor N's fair server, as Linux's
 * `sched_fair_server_write` (kernel/sched/debug.c:344-402): the text is a
 * decimal number by kstrtoull_from_user, with at most one newline after
 * it, which `sched_set_fair_server` checks against the other value and
 * applies. `len`; -EINVAL for text that is no such number or a value
 * refused, -ERANGE for one past 64 bits, -EBUSY for a processor offline,
 * or -EFAULT.
 */
static int32_t server_store(void *data, const char __user *buf, uint32_t len)
{
    uintptr_t at = (uintptr_t)data;
    uint64_t runtime, period, v = 0, d;
    char text[24];
    uint32_t n = len < sizeof(text) - 1u ? len : (uint32_t)sizeof(text) - 1u, i;
    int rc;

    if (copy_from_user(text, buf, n) != 0) {
        return -EFAULT;
    }
    text[n] = '\0';
    if (n > 0 && text[n - 1] == '\n') {
        text[--n] = '\0';
    }
    if (n == 0 || (len > sizeof(text) - 1u)) {
        return -EINVAL;
    }
    for (i = text[0] == '+' ? 1u : 0u; i < n; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return -EINVAL;
        }
        d = (uint64_t)(text[i] - '0');
        if (v > (~(uint64_t)0 - d) / 10u) {
            return -ERANGE;
        }
        v = v * 10u + d;
    }
    if (text[0] == '+' && n == 1u) {
        return -EINVAL;
    }
    sched_fair_server((int)(at / 2u), &runtime, &period);
    if (at % 2u == SERVER_RUNTIME) {
        runtime = v;
    } else {
        period = v;
    }
    rc = sched_set_fair_server((int)(at / 2u), runtime, period);
    return rc != 0 ? rc : (int32_t)len;
}

static const struct debugfs_text_ops server_ops = {server_show, server_store};

/* The bandwidth unit of Linux's deadline class, 1 << BW_SHIFT. */
#define BW_UNIT (1u << 20)

/** Linux's to_ratio: `runtime` over `period` in BW_UNITs, BW_UNIT for an
 * unlimited runtime, 0 for a period of 0 (kernel/sched/core.c:4818). */
static uint64_t bw_ratio(uint64_t period, uint64_t runtime, int unlimited)
{
    if (unlimited) {
        return BW_UNIT;
    }
    return period == 0 ? 0 : tick_div64(runtime << 20, period, NULL);
}

/** The state letter of thread `p`, as Linux's task_state_to_char. */
static char state_char(const process_t *p)
{
    if (p->state == PROC_ZOMBIE) {
        return 'Z';
    }
    if (p->ptrace_status != 0) {
        return 't';
    }
    if (p->state == PROC_STOPPED) {
        return 'T';
    }
    if (p->state == PROC_BLOCKED) {
        return p->uninterruptible ? 'D' : 'S';
    }
    return 'R';
}

/** Linux's prio of thread `p`: 99 less its effective real-time priority,
 * -1 for SCHED_DEADLINE, or 120 plus its nice value. */
static int task_prio(const process_t *p)
{
    unsigned int rt = sched_priority(p);

    if (rt > 0) {
        return 99 - (int)rt;
    }
    return p->sched_policy == SCHED_DEADLINE ? -1 : 120 + (int)p->nice;
}

/** Append the path of the group `tg` as Linux's task_group_path gives it:
 * "/autogroup-ID" for an autogroup, "/" for the root group. */
static void put_group(struct out *o, const struct task_group *tg)
{
    if (tg && tg->autogroup) {
        put_str(o, "/autogroup-");
        put_u64(o, tg->autogroup->id);
    } else {
        put_str(o, "/");
    }
}

/** Append the queue `q`'s lines, as Linux's print_cfs_rq and
 * print_cfs_group_stats (debug.c:648-683, 787-883). */
static void put_cfs_rq(struct out *o, int cpu, struct cfs_rq *q)
{
    int64_t left_vruntime = -1, right_vruntime = -1, left_deadline = -1;
    struct rb_node *n;
    struct sched_entity *se;

    put_str(o, "\ncfs_rq[");
    put_u64(o, (uint64_t)cpu);
    put_str(o, "]:");
    put_group(o, q->tg);
    put_str(o, "\n");
    n = q->tasks_timeline.rb_root.rb_node;
    if (n) {
        left_vruntime = (int64_t)rb_entry(n, struct sched_entity, run_node)->min_vruntime;
    }
    n = q->tasks_timeline.rb_leftmost;
    if (n) {
        left_deadline = (int64_t)rb_entry(n, struct sched_entity, run_node)->deadline;
    }
    n = rb_last(&q->tasks_timeline.rb_root);
    if (n) {
        right_vruntime = (int64_t)rb_entry(n, struct sched_entity, run_node)->vruntime;
    }
    put_row_ns(o, "left_deadline", 30, left_deadline);
    put_row_ns(o, "left_vruntime", 30, left_vruntime);
    put_row_ns(o, "zero_vruntime", 30, (int64_t)q->zero_vruntime);
    put_row_ns(o, "avg_vruntime", 30, (int64_t)fair_avg_vruntime(q));
    put_row_ns(o, "right_vruntime", 30, right_vruntime);
    put_row_ns(o, "spread", 30, right_vruntime - left_vruntime);
    put_row(o, "nr_running", 30, q->nr_running);
    put_row(o, "h_nr_runnable", 30, q->h_nr_runnable);
    put_row(o, "h_nr_queued", 30, q->h_nr_queued);
    put_row(o, "h_nr_delayed", 30, q->h_nr_delayed);
    put_row(o, "idle_nr_running", 30, q->idle_nr_running);
    put_row(o, "idle_h_nr_running", 30, q->idle_h_nr_running);
    put_row(o, "load", 30, (int64_t)q->load.weight);
    put_row(o, "load_avg", 30, (int64_t)q->avg.load_avg);
    put_row(o, "runnable_avg", 30, (int64_t)q->avg.runnable_avg);
    put_row(o, "util_avg", 30, (int64_t)q->avg.util_avg);
    put_row(o, "util_est", 30, q->avg.util_est);
    put_row(o, "removed.load_avg", 30, (int64_t)q->removed.load_avg);
    put_row(o, "removed.util_avg", 30, (int64_t)q->removed.util_avg);
    put_row(o, "removed.runnable_avg", 30, (int64_t)q->removed.runnable_avg);
    put_row(o, "tg_load_avg_contrib", 30, (int64_t)q->tg_load_avg_contrib);
    put_row(o, "tg_load_avg", 30, q->tg ? q->tg->load_avg : 0);
    se = q->tg && q->tg->se ? q->tg->se[cpu] : NULL;
    if (!se) {
        return;
    }
    put_row_ns(o, "se->exec_start", 30, (int64_t)se->exec_start);
    put_row_ns(o, "se->vruntime", 30, (int64_t)se->vruntime);
    put_row_ns(o, "se->sum_exec_runtime", 30, (int64_t)se->sum_exec_runtime);
    if (fair_schedstats) {
        put_row_ns(o, "stats->wait_start", 30, (int64_t)se->stats.wait_start);
        put_row_ns(o, "stats->sleep_start", 30, (int64_t)se->stats.sleep_start);
        put_row_ns(o, "stats->block_start", 30, (int64_t)se->stats.block_start);
        put_row_ns(o, "stats->sleep_max", 30, (int64_t)se->stats.sleep_max);
        put_row_ns(o, "stats->block_max", 30, (int64_t)se->stats.block_max);
        put_row_ns(o, "stats->exec_max", 30, se->stats.exec_max);
        put_row_ns(o, "stats->slice_max", 30, (int64_t)se->stats.slice_max);
        put_row_ns(o, "stats->wait_max", 30, (int64_t)se->stats.wait_max);
        put_row_ns(o, "stats->wait_sum", 30, (int64_t)se->stats.wait_sum);
        put_row(o, "stats->wait_count", 30, (int64_t)se->stats.wait_count);
    }
    put_row(o, "se->load.weight", 30, (int64_t)se->load.weight);
    put_row(o, "se->avg.load_avg", 30, (int64_t)se->avg.load_avg);
    put_row(o, "se->avg.util_avg", 30, (int64_t)se->avg.util_avg);
    put_row(o, "se->avg.runnable_avg", 30, (int64_t)se->avg.runnable_avg);
}

/** Append thread `p`'s line of the runnable tasks, as Linux's print_task
 * (debug.c:744-777) with CONFIG_CGROUP_SCHED and without NUMA balancing. */
static void put_task(struct out *o, process_t *p, int current)
{
    struct sched_entity *se = &p->se;
    int eligible = se->cfs_rq && fair_entity_eligible(se->cfs_rq, se);
    char st[3];

    st[0] = current ? '>' : ' ';
    st[1] = current ? 'R' : state_char(p);
    st[2] = '\0';
    put_str(o, st);
    put_str(o, " ");
    {
        char comm[sizeof(p->comm) + 1];
        int i;

        k_memcpy(comm, p->comm, sizeof(p->comm));
        comm[sizeof(p->comm)] = '\0';
        for (i = (int)k_strlen(comm); i < 15; i++) {
            put_str(o, " ");
        }
        put_str(o, comm);
    }
    put_str(o, " ");
    put_s64_width(o, (int64_t)p->pid, 5);
    put_str(o, " ");
    put_ns_width(o, (int64_t)se->vruntime, 9);
    put_str(o, eligible ? "   E   " : "   N   ");
    put_ns_width(o, (int64_t)se->deadline, 9);
    put_str(o, se->custom_slice ? " S " : "   ");
    put_ns_width(o, (int64_t)se->slice, 9);
    put_str(o, " ");
    put_ns_width(o, (int64_t)se->sum_exec_runtime, 9);
    put_str(o, " ");
    put_s64_width(o, (int64_t)(p->nvcsw + p->nivcsw), 9);
    put_str(o, "   ");
    put_s64_width(o, task_prio(p), 5);
    put_str(o, " ");
    put_ns_width(o, fair_schedstats ? (int64_t)se->stats.wait_sum : 0, 9);
    put_str(o, " ");
    put_ns_width(o, fair_schedstats ? se->stats.sum_sleep_runtime : 0, 9);
    put_str(o, " ");
    put_ns_width(o, fair_schedstats ? se->stats.sum_block_runtime : 0, 9);
    put_str(o, "        ");
    put_group(o, se->cfs_rq ? se->cfs_rq->tg : NULL);
    put_str(o, "\n");
}

/** Append processor `cpu`'s section, as Linux's print_cpu (debug.c:
 * 940-1001) with print_cfs_stats over its queues that hold load,
 * print_rt_rq, print_dl_rq and print_rq. With sched_lock held. */
static void put_cpu(struct out *o, int cpu, uint64_t dl_total)
{
    struct sched_rq_debug rq;
    uint64_t next_balance, avg_idle, max_cost, mhz, khz_low;
    uint64_t khz = tick_div64(platform_cpu_hz(), 1000u, NULL);
    struct cfs_rq *q;
    process_t *t;
    int period, runtime;

    sched_rq_debug(cpu, &rq);
    balance_rq_debug(cpu, &next_balance, &avg_idle, &max_cost);
    if (khz == 0) {
        khz = 1;
    }
    put_str(o, "cpu#");
    put_u64(o, (uint64_t)cpu);
    put_str(o, ", ");
    mhz = tick_div64(khz, 1000u, &khz_low);
    put_u64(o, mhz);
    put_str(o, ".");
    put_str(o, khz_low < 100u ? (khz_low < 10u ? "00" : "0") : "");
    put_u64(o, khz_low);
    put_str(o, " MHz\n");
    put_row(o, "nr_running", 30, rq.nr_running);
    put_row(o, "nr_switches", 30, (int64_t)rq.nr_switches);
    put_row(o, "nr_uninterruptible", 30, rq.nr_uninterruptible);
    put_row_ns(o, "next_balance", 30, (int64_t)next_balance);
    put_row(o, "curr->pid", 30, rq.curr_pid);
    put_row_ns(o, "clock", 30, (int64_t)rq.clock);
    put_row_ns(o, "clock_task", 30, (int64_t)rq.clock_task);
    put_row(o, "avg_idle", 30, (int64_t)avg_idle);
    put_row(o, "max_idle_balance_cost", 30, (int64_t)max_cost);
    if (fair_schedstats) {
        put_row(o, "yld_count", 30, rq.yld_count);
        put_row(o, "sched_count", 30, rq.sched_count);
        put_row(o, "sched_goidle", 30, rq.sched_goidle);
        put_row(o, "ttwu_count", 30, rq.ttwu_count);
        put_row(o, "ttwu_local", 30, rq.ttwu_local);
    }
    for (q = fair_rqs[cpu].leaf_head; q; q = q->leaf_next) {
        put_cfs_rq(o, cpu, q);
    }
    put_str(o, "\nrt_rq[");
    put_u64(o, (uint64_t)cpu);
    put_str(o, "]:\n");
    put_row(o, "rt_nr_running", 30, rq.rt_nr_running);
    put_str(o, "\ndl_rq[");
    put_u64(o, (uint64_t)cpu);
    put_str(o, "]:\n");
    put_row(o, "dl_nr_running", 30, 0);
    sched_rt_bandwidth(&period, &runtime);
    put_row(
        o, "dl_bw->bw", 30,
        (int64_t)bw_ratio((uint64_t)period, (uint64_t)(runtime < 0 ? 0 : runtime), runtime < 0));
    put_row(o, "dl_bw->total_bw", 30, (int64_t)dl_total);
    put_str(o, "\nrunnable tasks:\n"
               " S            task   PID       vruntime   eligible    "
               "deadline             slice          sum-exec      switches  "
               "prio         wait-time        sum-sleep       sum-block"
               "  group-path\n"
               "-------------------------------------------------------"
               "------------------------------------------------------"
               "------------------------------------------------------"
               "--------------\n");
    for (t = proc_list; t; t = t->next) {
        if (t->cpu == cpu && t->state != PROC_UNUSED && t != cpu_table[cpu].idle) {
            put_task(o, t, cpu_table[cpu].current == t);
        }
    }
    put_str(o, "\n");
}

/**
 * debug: the scheduler's state, as Linux's sched_debug_show (debug.c:
 * 1007-1063): a header with the clocks, the tick count and the base slice
 * and features, then each online processor's run queue, fair queues with
 * load, real-time and deadline queues and threads. The deadline queue's
 * total bandwidth is the fair servers' of every online processor, as
 * Linux's root domain sums it; it runs no deadline thread.
 */
static int32_t debug_show(void *data, char *buf, uint32_t cap)
{
    struct out o;
    uint64_t now = (uint64_t)rtc_monotonic_ns(), runtime, period, total = 0;
    uint32_t flags;
    int c;

    (void)data;
    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    put_str(&o, "Sched Debug Version: v0.11, " UTS_RELEASE " #1\n");
    put_left(&o, "ktime", 40);
    put_str(&o, ": ");
    put_ns_width(&o, (int64_t)now, 0);
    put_str(&o, "\n");
    put_left(&o, "sched_clk", 40);
    put_str(&o, ": ");
    put_ns_width(&o, (int64_t)now, 0);
    put_str(&o, "\n");
    put_left(&o, "cpu_clk", 40);
    put_str(&o, ": ");
    put_ns_width(&o, (int64_t)now, 0);
    put_str(&o, "\n");
    put_left(&o, "jiffies", 40);
    put_str(&o, ": ");
    put_u64(&o, kernel_ticks);
    put_str(&o, "\n\nsysctl_sched\n");
    put_row_ns(&o, "sysctl_sched_base_slice", 40, (int64_t)sysctl_sched_base_slice);
    put_row(&o, "sysctl_sched_features", 40, sched_features);
    put_str(&o, "  .");
    put_left(&o, "sysctl_sched_tunable_scaling", 40);
    put_str(&o, ": 1 (logarithmic)\n\n");
    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            sched_fair_server(c, &runtime, &period);
            total += bw_ratio(period, runtime, 0);
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            flags = sched_lock_take();
            put_cpu(&o, c, total);
            sched_lock_give(flags);
        }
    }
    return (int32_t)o.len;
}

static const struct debugfs_text_ops debug_ops = {debug_show, NULL};

/* The files, Linux's names and modes (debug.c:501-537). */
static const struct {
    const char *path;
    uint32_t mode;
    const struct debugfs_text_ops *ops;
    void *data;
} files[] = {{"sched/features", 0644u, &features_ops, NULL},
             {"sched/base_slice_ns", 0644u, &base_slice_ops, NULL},
             {"sched/migration_cost_ns", 0644u, &u32_ops, &sysctl_sched_migration_cost},
             {"sched/nr_migrate", 0644u, &u32_ops, &sysctl_sched_nr_migrate},
             {"sched/debug", 0444u, &debug_ops, NULL}};

void sched_debugfs_init(void)
{
    char path[48];
    uint32_t i;
    int c, w;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (debugfs_create_text(files[i].path, files[i].mode, files[i].ops, files[i].data) != 0) {
            klog(KLOG_WARN, "sched", "debugfs: %s not registered", files[i].path);
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        if (!cpu_table[c].online) {
            continue;
        }
        for (w = SERVER_RUNTIME; w <= SERVER_PERIOD; w++) {
            k_memcpy(path, "sched/fair_server/cpu", 22);
            (void)diskinfo_u64_dec(path + 21, 8, (uint64_t)c);
            k_memcpy(path + k_strlen(path), w == SERVER_RUNTIME ? "/runtime" : "/period",
                     w == SERVER_RUNTIME ? 9u : 8u);
            if (debugfs_create_text(path, 0644u, &server_ops, (void *)(uintptr_t)(c * 2 + w)) !=
                0) {
                klog(KLOG_WARN, "sched", "debugfs: %s not registered", path);
            }
        }
    }
}
