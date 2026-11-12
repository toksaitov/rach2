/*
 * exitstat.c: the guests' exits counted by the processor's exit code since
 * boot, with the TSC cycles of each kind's entry, run and exit, its wait for
 * the big lock and its handling, read from the debug file kvm/exit_stats.
 * KVM_RUN runs without the big lock, so each processor keeps counts of its
 * own.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../fs/debugfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/percpu.h"
#include "../proc/smp.h"

/*
 * The rows. A code below ROW_HIGH has the row of its number, SVM's
 * 0x400 to 0x40F (NPF, AVIC, VMGEXIT) rows from ROW_HIGH on, and any other
 * code ROW_OTHER. A failed entry, SVM's EXITCODE -1 or VMX's bit 31, is
 * ROW_FAIL.
 */
#define ROW_HIGH  0xB0u
#define ROW_OTHER (ROW_HIGH + 16u)
#define ROW_FAIL  (ROW_OTHER + 1u)
#define ROWS      (ROW_FAIL + 1u)

#define SVM_EXIT_HIGH 0x400u
#define VMX_FAILED    0x80000000u

/*
 * One kind's counts. `kernel` exits were handled in the kernel and the
 * guest entered again; `user` ended KVM_RUN, most of them for the caller
 * to answer. The rest are TSC cycles, of the run each exit ends and of the
 * exit's handling after it, in the order they pass:
 *
 *   entry    from just before the run loop enters the guest to the entry
 *            instruction: the guest's state loaded and the VMCS or VMCB
 *            brought up to date;
 *   run      from the entry instruction to the exit: the processor's
 *            entry and exit and the guest's own run;
 *   exit     from the exit to the backend's return: the host's state put
 *            back and the exit read;
 *   wait     the part of the exit's handling spent waiting for the big
 *            lock (`kvm_bkl_enter`), which the exits the kernel handles
 *            without it never do;
 *   handle   the rest of the time from the backend's return to just
 *            before the next entry, or to KVM_RUN's return;
 *   prepare  the part of `handle` from the run loop's first pass over its
 *            top after the exit was handled to the end of `handle`: a
 *            switch to another thread at the slice's end, the requests,
 *            the local APIC's work, a halted vCPU's sleep until an
 *            interrupt, and the event to inject.
 *
 * A HLT the kernel handles only marks the vCPU halted, and its sleep comes
 * at the loop's top, so both `handle` and `prepare` count it. entry, run
 * and exit count only the runs whose backend timed them; `prepare` only the
 * exits handled in the kernel.
 */
struct row {
    uint64_t exits, kernel, user, wait, handle, prepare, entry, run, exit;
};

/*
 * Each processor's ROWS rows, made for the processors started by
 * kvm_exitstat_start and again at each VM's creation, since the others
 * start after KVM, as Linux keeps a vCPU's exits in its own counters
 * (arch/x86/kvm/x86.c:10657); NULL for any other, whose exits go
 * uncounted. A processor writes only its own rows, with interrupts off, so
 * no lock guards them. A read from another processor may miss an exit
 * counted at the same time. A clear while a guest runs may leave a row
 * uncleared: a count read before the clear and stored after it keeps its
 * old value and the exit, and on i686 a 64-bit count may keep its old high
 * word, as Linux's vcpu_stat_clear zeroes the vCPUs' counters under no
 * vCPU's lock (virt/kvm/kvm_main.c:6159-6168, 6278-6293). An exit's
 * account may close on another processor than the one that opened it, so
 * only the sums over the processors, which kvm/exit_stats shows, mean
 * anything.
 */
static struct row *rows[NR_CPUS];

/*
 * Each processor's completed entries since boot, for every processor,
 * rows or none, and never cleared. Written as the rows are and summed
 * without a lock by kvm_exit_count, as Linux's vcpu_stat_get sums its
 * vCPUs' counters (virt/kvm/kvm_main.c:6146-6156, 6264-6274); on i686 a
 * sum read while another processor carries into a count's high word may
 * be off by that carry, as on 32-bit Linux.
 */
static uint64_t cpu_exits[NR_CPUS];

/* A code and its name, as Linux's SVM_EXIT_REASONS and VMX_EXIT_REASONS
 * give them to its kvm_exit trace event. */
struct code_name {
    uint32_t code;
    const char *name;
};

