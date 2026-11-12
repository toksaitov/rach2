/*
 * exits.c: counts, on each processor, of the instructions a hypervisor may
 * take as a VM exit, by kind and number, which debugfs's `exits` prints.
 */

#include "exits.h"
#include "../arch.h"
#include "../../proc/percpu.h"
#include "../../proc/smp.h"
#include "../../cmdline.h"

/* The kinds and numbers one processor has counted, in the order it first
 * met them; a running system fills a few dozen. */
#define EXITS_SLOTS 64

/* A slot being claimed, which no lookup matches. */
#define EXITS_CLAIMING 0xFFu

struct exits_slot {
    volatile unsigned int kind; /* EXIT_*, 0 while free, EXITS_CLAIMING while claimed */
    uint32_t number;            /* the MSR, the leaf, the port or the IPI's kind */
    uint32_t count;
};

struct exits_cpu {
    struct exits_slot slot[EXITS_SLOTS];
    uint32_t lost; /* counts that found every slot taken */
};

static struct exits_cpu exits_cpus[NR_CPUS];

/* 0 until every processor's own record is reachable (`exits_start`). */
static volatile int exits_on;

static const char *const kind_names[EXIT_KINDS] = {
    "", "rdmsr", "wrmsr", "cpuid", "in", "out", "hlt", "eoipv", "eoitaken", "eoiunset", "vmcall"};

int exits_start(void)
{
    exits_on = kcmdline_has("rach2.exits");
    return exits_on;
}

int exits_counting(void)
{
    return exits_on;
}

/**
 * Count one instruction of `kind` on `number` on this processor. A slot is
 * claimed by an exchange, so that an NMI that counts between the claim and
 * its fill takes another slot; an NMI that counts the same slot between a
 * load and a store of `count` loses one count of it.
 */
void exits_count(unsigned int kind, uint32_t number)
{
    struct exits_cpu *c;
    struct exits_slot *s;
    int i;

    if (!exits_on) {
        return;
    }
    c = &exits_cpus[arch_cpu_id()];
    for (i = 0; i < EXITS_SLOTS; i++) {
        s = &c->slot[i];
        if (s->kind == kind && s->number == number) {
            s->count++;
            return;
        }
        if (s->kind == 0 && arch_atomic_cmpxchg(&s->kind, 0, EXITS_CLAIMING) == 0) {
            s->number = number;
            s->count = 1;
            (void)arch_atomic_xchg(&s->kind, kind);
            return;
        }
    }
    c->lost++;
}

/* A text being made: the bytes go to `buf` while they fit, and `len`
 * counts them all, so that a NULL buffer sizes the text. */
struct exits_text {
    char *buf;
    uint32_t cap;
    uint32_t len;
};

/** Add the string `s` to the text `t`. */
static void put_str(struct exits_text *t, const char *s)
{
    while (*s) {
        if (t->len < t->cap) {
            t->buf[t->len] = *s;
        }
        t->len++;
        s++;
    }
}

/** Add `v` to the text `t`, in hexadecimal with `0x` when `hex`, else in
 * decimal. */
static void put_num(struct exits_text *t, uint32_t v, int hex)
{
    char tmp[12];
    int n = 0;
    uint32_t base = hex ? 16u : 10u;

    if (hex) {
        put_str(t, "0x");
    }
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v != 0);
    while (n > 0) {
        char one[2];

        one[0] = tmp[--n];
        one[1] = '\0';
        put_str(t, one);
    }
}

/** The count of `kind` on `number` on processor `cpu`, 0 if it has none. */
static uint32_t count_of(int cpu, unsigned int kind, uint32_t number)
{
    const struct exits_cpu *c = &exits_cpus[cpu];
    int i;

    for (i = 0; i < EXITS_SLOTS; i++) {
        if (c->slot[i].kind == kind && c->slot[i].number == number) {
            return c->slot[i].count;
        }
    }
    return 0;
}

/** Whether a slot of a processor before `cpu` already holds `kind` on
 * `number`, whose line that processor's turn printed. */
static int seen_before(int cpu, unsigned int kind, uint32_t number)
{
    int c;

    for (c = 0; c < cpu; c++) {
        if (count_of(c, kind, number) != 0) {
            return 1;
        }
    }
    return 0;
}

/** The rest of a line: ` TOTAL CPU0 CPU1 ...` of the processors up to
 * `ncpu`, and the newline. */
static void put_counts(struct exits_text *t, const uint32_t *per, int ncpu)
{
    uint32_t total = 0;
    int c;

    for (c = 0; c < ncpu; c++) {
        total += per[c];
    }
    put_str(t, " ");
    put_num(t, total, 0);
    for (c = 0; c < ncpu; c++) {
        put_str(t, " ");
        put_num(t, per[c], 0);
    }
    put_str(t, "\n");
}

int32_t exits_show(void *data, char *buf, uint32_t cap)
{
    struct exits_text t;
    uint32_t per[NR_CPUS] = {0};
    int ncpu = smp_cpu_count();
    int c, d, i, k;

    (void)data;
    t.buf = buf;
    t.cap = buf ? cap : 0;
    t.len = 0;
    if (ncpu > NR_CPUS) {
        ncpu = NR_CPUS;
    }
    for (c = 0; c < ncpu; c++) {
        for (i = 0; i < EXITS_SLOTS; i++) {
            const struct exits_slot *s = &exits_cpus[c].slot[i];
            unsigned int kind = s->kind;

            if (kind == 0 || kind >= EXIT_KINDS || seen_before(c, kind, s->number)) {
                continue;
            }
            for (d = 0; d < ncpu; d++) {
                per[d] = count_of(d, kind, s->number);
            }
            put_str(&t, kind_names[kind]);
            put_str(&t, " ");
            put_num(&t, s->number, kind != EXIT_HLT);
            put_counts(&t, per, ncpu);
        }
    }
    for (k = 0; k < SMP_IPI_KINDS; k++) {
        for (d = 0; d < ncpu; d++) {
            per[d] = smp_ipi_count(d, k);
        }
        put_str(&t, "ipi ");
        put_str(&t, smp_ipi_name(k));
        put_counts(&t, per, ncpu);
    }
    for (d = 0; d < ncpu; d++) {
        per[d] = exits_cpus[d].lost;
    }
    put_str(&t, "lost");
    put_counts(&t, per, ncpu);
    return (int32_t)t.len;
}
