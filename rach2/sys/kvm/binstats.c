/*
 * binstats.c: the file KVM_GET_STATS_FD gives of a VM or a vCPU, as Linux's
 * binary_stats.c and the descriptors of its x86.c: a header, the object's
 * id, a descriptor for each statistic and the 64-bit values, read at any
 * offset.
 */

#include "kvm.h"

#include "../fs/fd.h"
#include "../fs/file.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"

/* The kinds of statistic Linux's STATS_DESC macros make, all to base 10. */
#define COUNTER (KVM_STATS_TYPE_CUMULATIVE | KVM_STATS_UNIT_NONE)
#define INSTANT (KVM_STATS_TYPE_INSTANT | KVM_STATS_UNIT_NONE)
#define PEAK    (KVM_STATS_TYPE_PEAK | KVM_STATS_UNIT_NONE)
#define BOOLEAN (KVM_STATS_TYPE_INSTANT | KVM_STATS_UNIT_BOOLEAN)
#define TIME    (KVM_STATS_TYPE_CUMULATIVE | KVM_STATS_UNIT_SECONDS)
#define HIST    (KVM_STATS_TYPE_LOG_HIST | KVM_STATS_UNIT_SECONDS)

/* One statistic: `size` values from `index` on among its object's. A time
 * is in nanoseconds, the exponent -9. */
struct stat_def {
    const char *name;
    uint16_t flags;
    uint16_t size;
    uint16_t index;
};

/* A VM's statistics, in the order of Linux's kvm_vm_stats_desc. */
static const struct stat_def vm_defs[] = {
    {"remote_tlb_flush", COUNTER, 1, KVM_VM_STAT_REMOTE_TLB_FLUSH},
    {"remote_tlb_flush_requests", COUNTER, 1, KVM_VM_STAT_REMOTE_TLB_FLUSH_REQUESTS},
    {"mmu_shadow_zapped", COUNTER, 1, KVM_VM_STAT_MMU_SHADOW_ZAPPED},
    {"mmu_pte_write", COUNTER, 1, KVM_VM_STAT_MMU_PTE_WRITE},
    {"mmu_pde_zapped", COUNTER, 1, KVM_VM_STAT_MMU_PDE_ZAPPED},
    {"mmu_flooded", COUNTER, 1, KVM_VM_STAT_MMU_FLOODED},
    {"mmu_recycled", COUNTER, 1, KVM_VM_STAT_MMU_RECYCLED},
    {"mmu_cache_miss", COUNTER, 1, KVM_VM_STAT_MMU_CACHE_MISS},
    {"mmu_unsync", INSTANT, 1, KVM_VM_STAT_MMU_UNSYNC},
    {"pages_4k", INSTANT, 1, KVM_VM_STAT_PAGES_4K},
    {"pages_2m", INSTANT, 1, KVM_VM_STAT_PAGES_2M},
    {"pages_1g", INSTANT, 1, KVM_VM_STAT_PAGES_1G},
    {"nx_lpage_splits", INSTANT, 1, KVM_VM_STAT_NX_LPAGE_SPLITS},
    {"max_mmu_rmap_size", PEAK, 1, KVM_VM_STAT_MAX_MMU_RMAP_SIZE},
    {"max_mmu_page_hash_collisions", PEAK, 1, KVM_VM_STAT_MAX_MMU_PAGE_HASH_COLLISIONS}};