static const struct code_name svm_names[] = {{0x00, "read_cr0"},
                                             {0x02, "read_cr2"},
                                             {0x03, "read_cr3"},
                                             {0x04, "read_cr4"},
                                             {0x08, "read_cr8"},
                                             {0x10, "write_cr0"},
                                             {0x12, "write_cr2"},
                                             {0x13, "write_cr3"},
                                             {0x14, "write_cr4"},
                                             {0x18, "write_cr8"},
                                             {0x20, "read_dr0"},
                                             {0x21, "read_dr1"},
                                             {0x22, "read_dr2"},
                                             {0x23, "read_dr3"},
                                             {0x24, "read_dr4"},
                                             {0x25, "read_dr5"},
                                             {0x26, "read_dr6"},
                                             {0x27, "read_dr7"},
                                             {0x30, "write_dr0"},
                                             {0x31, "write_dr1"},
                                             {0x32, "write_dr2"},
                                             {0x33, "write_dr3"},
                                             {0x34, "write_dr4"},
                                             {0x35, "write_dr5"},
                                             {0x36, "write_dr6"},
                                             {0x37, "write_dr7"},
                                             {0x40, "DE excp"},
                                             {0x41, "DB excp"},
                                             {0x43, "BP excp"},
                                             {0x44, "OF excp"},
                                             {0x45, "BR excp"},
                                             {0x46, "UD excp"},
                                             {0x47, "NM excp"},
                                             {0x48, "DF excp"},
                                             {0x4A, "TS excp"},
                                             {0x4B, "NP excp"},
                                             {0x4C, "SS excp"},
                                             {0x4D, "GP excp"},
                                             {0x4E, "PF excp"},
                                             {0x50, "MF excp"},
                                             {0x51, "AC excp"},
                                             {0x52, "MC excp"},
                                             {0x53, "XF excp"},
                                             {0x60, "interrupt"},
                                             {0x61, "nmi"},
                                             {0x62, "smi"},
                                             {0x63, "init"},
                                             {0x64, "vintr"},
                                             {0x65, "cr0_sel_write"},
                                             {0x66, "read_idtr"},
                                             {0x67, "read_gdtr"},
                                             {0x68, "read_ldtr"},
                                             {0x69, "read_rt"},
                                             {0x6A, "write_idtr"},
                                             {0x6B, "write_gdtr"},
                                             {0x6C, "write_ldtr"},
                                             {0x6D, "write_rt"},
                                             {0x6E, "rdtsc"},
                                             {0x6F, "rdpmc"},
                                             {0x70, "pushf"},
                                             {0x71, "popf"},
                                             {0x72, "cpuid"},
                                             {0x73, "rsm"},
                                             {0x74, "iret"},
                                             {0x75, "swint"},
                                             {0x76, "invd"},
                                             {0x77, "pause"},
                                             {0x78, "hlt"},
                                             {0x79, "invlpg"},
                                             {0x7A, "invlpga"},
                                             {0x7B, "io"},
                                             {0x7C, "msr"},
                                             {0x7D, "task_switch"},
                                             {0x7E, "ferr_freeze"},
                                             {0x7F, "shutdown"},
                                             {0x80, "vmrun"},
                                             {0x81, "hypercall"},
                                             {0x82, "vmload"},
                                             {0x83, "vmsave"},
                                             {0x84, "stgi"},
                                             {0x85, "clgi"},
                                             {0x86, "skinit"},
                                             {0x87, "rdtscp"},
                                             {0x88, "icebp"},
                                             {0x89, "wbinvd"},
                                             {0x8A, "monitor"},
                                             {0x8B, "mwait"},
                                             {0x8D, "xsetbv"},
                                             {0x8F, "write_efer_trap"},
                                             {0x90, "write_cr0_trap"},
                                             {0x94, "write_cr4_trap"},
                                             {0x98, "write_cr8_trap"},
                                             {0xA2, "invpcid"},
                                             {0x400, "npf"},
                                             {0x401, "avic_incomplete_ipi"},
                                             {0x402, "avic_unaccelerated_access"},
                                             {0x403, "vmgexit"},
                                             {0xFFFFFFFFu, "invalid_guest_state"}};

