/**
 * Check the numbers SVM's exits give an internal error's report and the
 * emulator (rach2/sys/kvm/svm.c), with the error code of the event under
 * delivery.
 *
 * host-deps: rach2/sys/kvm/decode.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * svm.c is the kernel's, included whole, over stubs of the run loop, the
 * emulator, the local APIC and the processor; decode.c is linked as it is,
 * so that a failed emulation's report is the one the caller reads. Each
 * row sets an exit's code, EXITINFO1, EXITINFO2, EXITINTINFO and its error
 * code, then runs one of the three paths that report them: a nested page
 * fault on MMIO, which gives them to the emulator; a #UD intercept, which
 * gives them to the emulator's #UD path; and an instruction that could not
 * be read at CPL 0, which ends in Linux's emulation failure. Each must give
 * the error code only when EXITINTINFO is valid and has one, as Linux's
 * svm_get_exit_info, and 0 for the error bit alone or the valid bit alone.
 * Beside them, the MSR filter's half in SVM: the permission map's bits
 * and the exits of the guest's filtered RDMSR and WRMSR.
 */
#include "kvm/svm.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);

static int failures;

#define CHECK(condition, detail)                              \
    do {                                                      \
        if (!(condition)) {                                   \
            printf("kvm-svm-hosttest: failed: %s\n", detail); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* ---- the kernel's environment ---- */

unsigned int host_cpu;
void (*k_panic_hook)(void);

/* What the emulator's stubs were given, and which of them ran. */
static uint64_t seen[5];
static int seen_mmio, seen_ud;

/* The MSR rows' filter, on with `filter_on`, which denies the guest's
 * reads of FS_BASE and writes of KERNEL_GS_BASE; the reasons the caller
 * takes, `user_filter` for KVM_MSR_EXIT_REASON_FILTER; and what the
 * instruction's exit did: the caller's exits, their failure and length,
 * the #GPs queued, the bytes fetched for a decode, the bytes skipped. */
static int filter_on, user_filter;
static int user_exits, user_r, gps, fetches;
static uint32_t user_len, skipped;

void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *p = (unsigned char *)ptr;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return ptr;
}

/** Record the numbers the MMIO path was given. */
int kvm_emul_mmio(struct kvm_vcpu *v, const uint64_t info[5])
{
    int i;
    (void)v;
    for (i = 0; i < 5; i++) {
        seen[i] = info[i];
    }
    seen_mmio++;
    return 0;
}

/** Record the numbers the #UD path was given. */
int kvm_emul_ud(struct kvm_vcpu *v, const uint64_t info[5])
{
    int i;
    (void)v;
    for (i = 0; i < 5; i++) {
        seen[i] = info[i];
    }
    seen_ud++;
    return 0;
}

/** Every frame the rows fault on is MMIO. */
int kvm_tdp_fault(struct kvm_vm *vm, uint64_t gfn, int write)
{
    (void)vm, (void)gfn, (void)write;
    return 1;
}

/** The rows run at CPL 0, where a failed emulation is an internal error. */
int kvm_emul_cpl(const struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

/* Stubs for what the rows do not reach. */
void *fpu_area_alloc(void)
{
    return NULL;
}
void fpu_area_free(void *area)
{
    (void)area;
}
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    (void)count, (void)phys;
    return NULL;
}
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    (void)leaf;
    *a = *b = *c = *d = 0;
}
void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
    (void)msr;
    *lo = *hi = 0;
}
void kfree(void *p)
{
    (void)p;
}
void kfree_pages(void *p, uint32_t count)
{
    (void)p, (void)count;
}
void *kzalloc(size_t size)
{
    (void)size;
    return NULL;
}
int smp_cpu_count(void)
{
    return 1;
}
const struct kvm_cpuid_entry2 *kvm_cpuid_find(const struct kvm_vcpu *v, uint32_t function,
                                              uint32_t index)
{
    (void)v, (void)function, (void)index;
    return NULL;
}
uint64_t kvm_cr4_reserved(const struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
int kvm_emul_complete(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
int kvm_emul_cpuid(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
int kvm_emul_fetch(struct kvm_vcpu *v, uint8_t *buf)
{
    (void)v, (void)buf;
    fetches++;
    return 0;
}
uint64_t kvm_emul_hypercall(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
int kvm_emul_mode(const struct kvm_vcpu *v)
{
    (void)v;
    return 64;
}
int kvm_emul_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value)
{
    (void)v, (void)index, (void)write, (void)value;
    return 0;
}
int kvm_emul_pio(struct kvm_vcpu *v, const struct kvm_pio *p)
{
    (void)v, (void)p;
    return 0;
}
int kvm_emul_resume(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
void kvm_emul_skip(struct kvm_vcpu *v, uint32_t len)
{
    (void)v;
    skipped += len;
}
int kvm_emul_xsetbv(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
void kvm_exception_rf(struct kvm_vcpu *v)
{
    (void)v;
}
int kvm_guest_pdptes(struct kvm_vcpu *v, uint64_t cr3, uint64_t pdpte[4])
{
    (void)v, (void)cr3, (void)pdpte;
    return 0;
}
int kvm_guest_rw(struct kvm_vcpu *v, uint32_t seg, uint64_t ea, void *buf, uint32_t n, int write)
{
    (void)v, (void)seg, (void)ea, (void)buf, (void)n, (void)write;
    return 0;
}
int kvm_hv_hypercall(struct kvm_vcpu *v, uint32_t len)
{
    (void)v, (void)len;
    return 0;
}
int kvm_hv_hypercall_enabled(const struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
int kvm_lapic_ack(struct kvm_vcpu *v)
{
    (void)v;
    return -1;
}
uint64_t kvm_lapic_cr8(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
int kvm_lapic_pending(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
void kvm_lapic_set_cr8(struct kvm_vcpu *v, uint64_t cr8)
{
    (void)v, (void)cr8;
}
uint8_t *kvm_map_pages(uint32_t count, uintptr_t *pa)
{
    (void)count, (void)pa;
    return NULL;
}
int kvm_msr_allowed(const struct kvm_vcpu *v, uint32_t index, int write)
{
    (void)v;
    return !filter_on || !(write ? index == MSR_KERNEL_GS_BASE : index == MSR_FS_BASE);
}
int kvm_msr_get(struct kvm_vcpu *v, uint32_t index, uint64_t *value, int host)
{
    (void)v, (void)index, (void)value, (void)host;
    return 0;
}
int kvm_msr_guest_mwait(const struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}
int kvm_msr_set(struct kvm_vcpu *v, uint32_t index, uint64_t value, int host)
{
    (void)v, (void)index, (void)value, (void)host;
    return 0;
}
uint32_t kvm_msr_user_reason(const struct kvm_vcpu *v, int r)
{
    (void)v;
    return user_filter && r == KVM_MSR_FILTERED ? KVM_MSR_EXIT_REASON_FILTER : 0;
}
int kvm_msr_user_exit(struct kvm_vcpu *v, uint32_t index, int write, uint64_t value, int r,
                      uint32_t len)
{
    (void)index, (void)write, (void)value;
    if (!kvm_msr_user_reason(v, r)) {
        return 0;
    }
    v->exit.kind = KVM_X_MSR;
    user_exits++;
    user_r = r;
    user_len = len;
    return 1;
}
void kvm_queue_db(struct kvm_vcpu *v, uint64_t payload)
{
    (void)v, (void)payload;
}
void kvm_queue_exception(struct kvm_vcpu *v, uint8_t nr, int has_error, uint32_t error)
{
    (void)v, (void)has_error, (void)error;
    gps += nr == GP_VECTOR;
}
int kvm_task_switch(struct kvm_vcpu *v, uint16_t sel, int reason, int idt_index, uint64_t next_rip,
                    int64_t error)
{
    (void)v, (void)sel, (void)reason, (void)idt_index, (void)next_rip, (void)error;
    return 0;
}
uint64_t kvm_tdp_root(struct kvm_vm *vm)
{
    (void)vm;
    return 0;
}
void kvm_vcpu_kick(struct kvm_vcpu *v, uint32_t request)
{
    (void)v, (void)request;
}
int kvm_vcpu_test_clear(struct kvm_vcpu *v, uint32_t request)
{
    (void)v, (void)request;
    return 0;
}

/* ---- the rows ---- */

/* One exit's EXITINTINFO, its error code, and the error code the report
 * must give. */
struct event_row {
    uint32_t int_info;
    uint32_t int_err;
    uint32_t want;
    const char *what;
};

static const struct event_row events[] = {
    {0, 0x1234, 0, "no event"},
    {EV_ERR, 0x1234, 0, "the error bit alone"},
    {EV_VALID | (EV_EXCEPTION << 8) | 13u, 0x1234, 0, "a valid event with no error code"},
    {EV_VALID | EV_ERR | (EV_EXCEPTION << 8) | 14u, 0x1234, 0x1234,
     "a valid event with its error code"},
};

/** Set the exit `v` ended with: a nested page fault at a write to 0xFEE00030
 * under event `e`. */
static void set_exit(struct kvm_vcpu *v, const struct event_row *e, uint64_t code)
{
    struct svm_exit *x = &sv(v)->ex;

    x->code = code;
    x->info1 = 0x6;
    x->info2 = 0xFEE00030ull;
    x->int_info = e->int_info;
    x->int_err = e->int_err;
    seen_mmio = seen_ud = 0;
    k_memset(seen, 0xA5, sizeof(seen));
}

/** Whether `info` holds the exit's numbers in svm_get_exit_info's order,
 * with the error code `want`. */
static int numbers(const uint64_t *info, uint64_t code, const struct event_row *e)
{
    return info[0] == code && info[1] == 0x6 && info[2] == 0xFEE00030ull &&
           info[3] == e->int_info && info[4] == e->want;
}

/** Each path that reports an exit, under each event of `events`. */
static void report_rows(struct kvm_vcpu *v)
{
    const uint64_t ud = EXIT_EXCP_BASE + UD_VECTOR;
    uint32_t i;

    for (i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        const struct event_row *e = &events[i];

        set_exit(v, e, EXIT_NPF);
        CHECK(exit_npf(v) == 0 && seen_mmio == 1 && numbers(seen, EXIT_NPF, e), e->what);

        set_exit(v, e, ud);
        CHECK(exit_exception(v, UD_VECTOR) == 0 && seen_ud == 1 && numbers(seen, ud, e), e->what);

        set_exit(v, e, EXIT_NPF);
        emulation_failed(v, NULL);
        CHECK(v->exit.kind == KVM_X_INTERNAL &&
                  v->exit.internal.suberror == KVM_INTERNAL_ERROR_EMULATION &&
                  v->exit.internal.ndata == 6 && numbers(&v->exit.internal.data[1], EXIT_NPF, e),
              e->what);
    }
}

/** A guest's RDMSR or WRMSR of `index` at RIP 0x1000, `next_rip` 0 for a
 * processor without NextRIP save, through `exit_msr`, with the counters
 * cleared first. */
static void msr_exit(struct kvm_vcpu *v, uint32_t index, int write, uint64_t next_rip)
{
    struct svm_exit *x = &sv(v)->ex;

    x->code = EXIT_MSR;
    x->info1 = (uint64_t)write;
    x->next_rip = next_rip;
    v->regs.rip = 0x1000;
    v->regs.rcx = index;
    v->exit.kind = KVM_X_AGAIN;
    user_exits = gps = fetches = 0;
    user_r = 0;
    user_len = skipped = 0;
    CHECK(exit_msr(v) == 0, "exit_msr returns 0");
}

/**
 * The SVM half of KVM_X86_SET_MSR_FILTER, as Linux's svm_msr_filter_changed
 * and svm_handle_msr: the permission map's bits for FS_BASE, GS_BASE and
 * KERNEL_GS_BASE, from byte 2048 + 0x40, the read's bit 2n and the write's
 * 2n + 1, with the byte's other bits kept; a filtered access going to the
 * caller with its length, or #GP when the caller did not ask for the
 * reason, then with no decode; and an access the new filter allows that
 * came under the old map entering again unstepped.
 */
static void filter_rows(struct kvm_vcpu *v, uint8_t *msrpm)
{
    sv(v)->msrpm = msrpm;
    msrpm[2048 + 0x40] = 0xC0;
    filter_on = 1;
    svm_msr_filter_changed(v);
    CHECK(msrpm[2048 + 0x40] == 0xE1,
          "the map intercepts FS_BASE's reads and KERNEL_GS_BASE's writes alone");

    user_filter = 1;
    msr_exit(v, MSR_FS_BASE, 0, 0x1002);
    CHECK(v->exit.kind == KVM_X_MSR && user_exits == 1 && user_r == KVM_MSR_FILTERED &&
              user_len == 2 && gps == 0 && skipped == 0,
          "a filtered RDMSR goes to the caller with its length, unstepped");

    user_filter = 0;
    msr_exit(v, MSR_FS_BASE, 0, 0);
    CHECK(v->exit.kind == KVM_X_AGAIN && user_exits == 0 && gps == 1 && fetches == 0 &&
              skipped == 0,
          "without the reason it is #GP, with no decode on a processor without NextRIP save");

    msr_exit(v, MSR_FS_BASE, 1, 0x1002);
    CHECK(v->exit.kind == KVM_X_AGAIN && gps == 0 && skipped == 0 && user_exits == 0,
          "a write of FS_BASE the filter allows enters again, unstepped, under the new map");

    filter_on = 0;
    svm_msr_filter_changed(v);
    CHECK(msrpm[2048 + 0x40] == 0xC0, "with no filter the map passes all three");
    sv(v)->msrpm = NULL;
}

int main(void)
{
    struct kvm_vcpu *v = calloc(1, sizeof(*v));
    struct svm_vcpu *s = calloc(1, sizeof(*s));
    uint8_t *msrpm = calloc(1, 8192);

    v->backend = s;
    report_rows(v);
    filter_rows(v, msrpm);
    free(msrpm);
    free(s);
    free(v);
    if (failures) {
        printf("kvm-svm-hosttest: %d failure(s)\n", failures);
        return 1;
    }
    printf("kvm-svm-hosttest: ok\n");
    return 0;
}