/* A vCPU's, in the order of Linux's kvm_vcpu_stats_desc. */
static const struct stat_def vcpu_defs[] = {
    {"halt_successful_poll", COUNTER, 1, KVM_VCPU_STAT_HALT_SUCCESSFUL_POLL},
    {"halt_attempted_poll", COUNTER, 1, KVM_VCPU_STAT_HALT_ATTEMPTED_POLL},
    {"halt_poll_invalid", COUNTER, 1, KVM_VCPU_STAT_HALT_POLL_INVALID},
    {"halt_wakeup", COUNTER, 1, KVM_VCPU_STAT_HALT_WAKEUP},
    {"halt_poll_success_ns", TIME, 1, KVM_VCPU_STAT_HALT_POLL_SUCCESS_NS},
    {"halt_poll_fail_ns", TIME, 1, KVM_VCPU_STAT_HALT_POLL_FAIL_NS},
    {"halt_wait_ns", TIME, 1, KVM_VCPU_STAT_HALT_WAIT_NS},
    {"halt_poll_success_hist", HIST, KVM_HALT_POLL_HIST, KVM_VCPU_STAT_HALT_POLL_SUCCESS_HIST},
    {"halt_poll_fail_hist", HIST, KVM_HALT_POLL_HIST, KVM_VCPU_STAT_HALT_POLL_FAIL_HIST},
    {"halt_wait_hist", HIST, KVM_HALT_POLL_HIST, KVM_VCPU_STAT_HALT_WAIT_HIST},
    {"blocking", BOOLEAN, 1, KVM_VCPU_STAT_BLOCKING},
    {"pf_taken", COUNTER, 1, KVM_VCPU_STAT_PF_TAKEN},
    {"pf_fixed", COUNTER, 1, KVM_VCPU_STAT_PF_FIXED},
    {"pf_emulate", COUNTER, 1, KVM_VCPU_STAT_PF_EMULATE},
    {"pf_spurious", COUNTER, 1, KVM_VCPU_STAT_PF_SPURIOUS},
    {"pf_fast", COUNTER, 1, KVM_VCPU_STAT_PF_FAST},
    {"pf_mmio_spte_created", COUNTER, 1, KVM_VCPU_STAT_PF_MMIO_SPTE_CREATED},
    {"pf_guest", COUNTER, 1, KVM_VCPU_STAT_PF_GUEST},
    {"tlb_flush", COUNTER, 1, KVM_VCPU_STAT_TLB_FLUSH},
    {"invlpg", COUNTER, 1, KVM_VCPU_STAT_INVLPG},
    {"exits", COUNTER, 1, KVM_VCPU_STAT_EXITS},
    {"io_exits", COUNTER, 1, KVM_VCPU_STAT_IO_EXITS},
    {"mmio_exits", COUNTER, 1, KVM_VCPU_STAT_MMIO_EXITS},
    {"signal_exits", COUNTER, 1, KVM_VCPU_STAT_SIGNAL_EXITS},
    {"irq_window_exits", COUNTER, 1, KVM_VCPU_STAT_IRQ_WINDOW_EXITS},
    {"nmi_window_exits", COUNTER, 1, KVM_VCPU_STAT_NMI_WINDOW_EXITS},
    {"l1d_flush", COUNTER, 1, KVM_VCPU_STAT_L1D_FLUSH},
    {"halt_exits", COUNTER, 1, KVM_VCPU_STAT_HALT_EXITS},
    {"request_irq_exits", COUNTER, 1, KVM_VCPU_STAT_REQUEST_IRQ_EXITS},
    {"irq_exits", COUNTER, 1, KVM_VCPU_STAT_IRQ_EXITS},
    {"host_state_reload", COUNTER, 1, KVM_VCPU_STAT_HOST_STATE_RELOAD},
    {"fpu_reload", COUNTER, 1, KVM_VCPU_STAT_FPU_RELOAD},
    {"insn_emulation", COUNTER, 1, KVM_VCPU_STAT_INSN_EMULATION},
    {"insn_emulation_fail", COUNTER, 1, KVM_VCPU_STAT_INSN_EMULATION_FAIL},
    {"hypercalls", COUNTER, 1, KVM_VCPU_STAT_HYPERCALLS},
    {"irq_injections", COUNTER, 1, KVM_VCPU_STAT_IRQ_INJECTIONS},
    {"nmi_injections", COUNTER, 1, KVM_VCPU_STAT_NMI_INJECTIONS},
    {"req_event", COUNTER, 1, KVM_VCPU_STAT_REQ_EVENT},
    {"nested_run", COUNTER, 1, KVM_VCPU_STAT_NESTED_RUN},
    {"directed_yield_attempted", COUNTER, 1, KVM_VCPU_STAT_DIRECTED_YIELD_ATTEMPTED},
    {"directed_yield_successful", COUNTER, 1, KVM_VCPU_STAT_DIRECTED_YIELD_SUCCESSFUL},
    {"preemption_reported", COUNTER, 1, KVM_VCPU_STAT_PREEMPTION_REPORTED},
    {"preemption_other", COUNTER, 1, KVM_VCPU_STAT_PREEMPTION_OTHER},
    {"guest_mode", BOOLEAN, 1, KVM_VCPU_STAT_GUEST_MODE},
    {"notify_window_exits", COUNTER, 1, KVM_VCPU_STAT_NOTIFY_WINDOW_EXITS}};

/* One open statistics file, of the VM `vm` or of a vCPU of it. It holds a
 * reference to the VM, which keeps the vCPU too. */
struct stats_file {
    struct anon_obj anon; /* `name`, for /proc/PID/fd */
    struct kvm_vm *vm;
    const struct stat_def *defs;
    const uint64_t *values;
    const char *id; /* KVM_STATS_NAME_SIZE bytes */
    uint32_t ndefs;
    uint32_t nvalues;
    char name[32]; /* "kvm-vm-stats" or "kvm-vcpu-stats:ID" */
};