static const struct code_name vmx_names[] = {{0, "EXCEPTION_NMI"},
                                             {1, "EXTERNAL_INTERRUPT"},
                                             {2, "TRIPLE_FAULT"},
                                             {3, "INIT_SIGNAL"},
                                             {4, "SIPI_SIGNAL"},
                                             {7, "INTERRUPT_WINDOW"},
                                             {8, "NMI_WINDOW"},
                                             {9, "TASK_SWITCH"},
                                             {10, "CPUID"},
                                             {12, "HLT"},
                                             {13, "INVD"},
                                             {14, "INVLPG"},
                                             {15, "RDPMC"},
                                             {16, "RDTSC"},
                                             {18, "VMCALL"},
                                             {19, "VMCLEAR"},
                                             {20, "VMLAUNCH"},
                                             {21, "VMPTRLD"},
                                             {22, "VMPTRST"},
                                             {23, "VMREAD"},
                                             {24, "VMRESUME"},
                                             {25, "VMWRITE"},
                                             {26, "VMOFF"},
                                             {27, "VMON"},
                                             {28, "CR_ACCESS"},
                                             {29, "DR_ACCESS"},
                                             {30, "IO_INSTRUCTION"},
                                             {31, "MSR_READ"},
                                             {32, "MSR_WRITE"},
                                             {33, "INVALID_STATE"},
                                             {34, "MSR_LOAD_FAIL"},
                                             {36, "MWAIT_INSTRUCTION"},
                                             {37, "MONITOR_TRAP_FLAG"},
                                             {39, "MONITOR_INSTRUCTION"},
                                             {40, "PAUSE_INSTRUCTION"},
                                             {41, "MCE_DURING_VMENTRY"},
                                             {43, "TPR_BELOW_THRESHOLD"},
                                             {44, "APIC_ACCESS"},
                                             {45, "EOI_INDUCED"},
                                             {46, "GDTR_IDTR"},
                                             {47, "LDTR_TR"},
                                             {48, "EPT_VIOLATION"},
                                             {49, "EPT_MISCONFIG"},
                                             {50, "INVEPT"},
                                             {51, "RDTSCP"},
                                             {52, "PREEMPTION_TIMER"},
                                             {53, "INVVPID"},
                                             {54, "WBINVD"},
                                             {55, "XSETBV"},
                                             {56, "APIC_WRITE"},
                                             {57, "RDRAND"},
                                             {58, "INVPCID"},
                                             {59, "VMFUNC"},
                                             {60, "ENCLS"},
                                             {61, "RDSEED"},
                                             {62, "PML_FULL"},
                                             {63, "XSAVES"},
                                             {64, "XRSTORS"},
                                             {67, "UMWAIT"},
                                             {68, "TPAUSE"},
                                             {74, "BUS_LOCK"},
                                             {75, "NOTIFY"},
                                             {VMX_FAILED, "FAILED_VMENTRY"}};

/** Whether the backend is VMX's; any other, SVM's or a test's, counts by
 * SVM's codes. */
static int is_vmx(void)
{
    return kvm_backend == &kvm_vmx;
}

/** The row of the exit code `code`. */
static uint32_t row_of(uint32_t code)
{
    if (is_vmx()) {
        if (code & VMX_FAILED) {
            return ROW_FAIL;
        }
        code &= 0xFFFFu;
        return code < ROW_HIGH ? code : ROW_OTHER;
    }
    if (code == 0xFFFFFFFFu) {
        return ROW_FAIL;
    }
    if (code < ROW_HIGH) {
        return code;
    }
    if (code >= SVM_EXIT_HIGH && code < SVM_EXIT_HIGH + 16u) {
        return ROW_HIGH + (code - SVM_EXIT_HIGH);
    }
    return ROW_OTHER;
}

/** The exit code of row `row`, the reverse of `row_of`; ROW_OTHER has
 * none and gives 0. */
static uint32_t code_of(uint32_t row)
{
    if (row == ROW_FAIL) {
        return is_vmx() ? VMX_FAILED : 0xFFFFFFFFu;
    }
    if (row == ROW_OTHER) {
        return 0;
    }
    if (row >= ROW_HIGH && !is_vmx()) {
        return SVM_EXIT_HIGH + (row - ROW_HIGH);
    }
    return row;
}

/** Linux's name of the exit code `code`, or NULL for one it does not
 * name. */
static const char *name_of(uint32_t code)
{
    const struct code_name *t = is_vmx() ? vmx_names : svm_names;
    uint32_t n = is_vmx() ? sizeof(vmx_names) / sizeof(vmx_names[0])
                          : sizeof(svm_names) / sizeof(svm_names[0]);
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (t[i].code == code) {
            return t[i].name;
        }
    }
    return NULL;
}

uint64_t kvm_exit_count(void)
{
    uint64_t n = 0;
    uint32_t i;

    for (i = 0; i < NR_CPUS; i++) {
        n += cpu_exits[i];
    }
    return n;
}

/** Close the open account of `v`'s exit in `row` at the TSC count `now`:
 * its waits for the big lock go to the row's wait, the rest of its time
 * since `v->xs_mark` to its handling. `now` may come from another
 * processor than `v->xs_mark`, whose TSC may read a little lower, as
 * Linux's kvm_arch_vcpu_load finds a backwards TSC when a vCPU moves
 * (arch/x86/kvm/x86.c:4584-4588); such a span counts as 0, not as a
 * wrap of some 2^64 cycles. */
static void account_close(struct row *row, struct kvm_vcpu *v, uint64_t now)
{
    uint64_t spent = now >= v->xs_mark ? now - v->xs_mark : 0;

    if (v->xs_wait > spent) {
        v->xs_wait = spent; /* the waits too may span two processors */
    }
    row->wait += v->xs_wait;
    row->handle += spent - v->xs_wait;
}

/** Count in `row` the preparation of `v`'s exit, from `v->xs_prep` to the
 * TSC count `entry`, without the waits for the big lock inside it, as
 * `account_close` counts the handling, of which it is a part. A span to a
 * processor whose TSC reads lower counts as 0, as there. Before
 * `account_close`, which may cut `v->xs_wait`. */
static void prepare_close(struct row *row, const struct kvm_vcpu *v, uint64_t entry)
{
    uint64_t spent, waited;

    if (!v->xs_prep) {
        return;
    }
    spent = entry >= v->xs_prep ? entry - v->xs_prep : 0;
    waited = v->xs_wait - v->xs_prep_wait;
    if (waited > spent) {
        waited = spent;
    }
    row->prepare += spent - waited;
}

void kvm_exitstat_exit(struct kvm_vcpu *v, uint64_t entry, uint64_t left)
{
    uint32_t cpu = arch_cpu_id(), r;
    struct row *mine = rows[cpu];

    cpu_exits[cpu]++;
    if (!mine) {
        v->xs_row = -1;
        v->xs_prep = 0;
        v->xs_in = 0;
        v->xs_out = 0;
        return;
    }
    if (v->xs_row >= 0) {
        prepare_close(&mine[v->xs_row], v, entry);
        account_close(&mine[v->xs_row], v, entry);
        mine[v->xs_row].kernel++;
    }
    r = row_of(v->hw_exit);
    mine[r].exits++;
    if (v->xs_in && v->xs_in >= entry && v->xs_out >= v->xs_in && left >= v->xs_out) {
        mine[r].entry += v->xs_in - entry;
        mine[r].run += v->xs_out - v->xs_in;
        mine[r].exit += left - v->xs_out;
    }
    v->xs_prep = 0;
    v->xs_in = 0;
    v->xs_out = 0;
    v->xs_row = (int32_t)r;
    v->xs_mark = left;
    v->xs_wait = 0;
}

void kvm_exitstat_prepare(struct kvm_vcpu *v)
{
    if (v->xs_row >= 0 && !v->xs_prep) {
        v->xs_prep = arch_cycles();
        v->xs_prep_wait = v->xs_wait;
    }
}

void kvm_exitstat_done(struct kvm_vcpu *v)
{
    uint32_t flags;
    struct row *mine;

    if (v->xs_row < 0) {
        return;
    }
    flags = kirq_save();
    mine = rows[arch_cpu_id()];
    if (mine) {
        account_close(&mine[v->xs_row], v, arch_cycles());
        mine[v->xs_row].user++;
    }
    v->xs_prep = 0;
    v->xs_row = -1;
    kirq_restore(flags);
}

void kvm_bkl_enter(struct kvm_vcpu *v)
{
    uint64_t start, end;

    if (!v || v->xs_row < 0 || bkl_held()) {
        bkl_enter();
        return;
    }
    start = arch_cycles();
    bkl_enter();
    end = arch_cycles();
    if (end > start) {
        v->xs_wait += end - start; /* 0 when the wait moved to a lower TSC */
    }
}

/* A text being written: `len` bytes so far, of which those below `cap`
 * are at `buf`. */
struct out {
    char *buf;
    uint32_t cap, len;
};

/** Append `s`. */
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

/** Append `n` in decimal. */
static void put_dec(struct out *o, uint64_t n)
{
    char d[21];
    int i = 0;

    do {
        d[i++] = (char)('0' + (int)(n % 10u));
        n /= 10u;
    } while (n != 0);
    while (i > 0) {
        char c[2];

        c[0] = d[--i];
        c[1] = '\0';
        put_str(o, c);
    }
}

/** Append `n` in hexadecimal with 0x before it. */
static void put_hex(struct out *o, uint32_t n)
{
    static const char hex[] = "0123456789abcdef";
    char d[11];
    int i = 0;

    do {
        d[i++] = hex[n & 15u];
        n >>= 4;
    } while (n != 0);
    put_str(o, "0x");
    while (i > 0) {
        char c[2];

        c[0] = d[--i];
        c[1] = '\0';
        put_str(o, c);
    }
}