char *kvm_decimal(char *p, uint32_t n)
{
    char digits[10];
    int i = 0;

    do {
        digits[i++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n != 0);
    while (i > 0) {
        *p++ = digits[--i];
    }
    *p = '\0';
    return p;
}

void kvm_stats_id(char *id, const struct kvm_vcpu *v)
{
    char *p;

    k_memset(id, 0, KVM_STATS_NAME_SIZE);
    k_memcpy(id, "kvm-", 4);
    p = kvm_decimal(id + 4, current_proc ? current_proc->pid : 0);
    if (v) {
        k_memcpy(p, "/vcpu-", 6);
        kvm_decimal(p + 6, v->id);
    }
}

/** Copy to `buf`, which takes the file's `count` bytes from `pos` on, what
 * they share with the `len` bytes of `src` that the file has at `at`. 0, or
 * -EFAULT. */
static int32_t part(const void *src, uint32_t at, uint32_t len, uint32_t pos, char __user *buf,
                    uint32_t count)
{
    uint32_t lo = pos > at ? pos : at;
    uint32_t hi = pos + count < at + len ? pos + count : at + len;

    if (lo >= hi) {
        return 0;
    }
    return copy_to_user(buf + (lo - pos), (const char *)src + (lo - at), hi - lo) != 0 ? -EFAULT
                                                                                       : 0;
}

/** The descriptors of `f` that the `count` bytes from `pos` on meet, each
 * made as it is copied. 0, or -EFAULT. */
static int32_t descriptors(const struct stats_file *f, uint32_t at, uint32_t pos, char __user *buf,
                           uint32_t count)
{
    struct kvm_stats_desc d;
    uint32_t i = pos > at ? (pos - at) / (uint32_t)sizeof(d) : 0, n;
    int32_t r = 0;

    for (; r == 0 && i < f->ndefs && at + i * sizeof(d) < pos + count; i++) {
        const struct stat_def *def = &f->defs[i];

        k_memset(&d, 0, sizeof(d));
        d.flags = def->flags | KVM_STATS_BASE_POW10;
        d.exponent = (def->flags & KVM_STATS_UNIT_SECONDS) == KVM_STATS_UNIT_SECONDS ? -9 : 0;
        d.size = def->size;
        d.offset = def->index * 8u;
        for (n = 0; def->name[n] != '\0'; n++) {
            d.name[n] = def->name[n];
        }
        r = part(&d, at + i * (uint32_t)sizeof(d), sizeof(d), pos, buf, count);
    }
    return r;
}

/** Read the file at the description's offset, as Linux's kvm_stats_read:
 * the bytes read, 0 at or past the end, or -EFAULT. */
static int32_t stats_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    const struct stats_file *f = (const struct stats_file *)e->priv;
    struct kvm_stats_header h;
    uint32_t size, pos;
    int32_t r;

    h.flags = 0;
    h.name_size = KVM_STATS_NAME_SIZE;
    h.num_desc = f->ndefs;
    h.id_offset = sizeof(h);
    h.desc_offset = h.id_offset + KVM_STATS_NAME_SIZE;
    h.data_offset = h.desc_offset + f->ndefs * (uint32_t)sizeof(struct kvm_stats_desc);
    size = h.data_offset + f->nvalues * 8u;
    if (e->ofd->offset >= size) {
        return 0;
    }
    pos = (uint32_t)e->ofd->offset;
    if (count > size - pos) {
        count = size - pos;
    }
    r = part(&h, 0, sizeof(h), pos, buf, count);
    if (r == 0) {
        r = part(f->id, h.id_offset, KVM_STATS_NAME_SIZE, pos, buf, count);
    }
    if (r == 0) {
        r = descriptors(f, h.desc_offset, pos, buf, count);
    }
    if (r == 0) {
        r = part(f->values, h.data_offset, f->nvalues * 8u, pos, buf, count);
    }
    if (r != 0) {
        return r;
    }
    e->ofd->offset = pos + count;
    return (int32_t)count;
}

/** A seek moves nothing and gives the offset, as Linux's noop_llseek; it
 * is what lets pread64 read the file. */
static int64_t stats_llseek(fd_entry_t *e, int64_t offset, int whence)
{
    (void)offset;
    (void)whence;
    return (int64_t)e->ofd->offset;
}

/** The last close: the file's reference to the VM goes. */
static void stats_close(fd_entry_t *e)
{
    struct stats_file *f = (struct stats_file *)e->priv;

    e->priv = NULL;
    kvm_vm_put(f->vm);
    kfree(f);
}

static const struct file_ops stats_ops = {stats_read,  NULL, NULL,         NULL,
                                          stats_close, NULL, stats_llseek, 0};

int32_t kvm_stats_fd(struct kvm_vm *vm, struct kvm_vcpu *v)
{
    struct stats_file *f = kzalloc(sizeof(*f));
    fd_entry_t *e;
    int fd;

    if (!f) {
        return -ENOMEM;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    f->vm = vm;
    if (v) {
        f->defs = vcpu_defs;
        f->ndefs = sizeof(vcpu_defs) / sizeof(vcpu_defs[0]);
        f->values = v->stat;
        f->nvalues = KVM_VCPU_STATS;
        f->id = v->stats_id;
        k_memcpy(f->name, "kvm-vcpu-stats:", 15);
        kvm_decimal(f->name + 15, v->id);
    } else {
        f->defs = vm_defs;
        f->ndefs = sizeof(vm_defs) / sizeof(vm_defs[0]);
        f->values = vm->stat;
        f->nvalues = KVM_VM_STATS;
        f->id = vm->stats_id;
        k_memcpy(f->name, "kvm-vm-stats", 13);
    }
    f->anon.name = f->name;
    kvm_vm_get(vm);
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &stats_ops;
    e->priv = f;
    e->flags = FD_FLAG_READ | FD_FLAG_CLOEXEC;
    fd_install(current_proc->fds, fd);
    return fd;
}