/**
 * kvm/exit_stats's text: a line with the backend and the TSC's rate, a
 * line naming the columns, then one line for each kind with exits: the
 * code, the exits, those handled in the kernel and those that ended
 * KVM_RUN, the cycles of `struct row` from `wait` to `exit`, and the name
 * last, since SVM's exceptions' names have a space. Each count is the sum
 * of the processors' rows.
 */
static int32_t exitstat_show(void *data, char *buf, uint32_t cap)
{
    struct out o;
    uint32_t i, c;

    (void)data;
    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    put_str(&o, "backend ");
    put_str(&o, kvm_backend ? kvm_backend->name : "none");
    put_str(&o, " tsc_khz ");
    put_dec(&o, kvm_tsc_khz());
    put_str(&o, "\ncode exits kernel user wait_cycles handle_cycles prepare_cycles entry_cycles "
                "run_cycles exit_cycles name\n");
    for (i = 0; i < ROWS; i++) {
        struct row sum;
        uint32_t code = code_of(i);
        const char *name = i == ROW_OTHER ? "other" : name_of(code);

        k_memset(&sum, 0, sizeof(sum));
        for (c = 0; c < NR_CPUS; c++) {
            if (rows[c]) {
                sum.exits += rows[c][i].exits;
                sum.kernel += rows[c][i].kernel;
                sum.user += rows[c][i].user;
                sum.wait += rows[c][i].wait;
                sum.handle += rows[c][i].handle;
                sum.prepare += rows[c][i].prepare;
                sum.entry += rows[c][i].entry;
                sum.run += rows[c][i].run;
                sum.exit += rows[c][i].exit;
            }
        }
        if (sum.exits == 0) {
            continue;
        }
        put_hex(&o, code);
        put_str(&o, " ");
        put_dec(&o, sum.exits);
        put_str(&o, " ");
        put_dec(&o, sum.kernel);
        put_str(&o, " ");
        put_dec(&o, sum.user);
        put_str(&o, " ");
        put_dec(&o, sum.wait);
        put_str(&o, " ");
        put_dec(&o, sum.handle);
        put_str(&o, " ");
        put_dec(&o, sum.prepare);
        put_str(&o, " ");
        put_dec(&o, sum.entry);
        put_str(&o, " ");
        put_dec(&o, sum.run);
        put_str(&o, " ");
        put_dec(&o, sum.exit);
        put_str(&o, " ");
        if (name) {
            put_str(&o, name);
        } else {
            put_hex(&o, code);
        }
        put_str(&o, "\n");
    }
    return (int32_t)o.len;
}

/** A write of "0", with a newline or not, clears every count, as a write
 * of 0 clears one of Linux's statistics in its debugfs; anything else is
 * EINVAL. An exit under way when the counts clear still ends its
 * account. */
static int32_t exitstat_store(void *data, const char __user *buf, uint32_t len)
{
    char c[2];
    uint32_t i;

    (void)data;
    if (len == 0 || len > 2) {
        return -EINVAL;
    }
    if (copy_from_user(c, buf, len) != 0) {
        return -EFAULT;
    }
    if (c[0] != '0' || (len == 2 && c[1] != '\n')) {
        return -EINVAL;
    }
    for (i = 0; i < NR_CPUS; i++) {
        if (rows[i]) {
            k_memset(rows[i], 0, ROWS * sizeof(struct row));
        }
    }
    return (int32_t)len;
}

static const struct debugfs_text_ops exitstat_ops = {exitstat_show, exitstat_store};

void kvm_exitstat_cpus(void)
{
    struct row *made;
    uint32_t i;

    for (i = 0; i < NR_CPUS; i++) {
        if (rows[i] || !(cpu_table[i].online || i == arch_cpu_id())) {
            continue;
        }
        made = kzalloc(ROWS * sizeof(struct row));
        if (!made) {
            klog(KLOG_WARN, "kvm", "failed: no memory for processor %u's exit counts", (unsigned)i);
        } else if (rows[i]) {
            kfree(made);
        } else {
            k_barrier();
            rows[i] = made;
        }
    }
}

void kvm_exitstat_start(void)
{
    int32_t r;

    kvm_exitstat_cpus();
    r = debugfs_create_text("kvm/exit_stats", 0644u, &exitstat_ops, NULL);

    if (r != 0) {
        klog(KLOG_WARN, "kvm", "failed: kvm/exit_stats not registered: %d", (int)r);
    }
}
