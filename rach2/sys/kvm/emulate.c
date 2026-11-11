/*
 * emulate.c: the guest instructions a backend's exits hand to the kernel,
 * the same on VMX and SVM: CPUID, XSETBV, the guest's RDMSR and WRMSR, the
 * task switch, port I/O with its string forms, and the instructions
 * `kvm_decode` decodes, which touch device memory or which the guest's
 * CPUID has and the host lacks, with the walk of the guest's page tables
 * and the reads and writes of guest memory they need.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"

#define CR0_PE 0x00000001ull
#define CR0_WP 0x00010000ull
#define CR0_PG 0x80000000ull

#define CR4_PSE     (1ull << 4)
#define CR4_PAE     (1ull << 5)
#define CR4_OSXSAVE (1ull << 18)
#define CR4_PKE     (1ull << 22)

#define EFER_NXE 0x800ull
#define EFER_LMA 0x400ull

#define RFLAGS_CF    0x0001ull
#define RFLAGS_PF    0x0004ull
#define RFLAGS_AF    0x0010ull
#define RFLAGS_ZF    0x0040ull
#define RFLAGS_SF    0x0080ull
#define RFLAGS_TF    0x0100ull
#define RFLAGS_DF    0x0400ull
#define RFLAGS_OF    0x0800ull
#define RFLAGS_RF    0x10000ull
#define RFLAGS_VM    0x20000ull
#define RFLAGS_ARITH (RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | RFLAGS_OF)

#define DR6_BS         0x4000ull
#define DR6_ACTIVE_LOW 0xFFFF0FF0ull /* the bits an event clears */
#define DR7_GD         0x2000ull

/* The page-table entry bits the walk reads. */
#define GPTE_P    0x001ull
#define GPTE_W    0x002ull
#define GPTE_U    0x004ull
#define GPTE_A    0x020ull
#define GPTE_D    0x040ull
#define GPTE_PS   0x080ull
#define GPTE_NX   (1ull << 63)
#define GPTE_ADDR 0x000FFFFFFFFFF000ull

/* The #PF error code's bits. */
#define PFERR_P 0x01u
#define PFERR_W 0x02u
#define PFERR_U 0x04u
#define PFERR_F 0x10u

#define DB_VECTOR 1
#define UD_VECTOR 6
#define GP_VECTOR 13
#define PF_VECTOR 14

/* The MSRs this file answers for every backend. */
#define MSR_IA32_TSC             0x010u
#define MSR_IA32_PLATFORM_ID     0x017u
#define MSR_EBL_CR_POWERON       0x02Au
#define MSR_IA32_FEATURE_CONTROL 0x03Au
#define MSR_IA32_TSC_ADJUST      0x03Bu
#define MSR_IA32_UCODE_REV       0x08Bu
#define MSR_PLATFORM_INFO        0x0CEu
#define MSR_P6_PERFCTR0          0x0C1u
#define MSR_P6_EVNTSEL0          0x186u
#define MSR_K7_EVNTSEL0          0xC0010000u

/* The registers of particular processors that a guest's kernel reads or
 * writes by the processor's family, with no CPUID bit to check first, and
 * the old performance counters: Linux's KVM reads them as zero or drops
 * their writes, so that the access does not fault (msr_reads_zero and
 * msr_write_dropped). */
#define MSR_IA32_UCODE_WRITE      0x079u
#define MSR_IA32_PERF_CTL         0x199u
#define MSR_IA32_LASTBRANCHFROMIP 0x1DBu /* then LASTBRANCHTOIP, LASTINTFROMIP */
#define MSR_IA32_LASTINTTOIP      0x1DEu
#define MSR_RAPL_POWER_UNIT       0x606u
#define MSR_PKG_ENERGY_STATUS     0x611u
#define MSR_DRAM_ENERGY_STATUS    0x619u
#define MSR_PP0_ENERGY_STATUS     0x639u
#define MSR_PP1_ENERGY_STATUS     0x641u
#define MSR_AMD64_SYSCFG          0xC0010010u
#define MSR_AMD64_NB_CFG          0xC001001Fu
#define MSR_AMD64_PATCH_LOADER    0xC0010020u
#define MSR_K8_INT_PENDING_MSG    0xC0010055u
#define MSR_FAM10H_MMIO_CONF_BASE 0xC0010058u
#define MSR_K8_TSEG_ADDR          0xC0010112u
#define MSR_K8_TSEG_MASK          0xC0010113u
#define MSR_AMD64_DC_CFG          0xC0011022u
#define MSR_AMD64_TW_CFG          0xC0011023u
#define MSR_AMD64_BU_CFG2         0xC001102Au
#define MSR_F15H_EX_CFG           0xC001102Cu

/* IA32_FEATURE_CONTROL as a guest without VMX, SMX or LMCE reads it:
 * locked with nothing enabled, the state Linux's KVM reaches once such a
 * guest's firmware locks it. */
#define FEATURE_CONTROL_LOCKED 0x1ull

/* CPUID leaf 7's EBX bit for IA32_TSC_ADJUST. */
#define CPUID_7_EBX_TSC_ADJUST 0x2u

/* XCR0's components, for XSETBV's checks. */
#define XFEATURE_X87    0x1ull
#define XFEATURE_SSE    0x2ull
#define XFEATURE_YMM    0x4ull
#define XFEATURE_AVX512 0xE0ull

/* The general registers by the processor's number, as indexes of the words
 * of struct kvm_regs: RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8 to R15. */
static const uint8_t reg_slot[16] = {0, 2, 3, 1, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};

/** The general register `n` of `v`, by the processor's number. */
static uint64_t *gpr(struct kvm_vcpu *v, int n)
{
    return &((uint64_t *)&v->regs)[reg_slot[n & 15]];
}

/** The mask of `bytes` bytes: 1, 2, 4 or 8. */
static uint64_t size_mask(uint32_t bytes)
{
    return bytes >= 8u ? ~0ull : (1ull << (bytes * 8u)) - 1u;
}

int kvm_emul_mode(const struct kvm_vcpu *v)
{
    if (!(v->sregs.cr0 & CR0_PE) || (v->regs.rflags & RFLAGS_VM)) {
        return 16;
    }
    if ((v->sregs.efer & EFER_LMA) && v->sregs.cs.l) {
        return 64;
    }
    return v->sregs.cs.db ? 32 : 16;
}

int kvm_emul_cpl(const struct kvm_vcpu *v)
{
    if (!(v->sregs.cr0 & CR0_PE)) {
        return 0;
    }
    if (v->regs.rflags & RFLAGS_VM) {
        return 3;
    }
    return v->sregs.ss.dpl & 3;
}

void kvm_queue_exception(struct kvm_vcpu *v, uint8_t nr, int has_error, uint32_t error)
{
    v->events.exception.injected = 0;
    v->events.exception.pending = 1;
    v->events.exception.nr = nr;
    v->events.exception.has_error_code = (uint8_t)(has_error != 0);
    v->events.exception.error_code = has_error ? error : 0;
    v->dirty |= KVM_PART_EVENTS;
    kvm_vcpu_kick(v, KVM_REQ_EVENT);
}

void kvm_queue_db(struct kvm_vcpu *v, uint64_t payload)
{
    uint64_t dr6 = v->debugregs.dr6;

    dr6 &= ~0xFull;
    dr6 |= DR6_ACTIVE_LOW;
    dr6 |= payload;
    dr6 ^= payload & DR6_ACTIVE_LOW;
    dr6 &= ~(1ull << 12);
    v->debugregs.dr6 = dr6;
    v->debugregs.dr7 &= ~DR7_GD;
    v->dirty |= KVM_PART_DEBUG;
    kvm_queue_exception(v, DB_VECTOR, 0, 0);
}

uint64_t kvm_emul_hypercall(struct kvm_vcpu *v)
{
    uint64_t mask = kvm_emul_mode(v) == 64 ? ~0ull : 0xFFFFFFFFull;
    uint64_t nr = v->regs.rax & mask, a0 = v->regs.rbx & mask, a1 = v->regs.rcx & mask;
    uint64_t a2 = v->regs.rdx & mask, a3 = v->regs.rsi & mask;
    struct kvm_lapic_irq irq;

    v->stat[KVM_VCPU_STAT_HYPERCALLS]++;
    if (kvm_emul_cpl(v) > 0) {
        return (uint64_t)-(int64_t)KVM_EPERM;
    }
    switch (nr) {
    case KVM_HC_VAPIC_POLL_IRQ:
        return 0;
    case KVM_HC_SCHED_YIELD:
        if (!kvm_pv_has(v, KVM_FEATURE_PV_SCHED_YIELD)) {
            break;
        }
        return 0;
    case KVM_HC_KICK_CPU:
        if (!kvm_pv_has(v, KVM_FEATURE_PV_UNHALT)) {
            break;
        }
        if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
            irq.vector = 0;
            irq.delivery_mode = KVM_APIC_DM_REMRD;
            irq.dest_mode = 0;
            irq.level = 0;
            irq.trig_mode = 0;
            irq.shorthand = 0;
            irq.dest_id = (uint32_t)a1;
            irq.redir_hint = 0;
            (void)kvm_lapic_deliver(v->vm, NULL, &irq);
        }
        return 0;
    case KVM_HC_CLOCK_PAIRING:
        return kvm_clock_pairing(v, a0, a1);
    case KVM_HC_SEND_IPI:
        if (!kvm_pv_has(v, KVM_FEATURE_PV_SEND_IPI) || v->vm->irqchip == KVM_IRQCHIP_NONE) {
            break;
        }
        return (uint64_t)kvm_lapic_pv_ipi(v->vm, a0, a1, (uint32_t)a2, a3,
                                          mask == ~0ull ? 64u : 32u);
    default:
        break;
    }
    return (uint64_t)-(int64_t)KVM_ENOSYS;
}

void kvm_exception_rf(struct kvm_vcpu *v)
{
    uint32_t nr = v->events.exception.nr;

    /* Every vector but #DB, NMI, #BP, #OF, #DF and #MC is a fault. */
    if (v->events.exception.pending && nr < 32u && !((1ul << nr) & 0x4011Eul)) {
        v->regs.rflags |= RFLAGS_RF;
        v->dirty |= KVM_PART_REGS;
    }
}

/** Raise the single-step trap after an instruction the kernel ran, with
 * `tf` the trap flag that counts: #DB with DR6.BS for the guest, or under
 * the caller's single step `emul.step_trap`, for the backend to end the
 * exit as KVM_EXIT_DEBUG, as Linux's kvm_vcpu_do_singlestep. */
static void step_trap(struct kvm_vcpu *v, int tf)
{
    if ((v->guest_debug.control & KVM_GUESTDBG_ENABLE) &&
        (v->guest_debug.control & KVM_GUESTDBG_SINGLESTEP)) {
        v->emul.step_trap = 1;
    } else if (tf) {
        kvm_queue_db(v, DR6_BS);
    }
}

void kvm_emul_skip(struct kvm_vcpu *v, uint32_t len)
{
    uint64_t rip = v->regs.rip + len;

    v->regs.rip = kvm_emul_mode(v) == 64 ? rip : (rip & 0xFFFFFFFFull);
    if (v->events.interrupt.shadow) {
        /* The backend read `irq_allowed` under the shadow this skip ends:
         * after `sti; hlt` the halted vCPU takes an interrupt at once. */
        v->events.interrupt.shadow = 0;
        v->irq_allowed = v->if_flag;
        v->dirty |= KVM_PART_EVENTS;
    }
    step_trap(v, (v->regs.rflags & RFLAGS_TF) != 0);
}

/* ---- guest memory ---- */

/**
 * Read or write, by `write`, the `n` bytes of `v`'s guest-physical memory
 * at `gpa` through the caller's memory that the slots map, in the vCPU's
 * thread. 0; 1 when a byte is in no slot or a write meets a read-only
 * slot, which is MMIO; -EFAULT when the caller's memory cannot be touched.
 */
static int gpa_rw(struct kvm_vcpu *v, uint64_t gpa, void *buf, uint32_t n, int write)
{
    struct kvm_slot_view s;
    uint8_t *p = (uint8_t *)buf;
    uint32_t chunk;

    while (n) {
        chunk = 4096u - (uint32_t)(gpa & 0xFFFu);
        if (chunk > n) {
            chunk = n;
        }
        if (!kvm_slot_find(v->vm, gpa >> 12, &s)) {
            return 1;
        }
        if (write && (s.flags & KVM_MEM_READONLY)) {
            return 1;
        }
        if (write) {
            if (copy_to_user((void __user *)(uintptr_t)(s.hva + (gpa & 0xFFFu)), p, chunk) != 0) {
                return -EFAULT;
            }
            kvm_dirty_mark(v->vm, gpa >> 12);
        } else if (copy_from_user(p, (const void __user *)(uintptr_t)(s.hva + (gpa & 0xFFFu)),
                                  chunk) != 0) {
            return -EFAULT;
        }
        p += chunk;
        gpa += chunk;
        n -= chunk;
    }
    return 0;
}

int kvm_guest_phys_rw(struct kvm_vcpu *v, uint64_t gpa, void *buf, uint32_t n, int write)
{
    return gpa_rw(v, gpa, buf, n, write);
}

/** One entry of the guest's tables at `gpa`, `size` 4 or 8 bytes: 0, or
 * nonzero as `gpa_rw` answers. */
static int pte_read(struct kvm_vcpu *v, uint64_t gpa, uint32_t size, uint64_t *e)
{
    *e = 0;
    return gpa_rw(v, gpa, e, size, 0);
}

/** Queue the #PF of a walk of `la` that failed with `error`. */
static int walk_fault(struct kvm_vcpu *v, uint64_t la, uint32_t error)
{
    v->sregs.cr2 = la;
    kvm_queue_exception(v, PF_VECTOR, 1, error);
    return -1;
}

/* A walk of the guest's page tables under way: the entry size, the levels
 * and the entries read at each, by level, 1 the lowest. */
struct walk {
    uint32_t size; /* 4 or 8 bytes an entry */
    uint32_t top;  /* the level of the first table read */
    uint64_t table;
    uint64_t gpa[5];
    uint64_t entry[5];
};

/** The bits of an index at each level of `w`'s tables. */
static uint32_t walk_bits(const struct walk *w)
{
    return w->size == 8u ? 9u : 10u;
}

/**
 * Begin the walk of `la` in `v`'s paging mode: the entry size, the top
 * level and its table, through PAE's PDPTE where it pages so. 0, or as
 * `kvm_guest_translate` answers; `*la` is cut to 32 bits outside long mode.
 */
static int walk_start(struct kvm_vcpu *v, uint64_t *la, uint32_t error, struct walk *w)
{
    uint64_t e;
    int r;

    w->size = 8;
    w->top = 4;
    if (v->sregs.efer & EFER_LMA) {
        w->table = v->sregs.cr3 & GPTE_ADDR;
        return 0;
    }
    *la &= 0xFFFFFFFFull;
    w->top = 2;
    if (!(v->sregs.cr4 & CR4_PAE)) {
        w->size = 4;
        w->table = v->sregs.cr3 & 0xFFFFF000ull;
        return 0;
    }
    r = pte_read(v, (v->sregs.cr3 & 0xFFFFFFE0ull) + ((*la >> 30) & 3u) * 8u, 8, &e);
    if (r != 0) {
        return r;
    }
    if (!(e & GPTE_P)) {
        return walk_fault(v, *la, error);
    }
    w->table = e & GPTE_ADDR;
    return 0;
}

/** Whether the entry `e` at `level` of `w` maps a page itself: the lowest
 * level, or PS on a 2 MiB or 4 MiB entry, or a 1 GiB one in 8-byte tables,
 * with CR4.PSE for 4-byte tables. */
static int walk_leaf(const struct walk *w, uint64_t cr4, uint32_t level, uint64_t e)
{
    if (level == 1) {
        return 1;
    }
    if (!(e & GPTE_PS)) {
        return 0;
    }
    if (w->size == 8u) {
        return level == 2 || level == 3;
    }
    return level == 2 && (cr4 & CR4_PSE) != 0;
}

/** Set the accessed bits of the entries `w` used down to `leaf`, and the
 * dirty bit of the leaf for a `write`, as the processor does. */
static void walk_mark(struct kvm_vcpu *v, const struct walk *w, uint32_t leaf, int write)
{
    uint32_t level;
    uint64_t e;

    for (level = w->top; level >= leaf && level >= 1u; level--) {
        e = w->entry[level] | GPTE_A;
        if (level == leaf && write) {
            e |= GPTE_D;
        }
        if (e != w->entry[level]) {
            (void)gpa_rw(v, w->gpa[level], &e, w->size, 1);
        }
    }
}

/** The guest-physical address of `la` that the leaf at `leaf` of `w`
 * maps. */
static uint64_t walk_address(const struct walk *w, uint32_t leaf, uint64_t la)
{
    uint64_t e = w->entry[leaf], ea;
    uint32_t shift = 12u + (leaf - 1u) * walk_bits(w);

    if (leaf == 1u) {
        ea = (w->size == 8u ? e : (e & 0xFFFFFFFFull)) & GPTE_ADDR;
    } else if (w->size == 8u) {
        ea = e & GPTE_ADDR & ~((1ull << shift) - 1u);
    } else {
        /* A 4 MiB page, with PSE-36's address bits 32 to 39 in 13 to 20. */
        ea = (e & 0xFFC00000ull) | ((e & 0x1FE000ull) << 19);
    }
    return ea | (la & ((1ull << shift) - 1u));
}

/**
 * Translate the guest's linear address `la` for an access of `v`, with
 * `access` the #PF error code's W, U and I/D bits, through the guest's
 * page tables in its paging mode, setting the accessed bits of the entries
 * used and the dirty bit of a written leaf, as the processor does. 0 with
 * the guest-physical address in `*gpa`; -1 with #PF queued; 1 when a table
 * is in no slot; -EFAULT.
 */
int kvm_guest_translate(struct kvm_vcpu *v, uint64_t la, uint32_t access, uint64_t *gpa)
{
    struct walk w;
    uint64_t e;
    uint32_t level, shift, i, error = access & (PFERR_W | PFERR_U | PFERR_F);
    int user = (access & PFERR_U) != 0, write = (access & PFERR_W) != 0;
    int fetch = (access & PFERR_F) != 0, r;
    int wp = (v->sregs.cr0 & CR0_WP) != 0, nxe;

    if (!(v->sregs.cr0 & CR0_PG)) {
        *gpa = la & 0xFFFFFFFFull;
        return 0;
    }
    r = walk_start(v, &la, error, &w);
    if (r != 0) {
        return r;
    }
    nxe = w.size == 8u && (v->sregs.efer & EFER_NXE) != 0;
    for (level = w.top; level >= 1u; level--) {
        shift = 12u + (level - 1u) * walk_bits(&w);
        i = (uint32_t)((la >> shift) & ((1u << walk_bits(&w)) - 1u));
        w.gpa[level] = w.table + (uint64_t)i * w.size;
        r = pte_read(v, w.gpa[level], w.size, &e);
        if (r != 0) {
            return r;
        }
        w.entry[level] = e;
        if (!(e & GPTE_P)) {
            return walk_fault(v, la, error);
        }
        if ((user && !(e & GPTE_U)) || (write && !(e & GPTE_W) && (user || wp)) ||
            (fetch && nxe && (e & GPTE_NX))) {
            return walk_fault(v, la, error | PFERR_P);
        }
        if (walk_leaf(&w, v->sregs.cr4, level, e)) {
            break;
        }
        w.table = (w.size == 8u ? e : (e & 0xFFFFFFFFull)) & GPTE_ADDR;
    }
    walk_mark(v, &w, level, write);
    *gpa = walk_address(&w, level, la);
    return 0;
}

/** The base of the segment `seg` (KVM_SEG_*) of `v`. */
static uint64_t seg_base(const struct kvm_vcpu *v, uint32_t seg)
{
    const struct kvm_segment *s[6];

    s[0] = &v->sregs.es;
    s[1] = &v->sregs.cs;
    s[2] = &v->sregs.ss;
    s[3] = &v->sregs.ds;
    s[4] = &v->sregs.fs;
    s[5] = &v->sregs.gs;
    if (seg > 5u) {
        return 0;
    }
    if (kvm_emul_mode(v) == 64 && seg != KVM_SEG_FS && seg != KVM_SEG_GS) {
        return 0;
    }
    return s[seg]->base;
}

/** The linear address of `seg`:`ea` in `v`'s mode. */
static uint64_t linear(const struct kvm_vcpu *v, uint32_t seg, uint64_t ea)
{
    uint64_t la = seg_base(v, seg) + ea;

    return kvm_emul_mode(v) == 64 ? la : (la & 0xFFFFFFFFull);
}

/** The #PF error code's access bits for `v`: U at CPL 3. */
static uint32_t access_bits(const struct kvm_vcpu *v, int write, int fetch)
{
    return (write ? PFERR_W : 0u) | (kvm_emul_cpl(v) == 3 ? PFERR_U : 0u) | (fetch ? PFERR_F : 0u);
}

int kvm_guest_rw(struct kvm_vcpu *v, uint32_t seg, uint64_t ea, void *buf, uint32_t n, int write)
{
    uint8_t *p = (uint8_t *)buf;
    uint64_t la = linear(v, seg, ea), gpa;
    uint32_t chunk;
    int r;

    while (n) {
        chunk = 4096u - (uint32_t)(la & 0xFFFu);
        if (chunk > n) {
            chunk = n;
        }
        r = kvm_guest_translate(v, la, access_bits(v, write, 0), &gpa);
        if (r != 0) {
            return r;
        }
        r = gpa_rw(v, gpa, p, chunk, write);
        if (r != 0) {
            return r;
        }
        p += chunk;
        la += chunk;
        n -= chunk;
    }
    return 0;
}

/**
 * The bytes of the instruction at `v`'s CS:RIP, up to KVM_INSN_MAX, into
 * `buf`: the count read, fewer where the next page cannot be read; -1 with
 * #PF queued when the first byte cannot be; 0 when it is in no slot.
 */
static int fetch(struct kvm_vcpu *v, uint8_t *buf)
{
    uint64_t la = linear(v, KVM_SEG_CS, v->regs.rip), gpa;
    uint32_t got = 0, chunk;
    int r;

    while (got < KVM_INSN_MAX) {
        chunk = 4096u - (uint32_t)(la & 0xFFFu);
        if (chunk > KVM_INSN_MAX - got) {
            chunk = KVM_INSN_MAX - got;
        }
        r = kvm_guest_translate(v, la, access_bits(v, 0, 1), &gpa);
        if (r < 0 && got == 0) {
            return r == -1 ? -1 : 0;
        }
        if (r != 0 || gpa_rw(v, gpa, buf + got, chunk, 0) != 0) {
            if (r == -1) {
                /* The walk of the second page queued a #PF the first
                 * page's bytes may not need; the decoder judges. */
                v->events.exception.pending = 0;
            }
            break;
        }
        got += chunk;
        la += chunk;
    }
    return (int)got;
}

/** Read the current instruction for a backend, with fetch's fault contract. */
int kvm_emul_fetch(struct kvm_vcpu *v, uint8_t *buf)
{
    return fetch(v, buf);
}

/* ---- CPUID, XSETBV and MSRs ---- */

/** Whether leaf 0 `basic` names AMD or Hygon, "AuthenticAMD" or
 * "HygonGenuine" in EBX, EDX and ECX. */
static int cpuid_vendor_amd(const struct kvm_cpuid_entry2 *basic)
{
    return (basic->ebx == 0x68747541u && basic->edx == 0x69746E65u && basic->ecx == 0x444D4163u) ||
           (basic->ebx == 0x6F677948u && basic->edx == 0x6E65476Eu && basic->ecx == 0x656E6975u);
}

int kvm_guest_amd(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *basic = kvm_cpuid_find(v, 0, 0);

    return basic && cpuid_vendor_amd(basic);
}

/**
 * The entry a CPUID of `*function` that `v`'s table lacks reads, as Linux's
 * get_out_of_range_cpuid_entry: past the highest leaf of its range, the
 * highest basic leaf, whose number goes in `*function`, as on Intel's
 * processors; NULL, for zeros, inside the range or for a guest whose vendor
 * is AMD or Hygon, as on theirs. The ranges are the basic, the hypervisor's
 * of 256 leaves from 0x40000000, the extended and Centaur's from 0xC0000000.
 */
static const struct kvm_cpuid_entry2 *cpuid_out_of_range(const struct kvm_vcpu *v,
                                                         uint32_t *function, uint32_t index)
{
    const struct kvm_cpuid_entry2 *basic = kvm_cpuid_find(v, 0, 0), *class;
    uint32_t f = *function;

    if (!basic || cpuid_vendor_amd(basic)) {
        return NULL;
    }
    if (f >= 0x40000000u && f <= 0x4FFFFFFFu) {
        class = kvm_cpuid_find(v, f & 0xFFFFFF00u, 0);
    } else if (f >= 0xC0000000u) {
        class = kvm_cpuid_find(v, 0xC0000000u, 0);
    } else {
        class = kvm_cpuid_find(v, f & 0x80000000u, 0);
    }
    if (class && f <= class->eax) {
        return NULL;
    }
    *function = basic->eax;
    return kvm_cpuid_find(v, basic->eax, index);
}

int kvm_emul_cpuid(struct kvm_vcpu *v)
{
    uint32_t function = (uint32_t)v->regs.rax, index = (uint32_t)v->regs.rcx;
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, function, index);
    uint32_t a = 0, b = 0, c = 0, d = 0;

    if ((v->misc_features & 1u) && kvm_emul_cpl(v) != 0) {
        kvm_queue_exception(v, GP_VECTOR, 1, 0);
        return -1;
    }
    if (!e) {
        e = cpuid_out_of_range(v, &function, index);
    }
    if (!e && (function == 0xBu || function == 0x1Fu)) {
        /* An undefined subleaf of a topology leaf that has subleaf 1 gives
         * CL back and the x2APIC id in EDX, as Linux's kvm_cpuid. */
        e = kvm_cpuid_find(v, function, 1);
        if (e) {
            c = index & 0xFFu;
            d = e->edx;
            e = NULL;
        }
    }
    if (e) {
        a = e->eax;
        b = e->ebx;
        c = e->ecx;
        d = e->edx;
        /* The bits that follow the guest's own state, as Linux's
         * kvm_update_cpuid_runtime keeps them. */
        if (function == 1) {
            c = (v->sregs.cr4 & CR4_OSXSAVE) ? (c | (1u << 27)) : (c & ~(1u << 27));
            if (!(v->sregs.apic_base & 0x800u)) {
                d &= ~(1u << 9);
            }
            if (!KVM_QUIRK(v->vm, KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT)) {
                c = kvm_msr_guest_mwait(v) ? (c | (1u << 3)) : (c & ~(1u << 3));
            }
        } else if (function == 7 && index == 0) {
            c = (v->sregs.cr4 & CR4_PKE) ? (c | (1u << 4)) : (c & ~(1u << 4));
        } else if (function == 0x80000007u && kvm_hv_invtsc_suppressed(v)) {
            d &= ~(1u << 8);
        }
    }
    v->regs.rax = a;
    v->regs.rbx = b;
    v->regs.rcx = c;
    v->regs.rdx = d;
    return 0;
}

int kvm_emul_xsetbv(struct kvm_vcpu *v)
{
    uint64_t xcr0 = (v->regs.rax & 0xFFFFFFFFull) | (v->regs.rdx << 32);
    uint64_t valid = kvm_xcr0_guest(v);

    /* Linux's __kvm_set_xcr: XCR0 alone, x87 always, AVX with SSE, the
     * three AVX-512 components together and with AVX, nothing the guest's
     * CPUID does not offer. */
    if ((uint32_t)v->regs.rcx != 0 || !(xcr0 & XFEATURE_X87) || (xcr0 & ~valid) ||
        ((xcr0 & XFEATURE_YMM) && !(xcr0 & XFEATURE_SSE)) ||
        ((xcr0 & XFEATURE_AVX512) &&
         ((xcr0 & XFEATURE_AVX512) != XFEATURE_AVX512 || !(xcr0 & XFEATURE_YMM)))) {
        kvm_queue_exception(v, GP_VECTOR, 1, 0);
        return -1;
    }
    v->xcr0 = xcr0;
    kvm_cpuid_xsave_sizes(v, v->cpuid, v->cpuid_nent);
    v->dirty |= KVM_PART_FPU;
    return 0;
}

/** Whether `index` is a performance counter or event select of the P6's
 * two or the K7's four, which a guest without a PMU reads as 0 and writes
 * to no effect, as Linux's kvm_get_msr_common and kvm_set_msr_common have
 * them; a guest's kernel clears them at boot whatever its CPUID says. */
static int perf_msr(uint32_t index)
{
    return (index >= MSR_P6_PERFCTR0 && index < MSR_P6_PERFCTR0 + 2u) ||
           (index >= MSR_P6_EVNTSEL0 && index < MSR_P6_EVNTSEL0 + 2u) ||
           (index >= MSR_K7_EVNTSEL0 && index < MSR_K7_EVNTSEL0 + 8u);
}

/**
 * Whether a guest reads the MSR `index` as zero, as Linux's
 * kvm_get_msr_common answers it: the identity MSRs, the last-branch
 * records, AMD's configuration registers, the performance state control,
 * the RAPL energy counters, and the old performance counters of a VM
 * without a PMU. A Linux guest on an AMD processor reads SYSCFG with its
 * MTRRs, and a #GP there puts a warning and a stack trace in its log.
 */
static int msr_reads_zero(uint32_t index)
{
    if (perf_msr(index) || (index >= MSR_IA32_LASTBRANCHFROMIP && index <= MSR_IA32_LASTINTTOIP)) {
        return 1;
    }
    switch (index) {
    case MSR_IA32_PLATFORM_ID:
    case MSR_EBL_CR_POWERON:
    case MSR_IA32_PERF_CTL:
    case MSR_RAPL_POWER_UNIT:
    case MSR_PKG_ENERGY_STATUS:
    case MSR_DRAM_ENERGY_STATUS:
    case MSR_PP0_ENERGY_STATUS:
    case MSR_PP1_ENERGY_STATUS:
    case MSR_AMD64_SYSCFG:
    case MSR_AMD64_NB_CFG:
    case MSR_K8_INT_PENDING_MSG:
    case MSR_FAM10H_MMIO_CONF_BASE:
    case MSR_K8_TSEG_ADDR:
    case MSR_K8_TSEG_MASK:
    case MSR_AMD64_DC_CFG:
    case MSR_AMD64_TW_CFG:
    case MSR_AMD64_BU_CFG2:
    case MSR_F15H_EX_CFG:
        return 1;
    default:
        return 0;
    }
}

/**
 * Whether a guest's write of `value` to the MSR `index` is taken and
 * dropped, as Linux's kvm_set_msr_common drops it: a microcode update,
 * AMD's configuration registers, the old performance counters of a VM
 * without a PMU, and a zero for the MMIO configuration base. A Linux
 * guest on an AMD processor sets a bit of NB_CFG on each processor it
 * brings up.
 */
static int msr_write_dropped(uint32_t index, uint64_t value)
{
    if (perf_msr(index)) {
        return 1;
    }
    switch (index) {
    case MSR_IA32_UCODE_WRITE:
    case MSR_AMD64_NB_CFG:
    case MSR_AMD64_PATCH_LOADER:
    case MSR_AMD64_DC_CFG:
    case MSR_AMD64_TW_CFG:
    case MSR_AMD64_BU_CFG2:
    case MSR_F15H_EX_CFG:
        return 1;
    case MSR_FAM10H_MMIO_CONF_BASE:
        return value == 0;
    default:
        return 0;
    }
}

/**
 * A guest's write of `value` to IA32_TSC or IA32_TSC_ADJUST, as Linux's
 * kvm_set_msr_common: a write of the adjustment moves the TSC by what it
 * changes, and is ignored when the guest's CPUID lacks TSC_ADJUST; a write
 * of the TSC moves the adjustment by what the TSC moved, and matches no
 * other vCPU's, as the caller's write may. Either writes the kvmclock
 * again. 0, or -1 for #GP.
 */
static int tsc_write(struct kvm_vcpu *v, uint32_t index, uint64_t value)
{
    const struct kvm_cpuid_entry2 *e;
    uint64_t adjust = 0, before = (uint64_t)v->tsc_offset;

    (void)kvm_msr_get(v, MSR_IA32_TSC_ADJUST, &adjust, 0);
    if (index == MSR_IA32_TSC_ADJUST) {
        e = kvm_cpuid_find(v, 7, 0);
        if (!e || !(e->ebx & CPUID_7_EBX_TSC_ADJUST)) {
            return 0;
        }
        if (kvm_msr_set(v, index, value, 0) != 0) {
            return KVM_MSR_INVAL;
        }
        v->tsc_offset = (int64_t)(before + (value - adjust));
        v->dirty |= KVM_PART_TSC;
        kvm_tsc_moved(v);
        return 0;
    }
    v->tsc_offset = (int64_t)(value - kvm_tsc_scale(v, arch_cycles()));
    v->dirty |= KVM_PART_TSC;
    kvm_tsc_moved(v);
    return kvm_msr_set(v, MSR_IA32_TSC_ADJUST, adjust + ((uint64_t)v->tsc_offset - before), 0);
}

int kvm_emul_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value)
{
    int r;

    r = kvm_lapic_msr(v, index, write, value, 0);
    if (r == 1) {
        r = kvm_hv_msr(v, index, write, value, 0);
    }
    if (r == 1) {
        r = kvm_pv_msr(v, index, write, value, 0);
    }
    if (r != 1) {
        return r;
    }
    if (write ? msr_write_dropped(index, *value) : msr_reads_zero(index)) {
        if (!write) {
            *value = 0;
        }
        return 0;
    }
    switch (index) {
    case MSR_IA32_FEATURE_CONTROL:
        if (write) {
            return -1;
        }
        *value = FEATURE_CONTROL_LOCKED;
        return 0;
    case MSR_IA32_UCODE_REV:
        /* Linux's guests write 0 before reading the revision, and a
         * guest's write changes nothing, as on Linux. */
        if (!write) {
            *value = v->ucode_rev;
        }
        return 0;
    default:
        break;
    }
    if (write && (index == MSR_IA32_TSC || index == MSR_IA32_TSC_ADJUST)) {
        return tsc_write(v, index, *value);
    }
    if (index == MSR_PLATFORM_INFO && (write || v->vm->platform_info_hidden)) {
        return KVM_MSR_INVAL;
    }
    return write ? kvm_msr_set(v, index, *value, 0) : kvm_msr_get(v, index, value, 0);
}

/* ---- PAE's PDPTEs ---- */

int kvm_guest_pdptes(struct kvm_vcpu *v, uint64_t cr3, uint64_t pdpte[4])
{
    uint64_t reserved = (~0ull << kvm_guest_maxphyaddr(v)) | 0x1E6ull;
    int i;

    if (kvm_guest_phys_rw(v, cr3 & 0xFFFFFFE0ull, pdpte, 4u * 8u, 0) != 0) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        if ((pdpte[i] & GPTE_P) && (pdpte[i] & reserved)) {
            return -1;
        }
    }
    return 0;
}

/* ---- the task switch ---- */

#define CR0_TS    0x00000008ull
#define RFLAGS_NT 0x4000ull
#define TS_VECTOR 10
#define NP_VECTOR 11
#define SS_VECTOR 12

/* DR7's local enables L0 to L3 and LE, which a task switch clears. */
#define DR7_LOCAL 0x155ull

/* The LDTR's index among the segments a TSS holds, after ES to GS. */
#define TSS_LDT 6

/* A descriptor of the GDT or LDT: its bytes and its linear address. */
struct desc {
    uint8_t b[8];
    uint64_t la;
};

#define DESC_TYPE(d) ((uint32_t)(d)->b[5] & 0xFu)
#define DESC_S(d)    (((uint32_t)(d)->b[5] >> 4) & 1u)
#define DESC_DPL(d)  (((uint32_t)(d)->b[5] >> 5) & 3u)
#define DESC_P(d)    ((uint32_t)(d)->b[5] >> 7)

/* Where a TSS keeps each field, by its kind: its size; the width of a
 * register; the offsets of EIP, EFLAGS, the general registers by the
 * processor's number, the selectors from ES on, of which there are `nsels`,
 * the LDT's selector and CR3 (0 for none); and the span a switch away from
 * the task writes, as Linux's task_switch_32 and task_switch_16. */
struct tss_layout {
    uint32_t size, width;
    uint32_t ip, flags, regs, sels, nsels, ldt, cr3;
    uint32_t save_from, save_to;
};

static const struct tss_layout tss32 = {104, 4, 32, 36, 40, 72, 6, 96, 28, 32, 96};
static const struct tss_layout tss16 = {44, 2, 14, 16, 18, 34, 4, 42, 0, 0, 44};

/* A task switch under way: the descriptors of the new and the current
 * TSS, their kinds, the new TSS's base, the current TR's selector, and both
 * TSSes as read. */
struct task_switch {
    struct desc next, cur;
    const struct tss_layout *l, *lo;
    uint64_t new_base;
    uint16_t old_sel;
    uint8_t new_tss[104], old_tss[104];
};

/** The `n`-byte little-endian value at `p`, `n` 2 or 4. */
static uint32_t get_le(const uint8_t *p, uint32_t n)
{
    uint32_t x = (uint32_t)p[0] | ((uint32_t)p[1] << 8);

    return n == 4u ? x | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24) : x;
}

/** Store `x` at `p` in `n` bytes, little-endian, `n` 2 or 4. */
static void put_le(uint8_t *p, uint32_t n, uint32_t x)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)(x >> (i * 8u));
    }
}

/** Queue the fault `nr` with `error` for a task switch: -1. */
static int ts_fault(struct kvm_vcpu *v, uint8_t nr, uint32_t error)
{
    kvm_queue_exception(v, nr, 1, error);
    return -1;
}

/** Read or write `n` bytes of `v`'s linear memory at `la` as the processor
 * touches its own tables and TSSes, at privilege 0: 0, or as
 * `kvm_guest_rw` answers. */
static int system_rw(struct kvm_vcpu *v, uint64_t la, void *buf, uint32_t n, int write)
{
    uint8_t *p = (uint8_t *)buf;
    uint64_t gpa;
    uint32_t chunk;
    int r;

    while (n) {
        chunk = 4096u - (uint32_t)(la & 0xFFFu);
        if (chunk > n) {
            chunk = n;
        }
        r = kvm_guest_translate(v, la, write ? PFERR_W : 0u, &gpa);
        if (r == 0) {
            r = gpa_rw(v, gpa, p, chunk, write);
        }
        if (r != 0) {
            return r;
        }
        p += chunk;
        la += chunk;
        n -= chunk;
    }
    return 0;
}

/** The segment register `n` of `v`: KVM_SEG_ES to KVM_SEG_GS, or TSS_LDT. */
static struct kvm_segment *task_seg(struct kvm_vcpu *v, int n)
{
    struct kvm_segment *s[7];

    s[0] = &v->sregs.es;
    s[1] = &v->sregs.cs;
    s[2] = &v->sregs.ss;
    s[3] = &v->sregs.ds;
    s[4] = &v->sregs.fs;
    s[5] = &v->sregs.gs;
    s[6] = &v->sregs.ldt;
    return s[n];
}

/** Make `s` the segment of the selector `sel` that `d` describes, its
 * limit scaled by G. */
static void desc_seg(struct kvm_segment *s, uint16_t sel, const struct desc *d)
{
    const uint8_t *b = d->b;
    uint32_t limit = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | (((uint32_t)b[6] & 0xFu) << 16);

    k_memset(s, 0, sizeof(*s));
    s->selector = sel;
    s->base =
        (uint32_t)b[2] | ((uint32_t)b[3] << 8) | ((uint32_t)b[4] << 16) | ((uint32_t)b[7] << 24);
    s->limit = (b[6] & 0x80u) ? (limit << 12) | 0xFFFu : limit;
    s->type = (uint8_t)DESC_TYPE(d);
    s->s = (uint8_t)DESC_S(d);
    s->dpl = (uint8_t)DESC_DPL(d);
    s->present = (uint8_t)DESC_P(d);
    s->avl = (uint8_t)((b[6] >> 4) & 1u);
    s->l = (uint8_t)((b[6] >> 5) & 1u);
    s->db = (uint8_t)((b[6] >> 6) & 1u);
    s->g = (uint8_t)(b[6] >> 7);
    s->unusable = (uint8_t)!s->present;
}

/**
 * Read the descriptor of `sel` from `v`'s GDT or, with TI, its LDT, as
 * Linux's read_segment_descriptor: 0 with `d` and its address; -1 with
 * #GP(`sel`) queued past the table's limit or with no LDT loaded; 1 or
 * -EFAULT as `system_rw`.
 */
static int desc_read(struct kvm_vcpu *v, uint16_t sel, struct desc *d)
{
    uint64_t base = v->sregs.gdt.base;
    uint32_t limit = v->sregs.gdt.limit, index = (uint32_t)sel >> 3;

    if (sel & 4u) {
        base = v->sregs.ldt.base;
        limit = v->sregs.ldt.unusable ? 0 : v->sregs.ldt.limit;
    }
    if (limit < index * 8u + 7u) {
        return ts_fault(v, GP_VECTOR, sel & 0xFFFCu);
    }
    d->la = base + index * 8u;
    if (!(v->sregs.efer & EFER_LMA)) {
        d->la &= 0xFFFFFFFFull;
    }
    return system_rw(v, d->la, d->b, 8, 0);
}

/** Write `d`'s access byte, its type, S, DPL and P, back to the table. */
static int desc_write_type(struct kvm_vcpu *v, const struct desc *d)
{
    uint8_t t = d->b[5];

    return system_rw(v, d->la + 5u, &t, 1, 1);
}

/**
 * The fault, #TS, #NP or #SS, by which a task switch refuses the
 * descriptor `d` for the segment `n` (KVM_SEG_* or TSS_LDT) with the
 * selector `sel` at privilege `cpl`, as Linux's __load_segment_descriptor
 * for X86_TRANSFER_TASK_SWITCH; 0 when it may load.
 */
static int seg_refused(int n, uint16_t sel, const struct desc *d, uint32_t cpl)
{
    uint32_t type = DESC_TYPE(d), dpl = DESC_DPL(d), rpl = sel & 3u;
    int bad;

    switch (n) {
    case KVM_SEG_SS:
        bad = !DESC_S(d) || rpl != cpl || (type & 0xAu) != 2u || dpl != cpl;
        break;
    case KVM_SEG_CS:
        bad = !DESC_S(d) || !(type & 8u) || ((type & 4u) ? dpl > rpl : dpl != rpl);
        break;
    case TSS_LDT:
        bad = DESC_S(d) || type != 2u;
        break;
    default:
        bad = !DESC_S(d) || (type & 0xAu) == 8u ||
              ((type & 0xCu) != 0xCu && (rpl > dpl || cpl > dpl));
        break;
    }
    if (bad) {
        return TS_VECTOR;
    }
    if (!DESC_P(d)) {
        return n == KVM_SEG_SS ? SS_VECTOR : NP_VECTOR;
    }
    return 0;
}

/**
 * Load `v`'s segment `n` (KVM_SEG_* or TSS_LDT) from its selector, which
 * the new task's TSS gave, at privilege `cpl`, in virtual-8086 mode with
 * `vm86`, as Linux's __load_segment_descriptor for a task switch: a null
 * selector, #GP(0) for CS and SS, loads an unusable segment; a descriptor
 * is checked and marked accessed, and CS takes `cpl` as its RPL. 0; -1
 * with the fault queued; 1 or -EFAULT as `system_rw`.
 */
static int load_seg(struct kvm_vcpu *v, int n, uint32_t cpl, int vm86)
{
    struct kvm_segment *s = task_seg(v, n);
    uint16_t sel = s->selector;
    struct desc d;
    int r, nr;

    if (vm86 && n != TSS_LDT) {
        k_memset(s, 0, sizeof(*s));
        s->selector = sel;
        s->base = (uint64_t)sel << 4;
        s->limit = 0xFFFFu;
        s->type = 3;
        s->present = 1;
        s->s = 1;
        s->dpl = 3;
        return 0;
    }
    if (!(sel & 0xFFFCu)) {
        if (n == KVM_SEG_CS || n == KVM_SEG_SS) {
            return ts_fault(v, GP_VECTOR, 0);
        }
        k_memset(s, 0, sizeof(*s));
        s->selector = sel;
        s->unusable = 1;
        return 0;
    }
    r = desc_read(v, sel, &d);
    if (r != 0) {
        return r;
    }
    nr = seg_refused(n, sel, &d, cpl);
    if (nr != 0) {
        return ts_fault(v, (uint8_t)nr, sel & 0xFFFCu);
    }
    if (DESC_S(&d) && !(d.b[5] & 1u)) {
        d.b[5] |= 1u;
        r = desc_write_type(v, &d);
        if (r != 0) {
            return r;
        }
    }
    desc_seg(s, n == KVM_SEG_CS ? (uint16_t)((sel & 0xFFFCu) | cpl) : sel, &d);
    return 0;
}

/** The DPL check of the task gate at IDT vector `vec` that an INT n goes
 * through, which Linux makes for software interrupts: 0, or -1 with
 * #GP(`vec`, IDT) queued when the gate's DPL is below the CPL or below the
 * RPL of the TSS selector `sel`. */
static int gate_check(struct kvm_vcpu *v, uint16_t sel, uint32_t vec)
{
    uint32_t error = (vec << 3) | 2u, dpl;
    uint8_t g[8];
    int r;

    if (v->sregs.idt.limit < vec * 8u + 7u) {
        return ts_fault(v, GP_VECTOR, error);
    }
    r = system_rw(v, (v->sregs.idt.base + vec * 8u) & 0xFFFFFFFFull, g, 8, 0);
    if (r != 0) {
        return r;
    }
    dpl = ((uint32_t)g[5] >> 5) & 3u;
    if ((sel & 3u) > dpl || (uint32_t)kvm_emul_cpl(v) > dpl) {
        return ts_fault(v, GP_VECTOR, error);
    }
    return 0;
}

/**
 * The checks and reads a task switch makes before it changes anything,
 * whose faults the old task takes: the descriptors of the TSS `sel` and of
 * the current one, the task gate of an INT n at `idt_index` (-1 for
 * none), the new TSS present and big enough for its kind, as Linux's
 * emulator_do_task_switch, and both TSSes read. 0; -1 with the fault
 * queued; 1 or -EFAULT.
 */
static int ts_begin(struct kvm_vcpu *v, struct task_switch *ts, uint16_t sel, int reason,
                    int idt_index)
{
    struct kvm_segment n;
    int r;

    ts->old_sel = v->sregs.tr.selector;
    r = desc_read(v, sel, &ts->next);
    if (r == 0) {
        r = desc_read(v, ts->old_sel, &ts->cur);
    }
    if (r == 0 && reason == KVM_TS_GATE && idt_index >= 0) {
        r = gate_check(v, sel, (uint32_t)idt_index);
    }
    if (r != 0) {
        return r;
    }
    ts->l = (DESC_TYPE(&ts->next) & 8u) ? &tss32 : &tss16;
    ts->lo = (v->sregs.tr.type & 8u) ? &tss32 : &tss16;
    desc_seg(&n, sel, &ts->next);
    if (!n.present || n.limit < ts->l->size - 1u) {
        return ts_fault(v, TS_VECTOR, sel & 0xFFFCu);
    }
    ts->new_base = n.base;
    r = system_rw(v, v->sregs.tr.base, ts->old_tss, ts->lo->size, 0);
    if (r == 0) {
        r = system_rw(v, n.base, ts->new_tss, ts->l->size, 0);
    }
    return r;
}

/**
 * The writes of a task switch, in the order Linux's emulator_do_task_switch
 * makes them: the current TSS's busy bit cleared for IRET and JMP, its
 * EIP, EFLAGS (NT cleared for IRET), general registers and selectors saved
 * with `next_rip` the EIP, the new TSS's back link for CALL and a task
 * gate, and its busy bit set but for IRET. 0; -1 with #PF queued; 1 or
 * -EFAULT.
 */
static int ts_save(struct kvm_vcpu *v, struct task_switch *ts, int reason, uint64_t next_rip)
{
    const struct tss_layout *lo = ts->lo;
    uint64_t rflags = v->regs.rflags;
    uint8_t *t = ts->old_tss;
    int i, r = 0;

    if (reason == KVM_TS_IRET || reason == KVM_TS_JMP) {
        ts->cur.b[5] &= (uint8_t)~2u;
        r = desc_write_type(v, &ts->cur);
    }
    if (reason == KVM_TS_IRET) {
        rflags &= ~RFLAGS_NT;
    }
    put_le(t + lo->ip, lo->width, (uint32_t)next_rip);
    put_le(t + lo->flags, lo->width, (uint32_t)rflags);
    for (i = 0; i < 8; i++) {
        put_le(t + lo->regs + (uint32_t)i * lo->width, lo->width, (uint32_t)*gpr(v, i));
    }
    for (i = 0; i < (int)lo->nsels; i++) {
        put_le(t + lo->sels + (uint32_t)i * lo->width, lo->width, task_seg(v, i)->selector);
    }
    if (!lo->cr3) {
        put_le(t + lo->ldt, 2, v->sregs.ldt.selector);
    }
    if (r == 0) {
        r = system_rw(v, v->sregs.tr.base + lo->save_from, t + lo->save_from,
                      lo->save_to - lo->save_from, 1);
    }
    if (r == 0 && (reason == KVM_TS_CALL || reason == KVM_TS_GATE)) {
        put_le(ts->new_tss, 2, ts->old_sel);
        r = system_rw(v, ts->new_base, ts->new_tss, 2, 1);
    }
    if (r == 0 && reason != KVM_TS_IRET) {
        ts->next.b[5] |= 2u;
        r = desc_write_type(v, &ts->next);
    }
    return r;
}

/**
 * Make the new task current, as the processor does once the switch is
 * committed: TR the new TSS, CR0.TS set, DR7's local enables cleared, then
 * the TSS's CR3, EIP, EFLAGS (NT set for CALL and a task gate), general
 * registers and selectors. A CR3 whose PAE PDPTEs do not load is #GP(0)
 * in the new task, with the registers left as they were. 0 or -1.
 */
static int ts_enter(struct kvm_vcpu *v, const struct task_switch *ts, uint16_t sel, int reason)
{
    const struct tss_layout *l = ts->l;
    const uint8_t *t = ts->new_tss;
    uint64_t cr3, pdpte[4];
    int i;

    desc_seg(&v->sregs.tr, sel, &ts->next);
    v->sregs.cr0 |= CR0_TS;
    v->debugregs.dr7 &= ~DR7_LOCAL;
    v->events.interrupt.shadow = 0;
    v->dirty |= KVM_PART_SREGS | KVM_PART_DEBUG | KVM_PART_EVENTS;
    kvm_vcpu_kick(v, KVM_REQ_EVENT);
    if (l->cr3) {
        cr3 = get_le(t + l->cr3, 4);
        if ((v->sregs.cr0 & CR0_PG) && (v->sregs.cr4 & CR4_PAE) && !(v->sregs.efer & EFER_LMA) &&
            kvm_guest_pdptes(v, cr3, pdpte) != 0) {
            return ts_fault(v, GP_VECTOR, 0);
        }
        v->sregs.cr3 = cr3;
    }
    v->regs.rip = get_le(t + l->ip, l->width);
    v->regs.rflags = get_le(t + l->flags, l->width) | 2u;
    if (reason == KVM_TS_CALL || reason == KVM_TS_GATE) {
        v->regs.rflags |= RFLAGS_NT;
    }
    for (i = 0; i < 8; i++) {
        *gpr(v, i) = get_le(t + l->regs + (uint32_t)i * l->width, l->width);
    }
    for (i = 0; i < (int)l->nsels; i++) {
        task_seg(v, i)->selector = (uint16_t)get_le(t + l->sels + (uint32_t)i * l->width, 2);
    }
    v->sregs.ldt.selector = (uint16_t)get_le(t + l->ldt, 2);
    return 0;
}

/** Load the descriptors of the new task's selectors, in Linux's order: the
 * LDT's, then ES, CS, SS, DS and, from a 32-bit TSS, FS and GS. Their faults
 * are the new task's. 0; -1 with the fault queued; 1 or -EFAULT. */
static int ts_load(struct kvm_vcpu *v, const struct task_switch *ts)
{
    static const int order[7] = {TSS_LDT,    KVM_SEG_ES, KVM_SEG_CS, KVM_SEG_SS,
                                 KVM_SEG_DS, KVM_SEG_FS, KVM_SEG_GS};
    int vm86 = (v->regs.rflags & RFLAGS_VM) != 0, i, r;
    uint32_t cpl = vm86 ? 3u : (v->sregs.cs.selector & 3u);

    for (i = 0; i < (int)ts->l->nsels + 1; i++) {
        r = load_seg(v, order[i], cpl, vm86);
        if (r != 0) {
            return r;
        }
    }
    return 0;
}

/** Push `error`, the error code of the exception whose task gate made the
 * switch, on the new task's stack, a word or a doubleword by its TSS's
 * kind, with #SS(0) past SS's limit. 0; -1 with the fault queued; 1 or
 * -EFAULT. */
static int ts_push(struct kvm_vcpu *v, const struct task_switch *ts, uint32_t error)
{
    const struct kvm_segment *ss = &v->sregs.ss;
    uint32_t size = ts->l->width;
    uint64_t mask = ss->db ? 0xFFFFFFFFull : 0xFFFFull;
    uint64_t sp = (v->regs.rsp - size) & mask, end = sp + size - 1u;
    uint8_t b[4];
    int r;

    if ((ss->type & 4u) ? (sp <= ss->limit || end > mask) : end > ss->limit) {
        return ts_fault(v, SS_VECTOR, 0);
    }
    put_le(b, size, error);
    r = kvm_guest_rw(v, KVM_SEG_SS, sp, b, size, 1);
    if (r == 0) {
        v->regs.rsp = (v->regs.rsp & ~mask) | sp;
    }
    return r;
}

int kvm_task_switch(struct kvm_vcpu *v, uint16_t sel, int reason, int idt_index, uint64_t next_rip,
                    int64_t error)
{
    struct task_switch ts;
    int r;

    r = ts_begin(v, &ts, sel, reason, idt_index);
    if (r == 0) {
        r = ts_save(v, &ts, reason, next_rip);
    }
    if (r != 0) {
        return r;
    }
    r = ts_enter(v, &ts, sel, reason);
    if (r == 0) {
        r = ts_load(v, &ts);
    }
    if (r == 0 && error >= 0) {
        r = ts_push(v, &ts, (uint32_t)error);
    }
    return r == -1 ? 0 : r;
}

/* ---- the instruction emulator ---- */

/*
 * An instruction the processor could not finish, because an operand is
 * device memory or the guest's CPUID has what the host lacks, is carried
 * out here as Linux's emulator does it (arch/x86/kvm/emulate.c). One pass
 * reads the operands, runs the operation on the record's registers and
 * queues the writes to memory. A read of device memory ends the pass with
 * its MMIO exit; the answer is kept and the instruction runs again from
 * the start, taking the answers kept so far in order, until a pass reads
 * all it needs. A pass that faults or stops puts the registers back, so
 * nothing of the instruction shows until all of it can. Then the writes
 * are translated, those to RAM made, the registers kept, and each write to
 * device memory leaves as an MMIO exit of its own.
 */

/* The stages of an instruction under way, `v->emul.stage`. */
#define EMUL_IDLE     0
#define EMUL_READ     1 /* a pass waits for the answer of an MMIO read */
#define EMUL_WRITE    2 /* MMIO writes are left to deliver */
#define EMUL_PIO      3 /* a port read waits for its answer */
#define EMUL_PIO_READ 4 /* a string OUT waits for the answer of an MMIO read */
#define EMUL_RUN      5 /* the instruction runs again at the next step */
#define EMUL_PIO_RUN  6 /* the string OUT runs again at the next step */
#define EMUL_PIO_OUT  7 /* an OUT's bytes are out, and RIP is still at it */
#define EMUL_STEP     8 /* the caller's single step waits to be reported */
#define EMUL_FAIL     9 /* a string IN's failure, `fail`, waits to be reported */

/* How a pass or one of its steps ends; a negative errno ends KVM_RUN. */
#define X_OK    0
#define X_FAULT (-1) /* an exception is queued for the guest */
#define X_IO    1    /* `v->exit` is an MMIO read whose answer the pass needs */
#define X_FAIL  2    /* the instruction cannot be carried out */

/* How a far transfer loads CS, for the checks of its descriptor. */
#define XFER_NONE 0
#define XFER_JMP  1 /* a far jump or call */
#define XFER_RET  2 /* a far return */

#define CR0_EM     0x00000004ull
#define CR4_OSFXSR (1ull << 9)
#define CR4_UMIP   (1ull << 11)

#define EFER_SCE 0x001ull

#define RFLAGS_IF   0x0200ull
#define RFLAGS_IOPL 0x3000ull
#define RFLAGS_AC   0x40000ull
#define RFLAGS_ID   0x200000ull

#define DE_VECTOR 0
#define NM_VECTOR 7
#define MF_VECTOR 16

#define MSR_IA32_SYSENTER_CS  0x174u
#define MSR_IA32_SYSENTER_ESP 0x175u
#define MSR_IA32_SYSENTER_EIP 0x176u
#define MSR_STAR              0xC0000081u
#define MSR_LSTAR             0xC0000082u
#define MSR_CSTAR             0xC0000083u
#define MSR_SYSCALL_MASK      0xC0000084u
#define MSR_TSC_AUX           0xC0000103u

/* Where the XSAVE image keeps what the emulator reads: the control and
 * status words, the abridged tag word, MXCSR, the x87 and MMX registers,
 * the SSE registers, and the header's map of the components in use. */
#define FX_FCW  0
#define FX_FSW  2
#define FX_FTW  4
#define FX_ST   32
#define FX_XMM  160
#define FX_SIZE 512
#define XS_BV   512

/* One pass over an instruction. */
struct xc {
    struct kvm_vcpu *v;
    const struct kvm_insn *in;
    int mode;                /* 16, 32 or 64, as the instruction began */
    uint32_t cpl;            /* the privilege level, as it began */
    uint64_t next;           /* RIP after the instruction: the next one, or a branch's target */
    uint64_t d[2], s[2], s2; /* the operands' values */
    uint8_t raw[16];         /* a memory source's bytes */
    uint64_t dst_ea, src_ea; /* the memory operands' offsets in their segments */
    int write;               /* dst is written back */
    int again;               /* a REP iteration ended with more to come */
    int shadow;              /* the interrupt shadow the instruction leaves */
    int tf_after;            /* the single-step trap goes by TF as the instruction left it */
    int sregs;               /* `sregs` changed */
    uint32_t rpos;           /* the MMIO answers the pass has taken */
};

/** Queue the exception `nr` for the guest and end the pass. */
static int x_fault(struct xc *x, uint8_t nr, int has_error, uint32_t error)
{
    kvm_queue_exception(x->v, nr, has_error, error);
    return X_FAULT;
}

/** Sign-extend the low `bytes` bytes of `value`. */
static uint64_t sx(uint64_t value, uint32_t bytes)
{
    uint64_t m = size_mask(bytes), sign;

    if (bytes >= 8u) {
        return value;
    }
    sign = 1ull << (bytes * 8u - 1u);
    return (value & sign) ? (value | ~m) : (value & m);
}

/** Whether `la` is a canonical address of 48 bits. */
static int canonical(uint64_t la)
{
    return sx(la, 6) == la;
}

/** The mask of the stack pointer's width: SS's B bit outside 64-bit
 * mode. */
static uint64_t stack_mask(const struct kvm_vcpu *v)
{
    if (kvm_emul_mode(v) == 64) {
        return ~0ull;
    }
    return v->sregs.ss.db ? 0xFFFFFFFFull : 0xFFFFull;
}

/** Write `value` to the general register `n` in `size` bytes as the
 * processor does: 4 bytes clear the upper half, 1 or 2 keep the rest. */
static void reg_put(struct kvm_vcpu *v, int n, uint32_t size, uint64_t value)
{
    uint64_t *r = gpr(v, n);

    if (size >= 8u) {
        *r = value;
    } else if (size == 4u) {
        *r = value & 0xFFFFFFFFull;
    } else {
        *r = (*r & ~size_mask(size)) | (value & size_mask(size));
    }
}

/** Add `delta` to the register `n` within `bytes` bytes, the address
 * size of a string or loop instruction. */
static void reg_add(struct kvm_vcpu *v, int n, uint64_t delta, uint32_t bytes)
{
    reg_put(v, n, bytes, *gpr(v, n) + delta);
}

/** Add `delta` to the stack pointer within its width. */
static void sp_add(struct kvm_vcpu *v, uint64_t delta)
{
    uint64_t m = stack_mask(v);

    v->regs.rsp = (v->regs.rsp & ~m) | ((v->regs.rsp + delta) & m);
}

/** The parity flag of the low byte of `x`: set when its bits are even. */
static int parity(uint64_t x)
{
    uint32_t b = (uint32_t)(x & 0xFFu);

    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return !(b & 1u);
}

/** Set ZF, SF and PF in `*rflags` from the result `res` of `size` bytes. */
static void flags_szp(uint64_t *rflags, uint64_t res, uint32_t size)
{
    uint64_t f = *rflags & ~(RFLAGS_ZF | RFLAGS_SF | RFLAGS_PF);

    res &= size_mask(size);
    if (res == 0) {
        f |= RFLAGS_ZF;
    }
    if (res >> (size * 8u - 1u)) {
        f |= RFLAGS_SF;
    }
    if (parity(res)) {
        f |= RFLAGS_PF;
    }
    *rflags = f;
}

/** Set CF and OF in `*rflags` to `cf` and `of`. */
static void flags_co(uint64_t *rflags, int cf, int of)
{
    *rflags = (*rflags & ~(RFLAGS_CF | RFLAGS_OF)) | (cf ? RFLAGS_CF : 0u) | (of ? RFLAGS_OF : 0u);
}

/**
 * Run the operation `alu` on `a` and `b`, `size` bytes, as the processor
 * does: the result, with the arithmetic flags put in `*rflags`, CF from it
 * in for ADC and SBB. NOT changes no flag; INC and DEC keep CF.
 */
static uint64_t alu_run(uint32_t alu, uint64_t a, uint64_t b, uint32_t size, uint64_t *rflags)
{
    uint64_t m = size_mask(size), sign = 1ull << (size * 8u - 1u), res = 0, f;
    uint64_t cin = (*rflags & RFLAGS_CF) ? 1u : 0u;
    int cf = 0, of = 0, logic = 0;

    a &= m;
    b &= m;
    switch (alu) {
    case KVM_ALU_ADD:
    case KVM_ALU_ADC:
    case KVM_ALU_INC:
        if (alu == KVM_ALU_INC) {
            b = 1;
            cin = 0;
        } else if (alu == KVM_ALU_ADD) {
            cin = 0;
        }
        res = (a + b + cin) & m;
        cf = res < a || (cin && res == a);
        of = ((a ^ res) & (b ^ res) & sign) != 0;
        break;
    case KVM_ALU_SUB:
    case KVM_ALU_SBB:
    case KVM_ALU_CMP:
    case KVM_ALU_DEC:
    case KVM_ALU_NEG:
        if (alu == KVM_ALU_DEC) {
            b = 1;
            cin = 0;
        } else if (alu == KVM_ALU_NEG) {
            b = a;
            a = 0;
            cin = 0;
        } else if (alu != KVM_ALU_SBB) {
            cin = 0;
        }
        res = (a - b - cin) & m;
        cf = a < b || (cin && a == b);
        of = ((a ^ b) & (a ^ res) & sign) != 0;
        break;
    case KVM_ALU_OR:
        res = a | b;
        logic = 1;
        break;
    case KVM_ALU_AND:
    case KVM_ALU_TEST:
        res = a & b;
        logic = 1;
        break;
    case KVM_ALU_XOR:
        res = a ^ b;
        logic = 1;
        break;
    case KVM_ALU_NOT:
        return ~a & m;
    default:
        return a;
    }
    f = *rflags & ~RFLAGS_ARITH;
    if (alu == KVM_ALU_INC || alu == KVM_ALU_DEC) {
        f |= *rflags & RFLAGS_CF;
    } else if (cf && !logic) {
        f |= RFLAGS_CF;
    }
    if (alu == KVM_ALU_NEG) {
        f = (f & ~RFLAGS_CF) | (b != 0 ? RFLAGS_CF : 0u);
    }
    if (of && !logic) {
        f |= RFLAGS_OF;
    }
    if (!logic && ((a ^ b ^ res) & 0x10u)) {
        f |= RFLAGS_AF;
    }
    *rflags = f;
    flags_szp(rflags, res, size);
    return res;
}

/** The value `bytes` holds, `size` bytes little-endian, 8 at most. */
static uint64_t bytes_value(const uint8_t *bytes, uint32_t size)
{
    uint64_t x = 0;
    uint32_t i;

    for (i = 0; i < size && i < 8u; i++) {
        x |= (uint64_t)bytes[i] << (i * 8u);
    }
    return x;
}

/** Store `x` in `bytes`, `size` bytes little-endian, 8 at most. */
static void value_bytes(uint8_t *bytes, uint64_t x, uint32_t size)
{
    uint32_t i;

    for (i = 0; i < size && i < 8u; i++) {
        bytes[i] = (uint8_t)(x >> (i * 8u));
    }
}

/** Make `v->exit` the MMIO access of `size` bytes at `gpa`: a write of
 * `data`, or with `data` null a read, whose answer comes back through
 * `kvm_emul_complete`. */
static void mmio_exit(struct kvm_vcpu *v, uint64_t gpa, uint32_t size, const uint8_t *data)
{
    struct kvm_exit *ex = &v->exit;

    ex->kind = KVM_X_MMIO;
    k_memset(&ex->mmio, 0, sizeof(ex->mmio));
    ex->mmio.phys_addr = gpa;
    ex->mmio.len = size;
    ex->mmio.is_write = (uint8_t)(data != NULL);
    if (data) {
        k_memcpy(ex->mmio.data, data, size);
    }
}

/* ---- the emulator's reads and writes of memory ---- */

/**
 * Take the answer of the MMIO read of `n` bytes at `gpa`, 8 at most, into
 * `buf`: the next of those kept, counted by `*rpos`, or, with none kept,
 * X_IO with `v->exit` the read. An answer kept for another address, which
 * the guest's tables changing under the instruction leave, is dropped with
 * those after it. X_FAIL for more reads than an instruction makes.
 */
static int mmio_piece(struct kvm_vcpu *v, uint32_t *rpos, uint64_t gpa, uint8_t *buf, uint32_t n)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_emul_read *c;

    if (*rpos < em->rcount) {
        c = &em->rd[*rpos];
        if (c->gpa == gpa && c->len == n) {
            k_memcpy(buf, c->data, n);
            (*rpos)++;
            return X_OK;
        }
        em->rcount = *rpos;
    }
    if (em->rcount >= KVM_EMUL_READS) {
        return X_FAIL;
    }
    c = &em->rd[em->rcount];
    c->gpa = gpa;
    c->len = n;
    mmio_exit(v, gpa, n, NULL);
    return X_IO;
}

/**
 * Read `n` bytes at the linear address `la` of `v` into `buf`, at the
 * guest's privilege: RAM at once, device memory in pieces of 8 bytes at
 * most through `mmio_piece`, which counts by `*rpos`. X_OK, X_FAULT with
 * #PF queued, X_IO, X_FAIL for a page table in no slot, or -EFAULT.
 */
static int la_read(struct kvm_vcpu *v, uint64_t la, void *buf, uint32_t n, uint32_t *rpos)
{
    uint8_t *p = (uint8_t *)buf;
    uint64_t gpa;
    uint32_t chunk, piece, done;
    int r;

    while (n) {
        chunk = 4096u - (uint32_t)(la & 0xFFFu);
        if (chunk > n) {
            chunk = n;
        }
        r = kvm_guest_translate(v, la, access_bits(v, 0, 0), &gpa);
        if (r != 0) {
            return r == 1 ? X_FAIL : r;
        }
        r = gpa_rw(v, gpa, p, chunk, 0);
        if (r < 0) {
            return r;
        }
        for (done = 0; r == 1 && done < chunk; done += piece) {
            piece = chunk - done < 8u ? chunk - done : 8u;
            r = mmio_piece(v, rpos, gpa + done, p + done, piece);
            if (r != X_OK) {
                return r;
            }
            r = 1;
        }
        p += chunk;
        la += chunk;
        n -= chunk;
    }
    return X_OK;
}

/** Forget the writes queued and lined up. */
static void wq_reset(struct kvm_vcpu *v)
{
    v->emul.nw = 0;
    v->emul.wlen = 0;
    v->emul.nchunk = 0;
    v->emul.wcur = 0;
}

/**
 * Queue the write of the `n` bytes at `buf` to the linear address `la`,
 * whose part in device memory leaves in pieces of `unit` bytes: X_OK, or
 * X_FAIL for more than an instruction writes.
 */
static int wq_add(struct kvm_vcpu *v, uint64_t la, const void *buf, uint32_t n, uint32_t unit)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_emul_write *w;

    if (em->nw >= KVM_EMUL_WRITES || n > sizeof(em->wbuf) - em->wlen) {
        return X_FAIL;
    }
    w = &em->w[em->nw++];
    w->la = la;
    w->off = em->wlen;
    w->len = n;
    w->unit = unit;
    k_memcpy(em->wbuf + em->wlen, buf, n);
    em->wlen += n;
    return X_OK;
}

/** Whether a write to `gpa` of `v` goes to RAM: a slot that is not
 * read-only. */
static int gpa_ram(struct kvm_vcpu *v, uint64_t gpa)
{
    struct kvm_slot_view s;

    return kvm_slot_find(v->vm, gpa >> 12, &s) && !(s.flags & KVM_MEM_READONLY);
}

/**
 * Translate the queued write `w` for writing and line up its pages: X_OK,
 * X_FAULT with #PF queued, X_FAIL, or -EFAULT.
 */
static int wq_translate(struct kvm_vcpu *v, const struct kvm_emul_write *w)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_emul_chunk *c;
    uint64_t la = w->la, gpa;
    uint32_t n = w->len, off = w->off, chunk;
    int r;

    while (n) {
        chunk = 4096u - (uint32_t)(la & 0xFFFu);
        if (chunk > n) {
            chunk = n;
        }
        r = kvm_guest_translate(v, la, access_bits(v, 1, 0), &gpa);
        if (r != 0) {
            return r == 1 ? X_FAIL : r;
        }
        if (em->nchunk >= KVM_EMUL_CHUNKS) {
            return X_FAIL;
        }
        c = &em->chunk[em->nchunk++];
        c->gpa = gpa;
        c->off = off;
        c->len = chunk;
        c->unit = w->unit;
        c->done = 0;
        c->mmio = !gpa_ram(v, gpa);
        la += chunk;
        off += chunk;
        n -= chunk;
    }
    return X_OK;
}

/**
 * Carry out the queued writes: every page translated first, so that a
 * fault leaves none made, then those to RAM written; those to device
 * memory stay lined up for `wq_next`. X_OK, X_FAULT, X_FAIL or -EFAULT.
 */
static int wq_commit(struct kvm_vcpu *v)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_emul_chunk *c;
    uint32_t i;
    int r;

    em->nchunk = 0;
    em->wcur = 0;
    for (i = 0; i < em->nw; i++) {
        r = wq_translate(v, &em->w[i]);
        if (r != X_OK) {
            return r;
        }
    }
    for (i = 0; i < em->nchunk; i++) {
        c = &em->chunk[i];
        if (c->mmio) {
            continue;
        }
        r = gpa_rw(v, c->gpa, em->wbuf + c->off, c->len, 1);
        if (r < 0) {
            return r;
        }
        c->mmio = r == 1;
    }
    return X_OK;
}

/** Whether a write to device memory is left to deliver. */
static int wq_pending(const struct kvm_vcpu *v)
{
    const struct kvm_emul *em = &v->emul;
    uint32_t i;

    for (i = em->wcur; i < em->nchunk; i++) {
        if (em->chunk[i].mmio && em->chunk[i].done < em->chunk[i].len) {
            return 1;
        }
    }
    return 0;
}

/** Make `v->exit` the next piece of the writes to device memory: whether
 * there was one. */
static int wq_next(struct kvm_vcpu *v)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_emul_chunk *c;
    uint32_t piece;

    for (; em->wcur < em->nchunk; em->wcur++) {
        c = &em->chunk[em->wcur];
        if (!c->mmio || c->done >= c->len) {
            continue;
        }
        piece = c->len - c->done < c->unit ? c->len - c->done : c->unit;
        mmio_exit(v, c->gpa + c->done, piece, em->wbuf + c->off + c->done);
        c->done += piece;
        return 1;
    }
    return 0;
}

/** The segment register `seg` of `v`: KVM_SEG_ES to KVM_SEG_TR. */
static struct kvm_segment *x_seg(struct kvm_vcpu *v, uint32_t seg)
{
    if (seg == KVM_SEG_TR) {
        return &v->sregs.tr;
    }
    return task_seg(v, seg == KVM_SEG_LDTR ? TSS_LDT : (int)seg);
}

/**
 * The linear address of the `n` bytes at `seg`:`ea` for a read or, with
 * `write`, a write, with the checks Linux's __linearize makes: in 64-bit
 * mode a canonical address; otherwise the segment's limit and, in
 * protected mode, that it is usable, readable and for a write writable.
 * X_OK with `*la`, or X_FAULT with #SS(0) for the stack segment and #GP(0)
 * for another queued.
 */
static int x_linear(struct xc *x, uint32_t seg, uint64_t ea, uint32_t n, int write, uint64_t *la)
{
    const struct kvm_vcpu *v = x->v;
    const struct kvm_segment *s = x_seg(x->v, seg);
    uint64_t lim = s->limit;
    int bad = 0, prot = (v->sregs.cr0 & CR0_PE) && !(v->regs.rflags & RFLAGS_VM);

    if (x->mode == 64) {
        *la = seg_base(v, seg) + ea;
        bad = !canonical(*la) || !canonical(*la + n - 1u);
    } else {
        *la = (s->base + ea) & 0xFFFFFFFFull;
        if (prot) {
            bad = s->unusable || (write && ((s->type & 8u) || !(s->type & 2u))) ||
                  ((s->type & 8u) && !(s->type & 2u));
            if (!(s->type & 8u) && (s->type & 4u)) {
                /* An expand-down segment: the offsets above its limit. */
                bad = bad || ea <= lim;
                lim = s->db ? 0xFFFFFFFFull : 0xFFFFull;
            }
        }
        bad = bad || ea > lim || (lim != 0xFFFFFFFFull && n > lim + 1u - ea);
    }
    if (bad) {
        return x_fault(x, seg == KVM_SEG_SS ? SS_VECTOR : GP_VECTOR, 1, 0);
    }
    return X_OK;
}

/** Read `n` bytes at `seg`:`ea` into `buf` for the instruction: X_OK, or
 * as `x_linear` and `la_read` end. */
static int x_read(struct xc *x, uint32_t seg, uint64_t ea, void *buf, uint32_t n)
{
    uint64_t la;
    int r = x_linear(x, seg, ea, n, 0, &la);

    return r != X_OK ? r : la_read(x->v, la, buf, n, &x->rpos);
}

/** Queue the instruction's write of the `n` bytes at `buf` to `seg`:`ea`:
 * X_OK, or as `x_linear` and `wq_add` end. */
static int x_write(struct xc *x, uint32_t seg, uint64_t ea, const void *buf, uint32_t n)
{
    uint64_t la;
    int r = x_linear(x, seg, ea, n, 1, &la);

    return r != X_OK ? r : wq_add(x->v, la, buf, n, n < 8u ? n : 8u);
}

/** Push `value` in `size` bytes on the guest's stack. */
static int x_push(struct xc *x, uint64_t value, uint32_t size)
{
    struct kvm_vcpu *v = x->v;
    uint8_t b[8];
    int r;

    value_bytes(b, value, size);
    r = x_write(x, KVM_SEG_SS, (v->regs.rsp - size) & stack_mask(v), b, size);
    if (r == X_OK) {
        sp_add(v, 0ull - size);
    }
    return r;
}

/** Pop `size` bytes off the guest's stack into `*value`. */
static int x_pop(struct xc *x, uint64_t *value, uint32_t size)
{
    struct kvm_vcpu *v = x->v;
    uint8_t b[8];
    int r = x_read(x, KVM_SEG_SS, v->regs.rsp & stack_mask(v), b, size);

    if (r == X_OK) {
        *value = bytes_value(b, size);
        sp_add(v, size);
    }
    return r;
}

/**
 * Make `target`, cut to `size` bytes, the address the instruction leaves
 * RIP at, as Linux's assign_eip: #GP(0) for one that is not canonical in
 * 64-bit mode or lies past CS's limit outside it.
 */
static int x_jump(struct xc *x, uint64_t target, uint32_t size)
{
    const struct kvm_vcpu *v = x->v;

    target &= size_mask(size);
    if (kvm_emul_mode(v) == 64 ? !canonical(target) : target > v->sregs.cs.limit) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    x->next = target;
    return X_OK;
}

/* ---- the x87, MMX and SSE state ---- */

/** The 16-bit word at `off` of `v`'s XSAVE image. */
static uint32_t fx_word(const struct kvm_vcpu *v, uint32_t off)
{
    return (uint32_t)v->xsave[off] | ((uint32_t)v->xsave[off + 1u] << 8);
}

/**
 * Make the component `bit` of `v`'s XSAVE image, 1 for x87 or 2 for SSE,
 * one the image holds: a component its header leaves out is in its initial
 * state, which is written out and the header's bit set. On a host without
 * XSAVE the image is FXSAVE's, which has no header and holds both.
 */
static void fx_ready(struct kvm_vcpu *v, uint64_t bit)
{
    uint8_t *img = v->xsave;
    uint64_t bv = bytes_value(img + XS_BV, 8);

    if (kvm_xcr0_supported() == 0 || (bv & bit)) {
        return;
    }
    if (bit == 1u) {
        k_memset(img, 0, 24);
        img[FX_FCW] = 0x7F;
        img[FX_FCW + 1] = 0x03;
        k_memset(img + FX_ST, 0, 128);
    } else {
        k_memset(img + FX_XMM, 0, 256);
    }
    value_bytes(img + XS_BV, bv | bit, 8);
}

/**
 * Put `v`'s x87 state in MMX's form, as an MMX instruction leaves it: the
 * stack's top at register 0, with the registers rotated so that each keeps
 * its number, and every tag valid.
 */
static void mmx_enter(struct kvm_vcpu *v)
{
    uint8_t *img = v->xsave, old[128];
    uint32_t top, i;

    fx_ready(v, 1);
    top = (fx_word(v, FX_FSW) >> 11) & 7u;
    if (top != 0) {
        k_memcpy(old, img + FX_ST, 128);
        for (i = 0; i < 8u; i++) {
            k_memcpy(img + FX_ST + 16u * i, old + 16u * ((i - top) & 7u), 16);
        }
        img[FX_FSW + 1] &= (uint8_t)~0x38u;
    }
    img[FX_FTW] = 0xFF;
}

/**
 * The checks before an instruction with an SSE or MMX operand, as Linux
 * makes them: #UD with CR0.EM, or for SSE without CR4.OSFXSR; #NM with
 * CR0.TS; and for MMX #MF when an x87 exception is pending, else the state
 * put in MMX's form.
 */
static int x_simd(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    const struct kvm_insn *in = x->in;
    int sse = in->dst.kind == KVM_OPND_XMM || in->src.kind == KVM_OPND_XMM;
    int mmx = in->dst.kind == KVM_OPND_MM || in->src.kind == KVM_OPND_MM;

    if (!sse && !mmx) {
        return X_OK;
    }
    if ((v->sregs.cr0 & CR0_EM) || (sse && !(v->sregs.cr4 & CR4_OSFXSR))) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    if (v->sregs.cr0 & CR0_TS) {
        return x_fault(x, NM_VECTOR, 0, 0);
    }
    if (mmx) {
        fx_ready(v, 1);
        if (fx_word(v, FX_FSW) & 0x80u) {
            return x_fault(x, MF_VECTOR, 0, 0);
        }
        mmx_enter(v);
    } else {
        fx_ready(v, 2);
    }
    return X_OK;
}

/* ---- the operands ---- */

/** The offset of the memory operand `o` in its segment, within the
 * instruction's address size. */
static uint64_t mem_ea(const struct xc *x, const struct kvm_operand *o)
{
    struct kvm_vcpu *v = x->v;
    const struct kvm_insn *in = x->in;
    uint64_t ea = in->disp;

    switch (o->mem) {
    case KVM_MEM_SI:
        ea = v->regs.rsi;
        break;
    case KVM_MEM_DI:
        ea = v->regs.rdi;
        break;
    case KVM_MEM_XLAT:
        ea = v->regs.rbx + (v->regs.rax & 0xFFu);
        break;
    default:
        if (in->base == KVM_REG_RIP) {
            ea += x->next;
        } else if (in->base != KVM_REG_NONE) {
            ea += *gpr(v, in->base);
        }
        if (in->index != KVM_REG_NONE) {
            ea += *gpr(v, in->index) * in->scale;
        }
        break;
    }
    return ea & size_mask(in->addr_size);
}

/** The segment of the memory operand `o`: ES for a string's destination. */
static uint32_t mem_seg(const struct xc *x, const struct kvm_operand *o)
{
    return o->mem == KVM_MEM_DI ? (uint32_t)KVM_SEG_ES : x->in->seg;
}

/**
 * Check a memory operand of 16 bytes that must be aligned, at `ea`:
 * #GP(0) when its linear address is not a multiple of 16.
 */
static int mem_aligned(struct xc *x, const struct kvm_operand *o, uint64_t ea)
{
    if ((x->in->flags & KVM_INSN_ALIGNED) && ((seg_base(x->v, mem_seg(x, o)) + ea) & 15u)) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    return X_OK;
}

/**
 * Read the operand `o`, whose memory is at `ea`, into the two words at
 * `val`: a register, an immediate, a selector, an SSE or MMX register, or
 * memory, 16 bytes at most, whose bytes `raw` also receives when not null.
 */
static int opnd_read(struct xc *x, const struct kvm_operand *o, uint64_t ea, uint64_t *val,
                     uint8_t *raw)
{
    struct kvm_vcpu *v = x->v;
    uint8_t b[16];
    int r = X_OK;

    k_memset(b, 0, sizeof(b));
    val[0] = 0;
    val[1] = 0;
    switch (o->kind) {
    case KVM_OPND_REG:
        val[0] = o->high8 ? (*gpr(v, o->reg) >> 8) & 0xFFu : *gpr(v, o->reg) & size_mask(o->size);
        return X_OK;
    case KVM_OPND_IMM:
        val[0] = o->imm;
        return X_OK;
    case KVM_OPND_SREG:
        val[0] = x_seg(v, o->reg)->selector;
        return X_OK;
    case KVM_OPND_XMM:
        k_memcpy(b, v->xsave + FX_XMM + 16u * o->reg, 16);
        break;
    case KVM_OPND_MM:
        k_memcpy(b, v->xsave + FX_ST + 16u * o->reg, 8);
        break;
    case KVM_OPND_MEM:
        r = mem_aligned(x, o, ea);
        if (r == X_OK) {
            r = x_read(x, mem_seg(x, o), ea, b, o->size);
        }
        if (raw) {
            k_memcpy(raw, b, 16);
        }
        break;
    default:
        return X_OK;
    }
    val[0] = bytes_value(b, 8);
    val[1] = bytes_value(b + 8, 8);
    return r;
}

/** Write the two words at `val` to the operand `o`, whose memory is at
 * `ea`: a register, an SSE or MMX register, or memory, queued. */
static int opnd_write(struct xc *x, const struct kvm_operand *o, uint64_t ea, const uint64_t *val)
{
    struct kvm_vcpu *v = x->v;
    uint8_t b[16];
    uint64_t *r;
    int rc;

    value_bytes(b, val[0], 8);
    value_bytes(b + 8, val[1], 8);
    switch (o->kind) {
    case KVM_OPND_REG:
        if (o->high8) {
            r = gpr(v, o->reg);
            *r = (*r & ~0xFF00ull) | ((val[0] & 0xFFu) << 8);
        } else {
            reg_put(v, o->reg, o->size, val[0]);
        }
        break;
    case KVM_OPND_XMM:
        k_memcpy(v->xsave + FX_XMM + 16u * o->reg, b, 16);
        break;
    case KVM_OPND_MM:
        k_memcpy(v->xsave + FX_ST + 16u * o->reg, b, 8);
        v->xsave[FX_ST + 16u * o->reg + 8u] = 0xFF;
        v->xsave[FX_ST + 16u * o->reg + 9u] = 0xFF;
        break;
    case KVM_OPND_MEM:
        rc = mem_aligned(x, o, ea);
        return rc != X_OK ? rc : x_write(x, mem_seg(x, o), ea, b, o->size);
    default:
        break;
    }
    return X_OK;
}

/**
 * Read the instruction's operands: the sources, then the destination
 * unless the instruction only writes it. Memory the instruction names
 * without accessing is not read. A bit instruction with a register's bit
 * number reaches past its memory operand, by the number's whole words, as
 * Linux's fetch_bit_operand; POP to memory through rSP addresses it by
 * the stack pointer as the pop leaves it.
 */
static int x_operands(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    int named = (in->flags & KVM_INSN_NOACCESS) != 0, r = X_OK;
    uint64_t two[2], sv;

    if (in->src.kind == KVM_OPND_MEM) {
        x->src_ea = mem_ea(x, &in->src);
    }
    if (in->dst.kind == KVM_OPND_MEM) {
        x->dst_ea = mem_ea(x, &in->dst);
        if (in->op == KVM_OP_POP && in->base == 4) {
            x->dst_ea = (x->dst_ea + in->size) & size_mask(in->addr_size);
        }
    }
    if (!(named && in->src.kind == KVM_OPND_MEM)) {
        r = opnd_read(x, &in->src, x->src_ea, x->s, x->raw);
    }
    if (r == X_OK) {
        r = opnd_read(x, &in->src2, 0, two, NULL);
        x->s2 = two[0];
    }
    if (in->op == KVM_OP_BT && in->dst.kind == KVM_OPND_MEM && in->src.kind == KVM_OPND_REG) {
        sv = sx(x->s[0], in->size) & ~(uint64_t)(in->size * 8u - 1u);
        sv = (sv >> 63) ? ~(~sv >> 3) : (sv >> 3);
        x->dst_ea = (x->dst_ea + sv) & size_mask(in->addr_size);
    }
    if (r == X_OK && !(in->flags & KVM_INSN_MOV) && !(named && in->dst.kind == KVM_OPND_MEM)) {
        r = opnd_read(x, &in->dst, x->dst_ea, x->d, NULL);
    }
    return r;
}

/* ---- loading a segment register ---- */

/**
 * Whether the code segment descriptor `d` refuses CS with the selector
 * `sel` at privilege `cpl` for `transfer`: a far return to the same
 * privilege, or a jump or call, with a conforming segment's rule for each;
 * `lma` is EFER.LMA, under which a code segment has D clear when L is set.
 */
static int cs_refused(uint16_t sel, const struct desc *d, uint32_t cpl, int transfer, int lma)
{
    uint32_t type = DESC_TYPE(d), dpl = DESC_DPL(d), rpl = sel & 3u;
    int bad, conforming = (type & 4u) != 0;

    if (transfer == XFER_RET) {
        bad = rpl < cpl || (conforming ? dpl > rpl : dpl != rpl);
    } else {
        bad = conforming ? dpl > cpl : (rpl > cpl || dpl != cpl);
    }
    return bad || !(type & 8u) || ((d->b[6] & 0x60u) == 0x60u && lma);
}

/**
 * Whether the descriptor `d` may load the segment `seg` with the selector
 * `sel` at privilege `cpl`, as Linux's __load_segment_descriptor judges it
 * outside a task switch; `lma` is EFER.LMA. 0; the vector, #GP, #SS or
 * #NP, that refuses it; or -1 for a transfer Linux's emulator does not
 * carry out, a far jump or call through a gate or a return outward.
 */
static int seg_check(uint32_t seg, uint16_t sel, const struct desc *d, uint32_t cpl, int transfer,
                     int lma)
{
    uint32_t type = DESC_TYPE(d), dpl = DESC_DPL(d), rpl = sel & 3u;
    int bad;

    if (seg <= KVM_SEG_GS && !DESC_S(d)) {
        return transfer == XFER_JMP ? -1 : GP_VECTOR;
    }
    switch (seg) {
    case KVM_SEG_SS:
        bad = rpl != cpl || (type & 0xAu) != 2u || dpl != cpl;
        break;
    case KVM_SEG_CS:
        if (transfer == XFER_RET && rpl > cpl) {
            return -1;
        }
        bad = cs_refused(sel, d, cpl, transfer, lma);
        break;
    case KVM_SEG_TR:
        bad = DESC_S(d) || (type != 1u && type != 9u);
        break;
    case KVM_SEG_LDTR:
        bad = DESC_S(d) || type != 2u;
        break;
    default:
        bad = (type & 0xAu) == 8u || ((type & 0xCu) != 0xCu && (rpl > dpl || cpl > dpl));
        break;
    }
    if (bad) {
        return GP_VECTOR;
    }
    if (!DESC_P(d)) {
        return seg == KVM_SEG_SS ? SS_VECTOR : NP_VECTOR;
    }
    return 0;
}

/**
 * Load `seg` with the null selector `sel`: #GP(0) for CS, TR, and SS
 * outside 64-bit mode or at another privilege; SS becomes a flat data
 * segment at the CPL, any other unusable. SS's limit is Linux's: a zero
 * descriptor limit scaled by G to 0xFFF, since VM entry refuses a limit
 * whose low 12 bits are not all ones under G.
 */
static int seg_load_null(struct xc *x, uint32_t seg, uint16_t sel)
{
    struct kvm_segment *s = x_seg(x->v, seg);

    if (seg == KVM_SEG_CS || seg == KVM_SEG_TR ||
        (seg == KVM_SEG_SS && (x->mode != 64 || (sel & 3u) != x->cpl))) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    k_memset(s, 0, sizeof(*s));
    s->selector = sel;
    if (seg == KVM_SEG_SS) {
        s->type = 3;
        s->present = 1;
        s->s = 1;
        s->dpl = (uint8_t)x->cpl;
        s->db = 1;
        s->g = 1;
        s->limit = 0xFFFu;
    } else {
        s->unusable = 1;
    }
    return X_OK;
}

/**
 * Load `seg` with `sel` where no descriptor is read: in real mode the
 * base alone follows the selector, and in virtual-8086 mode a segment
 * register becomes a 64 KiB segment at privilege 3. Whether the mode is
 * one of the two.
 */
static int seg_load_real(struct kvm_vcpu *v, uint32_t seg, uint16_t sel)
{
    struct kvm_segment *s = x_seg(v, seg);

    if (!(v->sregs.cr0 & CR0_PE)) {
        s->selector = sel;
        s->base = (uint64_t)sel << 4;
        return 1;
    }
    if (seg > KVM_SEG_GS || !(v->regs.rflags & RFLAGS_VM)) {
        return 0;
    }
    k_memset(s, 0, sizeof(*s));
    s->selector = sel;
    s->base = (uint64_t)sel << 4;
    s->limit = 0xFFFFu;
    s->type = 3;
    s->present = 1;
    s->s = 1;
    s->dpl = 3;
    return 1;
}

/**
 * The upper half of the base of the system descriptor `d` of `sel`, which
 * in 64-bit mode is the four bytes after it, into `*base3`, 0 for any
 * other: X_OK; X_FAULT with #GP(`sel`) queued for a base that is not
 * canonical; X_FAIL or -EFAULT as `system_rw`.
 */
static int desc_base3(struct xc *x, const struct desc *d, uint16_t sel, uint64_t *base3)
{
    uint8_t hi[4];
    int r;

    *base3 = 0;
    if (DESC_S(d) || x->mode != 64) {
        return X_OK;
    }
    r = system_rw(x->v, d->la + 8u, hi, 4, 0);
    if (r != 0) {
        return r == 1 ? X_FAIL : r;
    }
    *base3 = get_le(hi, 4);
    if ((*base3 >> 15) != 0 && (*base3 >> 15) != 0x1FFFFu) {
        return x_fault(x, GP_VECTOR, 1, sel & 0xFFFCu);
    }
    return X_OK;
}

/**
 * Load the segment register `seg` (KVM_SEG_ES to KVM_SEG_TR) of the guest
 * with the selector `sel`, as Linux's __load_segment_descriptor: in real
 * and virtual-8086 mode the base alone follows the selector; in protected
 * mode the descriptor is read, checked for the segment and for `transfer`
 * (XFER_*), marked accessed, or busy for TR, in its table at once, and
 * loaded, with the upper half of a system descriptor's base in 64-bit
 * mode. X_OK, X_FAULT, X_FAIL or -EFAULT.
 */
static int seg_load(struct xc *x, uint32_t seg, uint16_t sel, int transfer)
{
    struct kvm_vcpu *v = x->v;
    struct kvm_segment *s = x_seg(v, seg);
    struct desc d;
    uint64_t base3 = 0;
    int r, nr;

    x->sregs = 1;
    if (seg_load_real(v, seg, sel)) {
        return X_OK;
    }
    if ((seg == KVM_SEG_TR && (sel & 4u)) ||
        (seg == KVM_SEG_SS && sel == 3u && x->mode == 64 && transfer == XFER_NONE)) {
        return x_fault(x, GP_VECTOR, 1, sel & 0xFFFCu);
    }
    if (!(sel & 0xFFFCu)) {
        return seg_load_null(x, seg, sel);
    }
    r = desc_read(v, sel, &d);
    if (r != 0) {
        return r == 1 ? X_FAIL : r;
    }
    nr = seg_check(seg, sel, &d, x->cpl, transfer, (v->sregs.efer & EFER_LMA) != 0);
    if (nr != 0) {
        return nr < 0 ? X_FAIL : x_fault(x, (uint8_t)nr, 1, sel & 0xFFFCu);
    }
    r = desc_base3(x, &d, sel, &base3);
    if (r != X_OK) {
        return r;
    }
    if (seg == KVM_SEG_TR || (DESC_S(&d) && !(d.b[5] & 1u))) {
        d.b[5] |= (uint8_t)(seg == KVM_SEG_TR ? 2u : 1u);
        r = desc_write_type(v, &d);
        if (r != 0) {
            return r == 1 ? X_FAIL : r;
        }
    }
    desc_seg(s, seg == KVM_SEG_CS ? (uint16_t)((sel & 0xFFFCu) | x->cpl) : sel, &d);
    s->base |= base3 << 32;
    return X_OK;
}

/* ---- the operations ---- */

/** Whether the condition `cc`, the low four bits of a Jcc's opcode, holds
 * in `f`. */
static int cond(uint64_t f, uint32_t cc)
{
    int sf = (f & RFLAGS_SF) != 0, of = (f & RFLAGS_OF) != 0, r;

    switch (cc >> 1) {
    case 0:
        r = of;
        break;
    case 1:
        r = (f & RFLAGS_CF) != 0;
        break;
    case 2:
        r = (f & RFLAGS_ZF) != 0;
        break;
    case 3:
        r = (f & (RFLAGS_CF | RFLAGS_ZF)) != 0;
        break;
    case 4:
        r = sf;
        break;
    case 5:
        r = (f & RFLAGS_PF) != 0;
        break;
    case 6:
        r = sf != of;
        break;
    default:
        r = (f & RFLAGS_ZF) || sf != of;
        break;
    }
    return (cc & 1u) ? !r : r;
}

/** #GP(0) above CPL 0, for a privileged instruction. */
static int x_priv(struct xc *x)
{
    return x->cpl > 0 ? x_fault(x, GP_VECTOR, 1, 0) : X_OK;
}

/** Whether the guest runs in protected mode outside virtual-8086 mode. */
static int x_prot(const struct xc *x)
{
    return (x->v->sregs.cr0 & CR0_PE) && !(x->v->regs.rflags & RFLAGS_VM);
}

/** MOV with its extensions, and the string, SSE and MMX moves. */
static int h_mov(struct xc *x)
{
    const struct kvm_insn *in = x->in;

    x->d[0] = x->s[0];
    x->d[1] = x->s[1];
    if (in->sub == KVM_MOV_ZX) {
        x->d[0] = x->s[0] & size_mask(in->src.size);
    } else if (in->sub == KVM_MOV_SX) {
        x->d[0] = sx(x->s[0], in->src.size);
    }
    return X_OK;
}

/** ADD to CMP, TEST, and the compares of CMPS and SCAS. */
static int h_alu(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    uint64_t *f = &x->v->regs.rflags;

    if (in->sub == KVM_ALU_CMPR) {
        (void)alu_run(KVM_ALU_CMP, x->s[0], x->d[0], in->size, f);
    } else {
        x->d[0] = alu_run(in->sub, x->d[0], x->s[0], in->size, f);
    }
    return X_OK;
}

/** INC, DEC, NOT and NEG. */
static int h_unary(struct xc *x)
{
    x->d[0] = alu_run(x->in->sub, x->d[0], 0, x->in->size, &x->v->regs.rflags);
    return X_OK;
}

/** XCHG: a register with itself, NOP and PAUSE, writes nothing. */
static int h_xchg(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    uint64_t old[2];

    /* Only 0x90, XCHG of the accumulator with itself, is a NOP, as in
     * Linux; 87 C0 clears the upper half in 64-bit mode. */
    if ((in->bytes[in->len - 1u] & 0xF8u) == 0x90u && in->dst.kind == KVM_OPND_REG &&
        in->dst.reg == in->src.reg) {
        x->write = 0;
        return X_OK;
    }
    old[0] = x->d[0];
    old[1] = 0;
    x->d[0] = x->s[0];
    return opnd_write(x, &in->src, x->src_ea, old);
}

/** ROL, ROR, RCL and RCR of `d` by `c`, not 0, in `bits` bits: the result,
 * with CF and OF in `*f`. */
static uint64_t rotate(uint32_t sub, uint64_t d, uint32_t c, uint32_t bits, uint64_t *f)
{
    uint64_t m = bits >= 64u ? ~0ull : (1ull << bits) - 1u, cf = (*f & RFLAGS_CF) ? 1u : 0u, out;
    uint32_t i, top = bits - 1u;
    int left = sub == KVM_SHIFT_ROL || sub == KVM_SHIFT_RCL;
    int carry = sub == KVM_SHIFT_RCL || sub == KVM_SHIFT_RCR;

    c %= carry ? bits + 1u : bits;
    for (i = 0; i < c; i++) {
        out = left ? (d >> top) & 1u : d & 1u;
        d = left ? ((d << 1) | (carry ? cf : out)) & m : (d >> 1) | ((carry ? cf : out) << top);
        cf = out;
    }
    if (!carry) {
        cf = left ? d & 1u : (d >> top) & 1u;
    } else if (c == 0) {
        return d;
    }
    flags_co(f, cf != 0, left ? ((d >> top) ^ cf) & 1u : ((d >> top) ^ (d >> (top - 1u))) & 1u);
    return d;
}

/** SHL, SHR and SAR of `d` by `c`, not 0, in `bits` bits: the result,
 * with the flags in `*f`. */
static uint64_t shift(uint32_t sub, uint64_t d, uint32_t c, uint32_t bits, uint64_t *f)
{
    uint64_t m = bits >= 64u ? ~0ull : (1ull << bits) - 1u, sign = 1ull << (bits - 1u);
    uint64_t cf = 0, was = d;
    uint32_t i;
    int left = sub == KVM_SHIFT_SHL || sub == KVM_SHIFT_SAL, of;

    for (i = 0; i < c; i++) {
        if (left) {
            cf = (d >> (bits - 1u)) & 1u;
            d = (d << 1) & m;
        } else {
            cf = d & 1u;
            d = (d >> 1) | (sub == KVM_SHIFT_SAR ? d & sign : 0u);
        }
    }
    if (left) {
        of = (((d >> (bits - 1u)) ^ cf) & 1u) != 0;
    } else {
        of = sub == KVM_SHIFT_SHR && c == 1u && (was & sign);
    }
    flags_co(f, cf != 0, of);
    flags_szp(f, d, bits / 8u);
    return d;
}

/** The rotates and shifts by 1, CL or an immediate; a count of 0 changes
 * no flag. */
static int h_shift(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    uint32_t c = (uint32_t)x->s[0] & (in->size == 8u ? 63u : 31u);
    uint64_t *f = &x->v->regs.rflags;

    if (c == 0) {
        return X_OK;
    }
    if (in->sub < KVM_SHIFT_SHL) {
        x->d[0] = rotate(in->sub, x->d[0], c, in->size * 8u, f);
    } else {
        x->d[0] = shift(in->sub, x->d[0], c, in->size * 8u, f);
    }
    return X_OK;
}

/** SHLD and SHRD: dst shifted with src's bits coming in. */
static int h_dshift(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    uint32_t bits = in->size * 8u, top = bits - 1u, i;
    uint32_t c = (uint32_t)x->s2 & (in->size == 8u ? 63u : 31u);
    uint64_t m = size_mask(in->size), d = x->d[0], s = x->s[0], cf = 0, was = d;
    uint64_t *f = &x->v->regs.rflags;

    for (i = 0; i < c; i++) {
        if (in->sub == 0) {
            cf = (d >> top) & 1u;
            d = ((d << 1) | ((s >> top) & 1u)) & m;
            s = ((s << 1) | cf) & m;
        } else {
            cf = d & 1u;
            d = (d >> 1) | ((s & 1u) << top);
            s = (s >> 1) | (cf << top);
        }
    }
    if (c != 0) {
        flags_co(f, cf != 0, (((d ^ was) >> top) & 1u) != 0);
        flags_szp(f, d, in->size);
    }
    x->d[0] = d;
    return X_OK;
}

/** The 128-bit product of `a` and `b` into `*hi` and `*lo`. */
static void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a0 = a & 0xFFFFFFFFull, a1 = a >> 32, b0 = b & 0xFFFFFFFFull, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);

    *lo = (p00 & 0xFFFFFFFFull) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/** The signed 128-bit product of `a` and `b` into `*hi` and `*lo`. */
static void imul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    mul64(a, b, hi, lo);
    if (a >> 63) {
        *hi -= b;
    }
    if (b >> 63) {
        *hi -= a;
    }
}

/** Divide the 128-bit `hi`:`lo` by `d`, not 0, into `*q` and `*r`: 0, or
 * -1 when the quotient does not fit in 64 bits. */
static int div128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *q, uint64_t *r)
{
    uint64_t rem = hi, quo = 0, carry;
    int i;

    if (hi >= d) {
        return -1;
    }
    for (i = 63; i >= 0; i--) {
        carry = rem >> 63;
        rem = (rem << 1) | ((lo >> i) & 1u);
        if (carry || rem >= d) {
            rem -= d;
            quo |= 1ull << i;
        }
    }
    *q = quo;
    *r = rem;
    return 0;
}

/** Store the two halves of a MUL's or DIV's result, `lo` and `hi`, each
 * of the operand's size: in AL and AH for a byte, else in rAX and rDX. */
static void md_store(struct xc *x, uint64_t lo, uint64_t hi)
{
    uint32_t size = x->in->size;

    if (size == 1u) {
        reg_put(x->v, 0, 2, (lo & 0xFFu) | ((hi & 0xFFu) << 8));
    } else {
        reg_put(x->v, 0, size, lo);
        reg_put(x->v, 2, size, hi);
    }
}

/** MUL and IMUL of rAX by the source: CF and OF tell whether the upper
 * half holds more than the lower half's sign. */
static int h_mul(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    uint32_t size = in->size, bits = size * 8u;
    uint64_t m = size_mask(size), a = x->v->regs.rax & m, b = x->s[0] & m, hi, lo;
    int sign = in->sub == KVM_MD_IMUL, over;

    if (sign) {
        a = sx(a, size);
        b = sx(b, size);
    }
    if (size == 8u && sign) {
        imul64(a, b, &hi, &lo);
    } else if (size == 8u) {
        mul64(a, b, &hi, &lo);
    } else {
        lo = (a * b) & m;
        hi = ((a * b) >> bits) & m;
    }
    over = sign ? hi != ((lo >> (bits - 1u)) & 1u ? m : 0u) : hi != 0;
    md_store(x, lo, hi);
    flags_co(&x->v->regs.rflags, over, over);
    return X_OK;
}

/** DIV and IDIV of rDX:rAX, AX for a byte, by the source: #DE for a
 * divisor of 0 or a quotient that does not fit. */
static int h_div(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    struct kvm_vcpu *v = x->v;
    uint32_t size = in->size, bits = size * 8u;
    uint64_t m = size_mask(size), b = x->s[0] & m, hi = 0, lo, q, r, top;
    int sign = in->sub == KVM_MD_IDIV, neg = 0, bneg = 0;

    if (size == 8u) {
        hi = v->regs.rdx;
        lo = v->regs.rax;
    } else if (size == 1u) {
        lo = v->regs.rax & 0xFFFFu;
    } else {
        lo = ((v->regs.rdx & m) << bits) | (v->regs.rax & m);
    }
    if (sign) {
        if (size != 8u) {
            lo = sx(lo, size == 4u ? 8u : size * 2u);
            neg = (int)(lo >> 63);
            lo = neg ? 0u - lo : lo;
        } else if (hi >> 63) {
            neg = 1;
            hi = ~hi + (lo == 0 ? 1u : 0u);
            lo = 0u - lo;
        }
        b = sx(b, size);
        bneg = (int)(b >> 63);
        b = bneg ? 0u - b : b;
    }
    top = sign ? (1ull << (bits - 1u)) - (neg != bneg ? 0u : 1u) : m;
    if (b == 0 || div128(hi, lo, b, &q, &r) != 0 || q > top) {
        return x_fault(x, DE_VECTOR, 0, 0);
    }
    md_store(x, neg != bneg ? 0u - q : q, neg ? 0u - r : r);
    return X_OK;
}

/** MUL, IMUL, DIV and IDIV of the accumulator. */
static int h_muldiv(struct xc *x)
{
    return x->in->sub >= KVM_MD_DIV ? h_div(x) : h_mul(x);
}

/** IMUL with two or three operands: dst = dst * src, or src * src2. */
static int h_imul(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    uint32_t size = in->size;
    uint64_t a = sx(x->s[0], size), hi, lo;
    uint64_t b = sx(in->src2.kind == KVM_OPND_NONE ? x->d[0] : x->s2, size);
    int over;

    imul64(a, b, &hi, &lo);
    if (size == 8u) {
        over = hi != ((lo >> 63) ? ~0ull : 0u);
    } else {
        over = sx(lo, size) != lo;
    }
    x->d[0] = lo;
    flags_co(&x->v->regs.rflags, over, over);
    return X_OK;
}

/** BT, BTS, BTR and BTC: CF is the bit, which the last three change. */
static int h_bt(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    uint64_t bit = 1ull << ((uint32_t)x->s[0] & (in->size * 8u - 1u));
    uint64_t *f = &x->v->regs.rflags;

    *f = (*f & ~RFLAGS_CF) | ((x->d[0] & bit) ? RFLAGS_CF : 0u);
    if (in->sub == KVM_BT_BTS) {
        x->d[0] |= bit;
    } else if (in->sub == KVM_BT_BTR) {
        x->d[0] &= ~bit;
    } else if (in->sub == KVM_BT_BTC) {
        x->d[0] ^= bit;
    }
    return X_OK;
}

/** BSF and BSR: ZF for a source of 0, which leaves dst as it was. */
static int h_bscan(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    uint64_t s = x->s[0] & size_mask(in->size), *f = &x->v->regs.rflags;
    uint32_t i = 0;

    if (s == 0) {
        *f |= RFLAGS_ZF;
        x->write = 0;
        return X_OK;
    }
    *f &= ~RFLAGS_ZF;
    if (in->sub == 0) {
        while (!((s >> i) & 1u)) {
            i++;
        }
    } else {
        i = in->size * 8u - 1u;
        while (!((s >> i) & 1u)) {
            i--;
        }
    }
    x->d[0] = i;
    return X_OK;
}

/** CMPXCHG: rAX compared with dst, which takes src when they are equal;
 * rAX takes dst when they are not, and dst is written back as it was. */
static int h_cmpxchg(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    struct kvm_vcpu *v = x->v;
    uint64_t was = x->d[0];

    (void)alu_run(KVM_ALU_CMP, v->regs.rax, was, in->size, &v->regs.rflags);
    if (v->regs.rflags & RFLAGS_ZF) {
        x->d[0] = x->s[0];
    } else {
        reg_put(v, 0, in->size, was);
    }
    return X_OK;
}

/** CMPXCHG8B: EDX:EAX compared with the 8 bytes of dst, which take
 * ECX:EBX when they are equal; EDX:EAX takes dst when they are not. */
static int h_cmpxchg8b(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    uint64_t was = x->d[0];

    if ((uint32_t)was != (uint32_t)v->regs.rax || (uint32_t)(was >> 32) != (uint32_t)v->regs.rdx) {
        v->regs.rax = was & 0xFFFFFFFFull;
        v->regs.rdx = was >> 32;
        v->regs.rflags &= ~RFLAGS_ZF;
    } else {
        x->d[0] = ((v->regs.rcx & 0xFFFFFFFFull) << 32) | (v->regs.rbx & 0xFFFFFFFFull);
        v->regs.rflags |= RFLAGS_ZF;
    }
    return X_OK;
}

/** XADD: dst takes the sum, src what dst held. */
static int h_xadd(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    uint64_t old[2];

    old[0] = x->d[0];
    old[1] = 0;
    x->d[0] = alu_run(KVM_ALU_ADD, x->d[0], x->s[0], in->size, &x->v->regs.rflags);
    return opnd_write(x, &in->src, x->src_ea, old);
}

/** CMOVcc: dst is written either way, which clears the upper half of a
 * 4-byte register. */
static int h_cmov(struct xc *x)
{
    if (cond(x->v->regs.rflags, x->in->sub)) {
        x->d[0] = x->s[0];
    }
    return X_OK;
}

/** SETcc. */
static int h_setcc(struct xc *x)
{
    x->d[0] = (uint64_t)cond(x->v->regs.rflags, x->in->sub);
    return X_OK;
}

/** `value`'s low `size` bytes in the reverse order. */
static uint64_t swab(uint64_t value, uint32_t size)
{
    uint64_t out = 0;
    uint32_t i;

    for (i = 0; i < size; i++) {
        out = (out << 8) | ((value >> (i * 8u)) & 0xFFu);
    }
    return out;
}

/** MOVBE: #UD for a guest whose CPUID lacks it. */
static int h_movbe(struct xc *x)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(x->v, 1, 0);

    if (!e || !(e->ecx & (1u << 22))) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    x->d[0] = swab(x->s[0], x->in->size);
    return X_OK;
}

/** BSWAP: of a 2-byte register it leaves 0, as the processor does. */
static int h_bswap(struct xc *x)
{
    x->d[0] = x->in->size == 2u ? 0u : swab(x->d[0], x->in->size);
    return X_OK;
}

/** LEA. */
static int h_lea(struct xc *x)
{
    x->d[0] = x->src_ea;
    return X_OK;
}

/** NOP, PAUSE, a prefetch, CLFLUSH. */
static int h_nop(struct xc *x)
{
    (void)x;
    return X_OK;
}

/** PUSH. */
static int h_push(struct xc *x)
{
    return x_push(x, x->s[0], x->in->size);
}

/** POP. */
static int h_pop(struct xc *x)
{
    return x_pop(x, &x->d[0], x->in->size);
}

/** PUSHA: rSP is pushed as it was before the first push. */
static int h_pusha(struct xc *x)
{
    uint64_t sp = x->v->regs.rsp;
    int i, r = X_OK;

    for (i = 0; i < 8 && r == X_OK; i++) {
        r = x_push(x, i == 4 ? sp : *gpr(x->v, i), x->in->size);
    }
    return r;
}

/** POPA: the stack's copy of rSP is passed over. */
static int h_popa(struct xc *x)
{
    uint64_t value;
    int i, r = X_OK;

    for (i = 7; i >= 0 && r == X_OK; i--) {
        if (i == 4) {
            sp_add(x->v, x->in->size);
            continue;
        }
        r = x_pop(x, &value, x->in->size);
        if (r == X_OK) {
            reg_put(x->v, i, x->in->size, value);
        }
    }
    return r;
}

/** PUSHF: the flags without VM. */
static int h_pushf(struct xc *x)
{
    return x_push(x, x->v->regs.rflags & ~RFLAGS_VM, x->in->size);
}

/** POPF, as Linux's emulate_popf: IOPL changes at CPL 0 alone and IF at a
 * CPL within IOPL; virtual-8086 mode below IOPL 3 is #GP(0). */
static int h_popf(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    uint64_t f = v->regs.rflags, value, change;
    uint32_t iopl = (uint32_t)((f & RFLAGS_IOPL) >> 12);
    int r;

    change = RFLAGS_ARITH | RFLAGS_TF | RFLAGS_DF | RFLAGS_NT | RFLAGS_AC | RFLAGS_ID;
    if (!(v->sregs.cr0 & CR0_PE)) {
        change |= RFLAGS_IOPL | RFLAGS_IF;
    } else if (f & RFLAGS_VM) {
        if (iopl < 3u) {
            return x_fault(x, GP_VECTOR, 1, 0);
        }
        change |= RFLAGS_IF;
    } else {
        change |= (x->cpl == 0 ? RFLAGS_IOPL : 0u) | (x->cpl <= iopl ? RFLAGS_IF : 0u);
    }
    r = x_pop(x, &value, x->in->size);
    if (r != X_OK) {
        return r;
    }
    if (x->in->size == 2u) {
        change &= 0xFFFFull;
    }
    v->regs.rflags = (f & ~change) | (value & change) | 2u;
    return X_OK;
}

/** PUSH of a segment register: with a 4-byte operand the selector's two
 * bytes are written and the other two left, as Linux's em_push_sreg. */
static int h_push_sreg(struct xc *x)
{
    uint64_t sel = x_seg(x->v, x->in->sub)->selector;

    if (x->in->size != 4u) {
        return x_push(x, sel, x->in->size);
    }
    sp_add(x->v, 0ull - 2u);
    return x_push(x, sel, 2);
}

/** POP of a segment register; SS leaves the MOV SS shadow. */
static int h_pop_sreg(struct xc *x)
{
    uint64_t sel;
    int r = x_pop(x, &sel, 2);

    if (r != X_OK) {
        return r;
    }
    sp_add(x->v, x->in->size - 2u);
    if (x->in->sub == KVM_SEG_SS) {
        x->shadow = KVM_X86_SHADOW_INT_MOV_SS;
    }
    return seg_load(x, x->in->sub, (uint16_t)sel, XFER_NONE);
}

/** ENTER with no nesting, the form Linux's emulator has. */
static int h_enter(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    uint64_t m = stack_mask(v);
    int r;

    if (x->s2 & 31u) {
        return X_FAIL;
    }
    r = x_push(x, v->regs.rbp, m == 0xFFFFull ? 2u : m == ~0ull ? 8u : 4u);
    if (r == X_OK) {
        v->regs.rbp = (v->regs.rbp & ~m) | (v->regs.rsp & m);
        sp_add(v, 0ull - (x->s[0] & 0xFFFFu));
    }
    return r;
}

/** LEAVE. */
static int h_leave(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    uint64_t m = stack_mask(v), value;
    int r;

    v->regs.rsp = (v->regs.rsp & ~m) | (v->regs.rbp & m);
    r = x_pop(x, &value, x->in->size);
    if (r == X_OK) {
        reg_put(v, 5, x->in->size, value);
    }
    return r;
}

/** Jcc. */
static int h_jcc(struct xc *x)
{
    if (!cond(x->v->regs.rflags, x->in->sub)) {
        return X_OK;
    }
    return x_jump(x, x->next + x->s[0], x->in->size);
}

/** JMP to a relative target. */
static int h_jmp(struct xc *x)
{
    return x_jump(x, x->next + x->s[0], x->in->size);
}

/** JMP to the target in a register or memory. */
static int h_jmp_abs(struct xc *x)
{
    return x_jump(x, x->s[0], x->in->size);
}

/** The offset and selector of a far jump's or call's target: the
 * instruction's own, or the far pointer its memory operand holds. */
static void far_target(const struct xc *x, uint64_t *off, uint16_t *sel)
{
    const struct kvm_insn *in = x->in;

    if (in->src.kind == KVM_OPND_IMM) {
        *off = x->s[0];
        *sel = (uint16_t)x->s2;
    } else {
        *off = bytes_value(x->raw, in->size);
        *sel = (uint16_t)bytes_value(x->raw + in->size, 2);
    }
}

/** JMP far: CS loaded, then RIP. */
static int h_jmp_far(struct xc *x)
{
    uint64_t off;
    uint16_t sel;
    int r;

    far_target(x, &off, &sel);
    r = seg_load(x, KVM_SEG_CS, sel, XFER_JMP);
    return r != X_OK ? r : x_jump(x, off, x->in->size);
}

/** CALL to a relative target, or to one in a register or memory. */
static int h_call(struct xc *x)
{
    uint64_t ret = x->next;
    int r;

    if (x->in->op == KVM_OP_CALL) {
        r = x_jump(x, x->next + x->s[0], x->in->size);
    } else {
        r = x_jump(x, x->s[0], x->in->size);
    }
    return r != X_OK ? r : x_push(x, ret, x->in->size);
}

/** CALL far: CS loaded, RIP set, then the old CS and RIP pushed. */
static int h_call_far(struct xc *x)
{
    uint64_t off, ret = x->next, cs = x->v->sregs.cs.selector;
    uint16_t sel;
    int r;

    far_target(x, &off, &sel);
    r = seg_load(x, KVM_SEG_CS, sel, XFER_JMP);
    if (r == X_OK) {
        r = x_jump(x, off, x->in->size);
    }
    if (r == X_OK) {
        r = x_push(x, cs, x->in->size);
    }
    return r != X_OK ? r : x_push(x, ret, x->in->size);
}

/** RET near, releasing the immediate's bytes of stack. */
static int h_ret(struct xc *x)
{
    uint64_t ip;
    int r = x_pop(x, &ip, x->in->size);

    if (r == X_OK) {
        r = x_jump(x, ip, x->in->size);
    }
    if (r == X_OK && x->in->src.kind == KVM_OPND_IMM) {
        sp_add(x->v, x->s[0]);
    }
    return r;
}

/** RET far to the same privilege level, the one Linux's emulator has. */
static int h_ret_far(struct xc *x)
{
    uint64_t ip, cs;
    int r = x_pop(x, &ip, x->in->size);

    if (r == X_OK) {
        r = x_pop(x, &cs, x->in->size);
    }
    if (r == X_OK) {
        r = seg_load(x, KVM_SEG_CS, (uint16_t)cs, XFER_RET);
    }
    if (r == X_OK) {
        r = x_jump(x, ip, x->in->size);
    }
    if (r == X_OK && x->in->src.kind == KVM_OPND_IMM) {
        sp_add(x->v, x->s[0]);
    }
    return r;
}

/** LOOPNE, LOOPE, LOOP and JrCXZ, on rCX in the address size. */
static int h_loop(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    struct kvm_vcpu *v = x->v;
    uint64_t m = size_mask(in->addr_size);
    int zf = (v->regs.rflags & RFLAGS_ZF) != 0, take;

    if (in->sub == 3u) {
        take = (v->regs.rcx & m) == 0;
    } else {
        reg_add(v, 1, ~0ull, in->addr_size);
        take = (v->regs.rcx & m) != 0 && (in->sub == 2u || (in->sub == 1u ? zf : !zf));
    }
    return take ? x_jump(x, x->next + x->s[0], in->size) : X_OK;
}

/** MOV to a segment register; SS leaves the MOV SS shadow. */
static int h_mov_sreg(struct xc *x)
{
    x->write = 0;
    if (x->in->dst.reg == KVM_SEG_SS) {
        x->shadow = KVM_X86_SHADOW_INT_MOV_SS;
    }
    return seg_load(x, x->in->dst.reg, (uint16_t)x->s[0], XFER_NONE);
}

/** MOV from a segment register, SLDT and STR. */
static int h_store_seg(struct xc *x)
{
    const struct kvm_insn *in = x->in;

    if (in->sub == 0) {
        x->d[0] = x->s[0];
        return X_OK;
    }
    if (!x_prot(x)) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    if ((x->v->sregs.cr4 & CR4_UMIP) && x->cpl > 0) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    x->d[0] = x_seg(x->v, in->sub)->selector;
    return X_OK;
}

/** LLDT and LTR. */
static int h_load_seg(struct xc *x)
{
    int r;

    if (!x_prot(x)) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    r = x_priv(x);
    return r != X_OK ? r : seg_load(x, x->in->sub, (uint16_t)x->s[0], XFER_NONE);
}

/** LES, LDS, LSS, LFS and LGS. */
static int h_lseg(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    int r = seg_load(x, in->sub, (uint16_t)bytes_value(x->raw + in->size, 2), XFER_NONE);

    x->d[0] = bytes_value(x->raw, in->size);
    return r;
}

/** SGDT and SIDT, as Linux's emulate_store_desc_ptr: 8 bytes of base in
 * 64-bit mode, and 24 bits of it with a 2-byte operand. */
static int h_store_dt(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    const struct kvm_dtable *t = in->sub ? &x->v->sregs.idt : &x->v->sregs.gdt;
    uint32_t size = x->mode == 64 ? 8u : in->size;
    uint64_t base = t->base;
    uint8_t b[10];

    x->write = 0;
    if ((x->v->sregs.cr4 & CR4_UMIP) && x->cpl > 0) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    if (size == 2u) {
        size = 4;
        base &= 0xFFFFFFull;
    }
    value_bytes(b, t->limit, 2);
    value_bytes(b + 2, base, size);
    return x_write(x, in->seg, x->dst_ea, b, 2u + size);
}

/** LGDT and LIDT. */
static int h_load_dt(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    struct kvm_dtable *t = in->sub ? &x->v->sregs.idt : &x->v->sregs.gdt;
    uint32_t size = x->mode == 64 ? 8u : in->size;
    uint8_t b[10];
    uint64_t base;
    int r = x_priv(x);

    if (r == X_OK) {
        r = x_read(x, in->seg, x->src_ea, b, 2u + (size == 2u ? 3u : size));
    }
    if (r != X_OK) {
        return r;
    }
    base = bytes_value(b + 2, size == 2u ? 3u : size);
    if (x->mode == 64 && !canonical(base)) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    t->base = base;
    t->limit = (uint16_t)bytes_value(b, 2);
    x->sregs = 1;
    return X_OK;
}

/** SMSW. */
static int h_smsw(struct xc *x)
{
    if ((x->v->sregs.cr4 & CR4_UMIP) && x->cpl > 0) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    x->d[0] = x->v->sregs.cr0;
    return X_OK;
}

/** LMSW: PE, MP, EM and TS from the source; PE is set, never cleared. */
static int h_lmsw(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    int r = x_priv(x);

    if (r == X_OK) {
        v->sregs.cr0 = (v->sregs.cr0 & ~0xEull) | (x->s[0] & 0xFu);
        x->sregs = 1;
    }
    return r;
}

/** CMC, CLC, STC, CLI, STI, CLD and STD; CLI and STI are #GP(0) above
 * IOPL, and STI leaves its shadow. */
static int h_flag(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    uint64_t *f = &v->regs.rflags;
    uint32_t sub = x->in->sub, iopl = (uint32_t)((*f & RFLAGS_IOPL) >> 12);

    if ((sub == KVM_FLAG_CLI || sub == KVM_FLAG_STI) && (v->sregs.cr0 & CR0_PE) &&
        ((*f & RFLAGS_VM) || x->cpl > iopl)) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    switch (sub) {
    case KVM_FLAG_CMC:
        *f ^= RFLAGS_CF;
        break;
    case KVM_FLAG_CLC:
        *f &= ~RFLAGS_CF;
        break;
    case KVM_FLAG_STC:
        *f |= RFLAGS_CF;
        break;
    case KVM_FLAG_CLI:
        *f &= ~RFLAGS_IF;
        break;
    case KVM_FLAG_STI:
        *f |= RFLAGS_IF;
        x->shadow = KVM_X86_SHADOW_INT_STI;
        break;
    case KVM_FLAG_CLD:
        *f &= ~RFLAGS_DF;
        break;
    default:
        *f |= RFLAGS_DF;
        break;
    }
    return X_OK;
}

/** CBW, CWDE and CDQE. */
static int h_cbw(struct xc *x)
{
    uint32_t size = x->in->size;

    reg_put(x->v, 0, size, sx(x->v->regs.rax, size / 2u));
    return X_OK;
}

/** CWD, CDQ and CQO. */
static int h_cwd(struct xc *x)
{
    uint32_t size = x->in->size;

    reg_put(x->v, 2, size, (sx(x->v->regs.rax, size) >> 63) ? ~0ull : 0u);
    return X_OK;
}

/** SAHF. */
static int h_sahf(struct xc *x)
{
    uint64_t *f = &x->v->regs.rflags;
    uint64_t keep = RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF;

    *f = (*f & ~0xFFull) | ((x->v->regs.rax >> 8) & keep) | 2u;
    return X_OK;
}

/** LAHF. */
static int h_lahf(struct xc *x)
{
    struct kvm_vcpu *v = x->v;

    v->regs.rax = (v->regs.rax & ~0xFF00ull) | ((v->regs.rflags & 0xFFu) << 8);
    return X_OK;
}

/** DAS, as Linux's em_das. */
static void bcd_das(struct kvm_vcpu *v)
{
    uint64_t *f = &v->regs.rflags;
    uint32_t al = (uint32_t)v->regs.rax & 0xFFu, was = al;
    int old_cf = (*f & RFLAGS_CF) != 0, cf = 0, af = (*f & RFLAGS_AF) != 0;

    if ((al & 0xFu) > 9u || af) {
        al = (al - 6u) & 0xFFu;
        cf = old_cf || al >= 250u;
        af = 1;
    }
    if (was > 0x99u || old_cf) {
        al = (al - 0x60u) & 0xFFu;
        cf = 1;
    }
    reg_put(v, 0, 1, al);
    flags_co(f, cf, 0);
    *f = (*f & ~RFLAGS_AF) | (af ? RFLAGS_AF : 0u);
    flags_szp(f, al, 1);
}

/** DAS, AAM, AAD and SALC; AAM by 0 is #DE. */
static int h_bcd(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    uint64_t *f = &v->regs.rflags;
    uint32_t al = (uint32_t)v->regs.rax & 0xFFu, ah = ((uint32_t)v->regs.rax >> 8) & 0xFFu;
    uint32_t base = (uint32_t)x->s[0] & 0xFFu;

    switch (x->in->sub) {
    case KVM_BCD_DAS:
        bcd_das(v);
        return X_OK;
    case KVM_BCD_SALC:
        reg_put(v, 0, 1, (*f & RFLAGS_CF) ? 0xFFu : 0u);
        return X_OK;
    case KVM_BCD_AAM:
        if (base == 0) {
            return x_fault(x, DE_VECTOR, 0, 0);
        }
        ah = al / base;
        al %= base;
        break;
    default:
        al = (al + ah * base) & 0xFFu;
        ah = 0;
        break;
    }
    reg_put(v, 0, 2, al | (ah << 8));
    *f &= ~(RFLAGS_CF | RFLAGS_OF | RFLAGS_AF);
    flags_szp(f, al, 1);
    return X_OK;
}

/** #NM for an x87 instruction under CR0.TS or CR0.EM; else the x87 state
 * made ready to read. */
static int x_x87(struct xc *x)
{
    if (x->v->sregs.cr0 & (CR0_TS | CR0_EM)) {
        return x_fault(x, NM_VECTOR, 0, 0);
    }
    fx_ready(x->v, 1);
    return X_OK;
}

/** FNSTCW and FNSTSW. */
static int h_fnst(struct xc *x)
{
    int r = x_x87(x);

    if (r == X_OK) {
        x->d[0] = fx_word(x->v, x->in->op == KVM_OP_FNSTCW ? FX_FCW : FX_FSW);
    }
    return r;
}

/** FNINIT. */
static int h_fninit(struct xc *x)
{
    uint8_t *img = x->v->xsave;
    int r = x_x87(x);

    if (r == X_OK) {
        k_memset(img, 0, 24);
        img[FX_FCW] = 0x7F;
        img[FX_FCW + 1] = 0x03;
    }
    return r;
}

/**
 * The checks of FXSAVE and FXRSTOR, as Linux's check_fxsr, and the bytes
 * of the image they move: 288 with CR4.OSFXSR, which adds the first eight
 * SSE registers, else 160. #UD for a guest whose CPUID lacks FXSR, #NM
 * under CR0.TS or CR0.EM, and X_FAIL in 64-bit mode, whose image Linux's
 * emulator does not move either.
 */
static int x_fxsr(struct xc *x, uint32_t *size)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(x->v, 1, 0);

    if (!e || !(e->edx & (1u << 24))) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    if (x->v->sregs.cr0 & (CR0_TS | CR0_EM)) {
        return x_fault(x, NM_VECTOR, 0, 0);
    }
    if (x->mode == 64) {
        return X_FAIL;
    }
    fx_ready(x->v, 1);
    fx_ready(x->v, 2);
    *size = (x->v->sregs.cr4 & CR4_OSFXSR) ? 288u : 160u;
    return X_OK;
}

/** FXSAVE. */
static int h_fxsave(struct xc *x)
{
    uint32_t size;
    int r = x_fxsr(x, &size);

    x->write = 0;
    if (r == X_OK) {
        r = mem_aligned(x, &x->in->dst, x->dst_ea);
    }
    return r != X_OK ? r : x_write(x, x->in->seg, x->dst_ea, x->v->xsave, size);
}

/** FXRSTOR: #GP(0) for a reserved bit of MXCSR. */
static int h_fxrstor(struct xc *x)
{
    uint8_t img[288];
    uint32_t size;
    int r = x_fxsr(x, &size);

    if (r == X_OK) {
        r = mem_aligned(x, &x->in->src, x->src_ea);
    }
    if (r == X_OK) {
        r = x_read(x, x->in->seg, x->src_ea, img, size);
    }
    if (r != X_OK) {
        return r;
    }
    if (bytes_value(img + 24, 4) >> 16) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    k_memcpy(x->v->xsave, img, size);
    return X_OK;
}

/** Make `s` the flat segment SYSCALL, SYSENTER and SYSEXIT load: code or
 * data at privilege `dpl`, 64-bit code with `l`. */
static void flat_seg(struct kvm_segment *s, uint16_t sel, int code, uint32_t dpl, int l)
{
    k_memset(s, 0, sizeof(*s));
    s->selector = sel;
    s->limit = 0xFFFFFFFFu;
    s->type = (uint8_t)(code ? 0xBu : 3u);
    s->s = 1;
    s->dpl = (uint8_t)dpl;
    s->present = 1;
    s->g = 1;
    s->db = (uint8_t)!l;
    s->l = (uint8_t)(l != 0);
}

/** The MSR `index` of the guest, 0 for one it does not keep. */
static uint64_t x_msr(struct xc *x, uint32_t index)
{
    uint64_t value = 0;

    return kvm_msr_get(x->v, index, &value, 0) == 0 ? value : 0;
}

/** Whether the guest's CPUID names AMD or Hygon, whose processors take
 * SYSCALL outside 64-bit mode and no SYSENTER in compatibility mode. */
static int x_amd(const struct xc *x)
{
    return kvm_guest_amd(x->v);
}

/**
 * SYSCALL, as Linux's em_syscall: #UD in real and virtual-8086 mode,
 * outside 64-bit mode for a guest whose vendor is not AMD's, and without
 * EFER.SCE. The single-step trap goes by TF as SYSCALL leaves it.
 */
static int h_syscall(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    uint64_t star = x_msr(x, MSR_STAR), efer = v->sregs.efer;
    uint16_t cs = (uint16_t)((star >> 32) & 0xFFFCu);
    int lma = (efer & EFER_LMA) != 0;

    if (!x_prot(x) || (x->mode != 64 && !x_amd(x)) || !(efer & EFER_SCE)) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    flat_seg(&v->sregs.cs, cs, 1, 0, lma);
    flat_seg(&v->sregs.ss, (uint16_t)((star >> 32) + 8u), 0, 0, 0);
    x->sregs = 1;
    v->regs.rcx = x->next;
    if (lma) {
        v->regs.r11 = v->regs.rflags;
        x->next = x_msr(x, x->mode == 64 ? MSR_LSTAR : MSR_CSTAR);
        v->regs.rflags = (v->regs.rflags & ~x_msr(x, MSR_SYSCALL_MASK)) | 2u;
    } else {
        x->next = star & 0xFFFFFFFFull;
        v->regs.rflags &= ~(RFLAGS_VM | RFLAGS_IF);
    }
    x->tf_after = 1;
    return X_OK;
}

/**
 * SYSENTER, as Linux's em_sysenter: #GP(0) in real mode and for a null
 * SYSENTER_CS, #UD in compatibility mode for a guest whose vendor is
 * AMD's, and X_FAIL from 64-bit mode, which Linux's emulator leaves out.
 */
static int h_sysenter(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    uint64_t cs = x_msr(x, MSR_IA32_SYSENTER_CS), m;
    int lma = (v->sregs.efer & EFER_LMA) != 0;

    if (!(v->sregs.cr0 & CR0_PE)) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    if (x->mode != 64 && lma && x_amd(x)) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    if (x->mode == 64) {
        return X_FAIL;
    }
    if (!(cs & 0xFFFCu)) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    m = lma ? ~0ull : 0xFFFFFFFFull;
    v->regs.rflags &= ~(RFLAGS_VM | RFLAGS_IF);
    flat_seg(&v->sregs.cs, (uint16_t)(cs & 0xFFFCu), 1, 0, lma);
    flat_seg(&v->sregs.ss, (uint16_t)((cs & 0xFFFCu) + 8u), 0, 0, 0);
    x->sregs = 1;
    x->next = x_msr(x, MSR_IA32_SYSENTER_EIP) & m;
    v->regs.rsp = x_msr(x, MSR_IA32_SYSENTER_ESP) & m;
    return X_OK;
}

/**
 * SYSEXIT, as Linux's em_sysexit: #GP(0) in real and virtual-8086 mode,
 * above CPL 0, for a null SYSENTER_CS, and to 64-bit mode for an RCX or
 * RDX that is not canonical. REX.W returns to 64-bit mode.
 */
static int h_sysexit(struct xc *x)
{
    struct kvm_vcpu *v = x->v;
    uint64_t cs = x_msr(x, MSR_IA32_SYSENTER_CS), rcx = v->regs.rcx, rdx = v->regs.rdx;
    int wide = x->in->size == 8u;

    if (!x_prot(x) || x->cpl > 0 || (wide ? cs == 0 : !(cs & 0xFFFCu))) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    if (wide && (!canonical(rcx) || !canonical(rdx))) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    if (!wide) {
        rcx &= 0xFFFFFFFFull;
        rdx &= 0xFFFFFFFFull;
    }
    cs += wide ? 32u : 16u;
    flat_seg(&v->sregs.cs, (uint16_t)(cs | 3u), 1, 3, wide);
    flat_seg(&v->sregs.ss, (uint16_t)((cs + 8u) | 3u), 0, 3, 0);
    x->sregs = 1;
    x->next = rdx;
    v->regs.rsp = rcx;
    return X_OK;
}

/**
 * VMCALL or VMMCALL that the host's processor refused, as Linux's
 * em_hypercall and emulator_fix_hypercall: the host's own of the two is
 * written over the instruction, which then runs again, RIP left on it.
 * VMCALL is #GP(0) above CPL 0, and VMMCALL #UD in real and virtual-8086
 * mode, as Linux's table marks them. With KVM_X86_QUIRK_FIX_HYPERCALL_INSN
 * disabled nothing is written and the guest takes #UD.
 */
static int h_hypercall(struct xc *x)
{
    static const uint8_t vmcall[3] = {0x0F, 0x01, 0xC1}, vmmcall[3] = {0x0F, 0x01, 0xD9};
    struct kvm_vcpu *v = x->v;

    if (x->in->sub == 1 && !x_prot(x)) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    if (x->in->sub == 0 && x->cpl > 0) {
        return x_fault(x, GP_VECTOR, 1, 0);
    }
    if (!KVM_QUIRK(v->vm, KVM_X86_QUIRK_FIX_HYPERCALL_INSN)) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    x->next = v->regs.rip;
    return wq_add(v, v->sregs.cs.base + v->regs.rip, kvm_backend == &kvm_svm ? vmmcall : vmcall, 3,
                  3);
}

/** RDPID, as Linux's em_rdpid: IA32_TSC_AUX into the register, or #UD
 * for a guest whose CPUID lacks RDPID. */
static int h_rdpid(struct xc *x)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(x->v, 7, 0);

    if (!e || !(e->ecx & (1u << 22))) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    x->d[0] = x_msr(x, MSR_TSC_AUX);
    return X_OK;
}

/* The function that carries out each operation, by KVM_OP_*. */
static int (*const handlers[KVM_OP_LAST + 1])(struct xc *) = {
    0,          h_mov,       h_alu,       h_unary,    h_xchg,      h_shift,   h_muldiv,    h_imul,
    h_bt,       h_bscan,     h_dshift,    h_cmpxchg,  h_cmpxchg8b, h_xadd,    h_cmov,      h_setcc,
    h_movbe,    h_bswap,     h_lea,       h_nop,      h_push,      h_pop,     h_pusha,     h_popa,
    h_pushf,    h_popf,      h_push_sreg, h_pop_sreg, h_enter,     h_leave,   h_jcc,       h_jmp,
    h_jmp_abs,  h_jmp_far,   h_call,      h_call,     h_call_far,  h_ret,     h_ret_far,   h_loop,
    h_mov_sreg, h_store_seg, h_load_seg,  h_lseg,     h_store_dt,  h_load_dt, h_smsw,      h_lmsw,
    h_flag,     h_cbw,       h_cwd,       h_sahf,     h_lahf,      h_bcd,     h_fnst,      h_fnst,
    h_fninit,   h_fxsave,    h_fxrstor,   h_syscall,  h_sysenter,  h_sysexit, h_hypercall, h_rdpid};

/**
 * Step the registers of a string instruction after one element: rSI and
 * rDI by the element's size, down under DF, and under REP rCX, with
 * `x->again` set while elements are left and, for CMPS and SCAS, ZF agrees
 * with the prefix.
 */
static void x_string(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    struct kvm_vcpu *v = x->v;
    uint64_t step = (v->regs.rflags & RFLAGS_DF) ? 0ull - in->size : in->size;
    int zf = (v->regs.rflags & RFLAGS_ZF) != 0, done;

    if (in->src.kind == KVM_OPND_MEM && in->src.mem == KVM_MEM_SI) {
        reg_add(v, 6, step, in->addr_size);
    }
    if (in->dst.kind == KVM_OPND_MEM && in->dst.mem == KVM_MEM_DI) {
        reg_add(v, 7, step, in->addr_size);
    }
    if (!(in->flags & (KVM_INSN_REP | KVM_INSN_REPNE))) {
        return;
    }
    reg_add(v, 1, ~0ull, in->addr_size);
    done = (v->regs.rcx & size_mask(in->addr_size)) == 0;
    if (in->op == KVM_OP_ALU && in->sub == KVM_ALU_CMPR) {
        done = done || ((in->flags & KVM_INSN_REP) ? !zf : zf);
    }
    x->again = !done;
}

/**
 * One pass over the instruction: its operands read, its operation run on
 * the record's registers, and its destination and other writes queued.
 * X_OK; X_FAULT with the guest's exception queued; X_IO; X_FAIL; -EFAULT.
 */
static int x_exec(struct xc *x)
{
    const struct kvm_insn *in = x->in;
    int string = (in->flags & KVM_INSN_STRING) != 0, r;

    if (in->flags & KVM_INSN_UD) {
        return x_fault(x, UD_VECTOR, 0, 0);
    }
    if (string && (in->flags & (KVM_INSN_REP | KVM_INSN_REPNE)) &&
        (x->v->regs.rcx & size_mask(in->addr_size)) == 0) {
        return X_OK;
    }
    r = x_simd(x);
    if (r == X_OK) {
        r = x_operands(x);
    }
    if (r == X_OK) {
        r = handlers[in->op](x);
    }
    if (r == X_OK && x->write && !(in->flags & KVM_INSN_NOWRITE)) {
        r = opnd_write(x, &in->dst, x->dst_ea, x->d);
    }
    if (r == X_OK && string) {
        x_string(x);
    }
    return r;
}

/**
 * Keep what a pass that ended well did to the registers, which were
 * `before`: RIP, unless a REP iteration has more to come; the interrupt
 * shadow, which an instruction that leaves the one it ran under ends, as
 * Linux's toggle_interruptibility; and the single-step trap, by TF as it
 * was before the instruction.
 */
static void x_commit(struct xc *x, const struct kvm_regs *before)
{
    struct kvm_vcpu *v = x->v;
    int tf = (before->rflags & RFLAGS_TF) != 0;
    uint8_t shadow = (uint8_t)x->shadow;

    if (x->sregs) {
        v->dirty |= KVM_PART_SREGS;
    }
    /* RF stays set while a REP repeats and clears once the instruction
     * ends, as Linux's x86_emulate_insn leaves it. */
    if (x->again) {
        v->regs.rflags |= RFLAGS_RF;
        return;
    }
    v->regs.rflags &= ~RFLAGS_RF;
    v->regs.rip = kvm_emul_mode(v) == 64 ? x->next : (x->next & 0xFFFFFFFFull);
    if (v->events.interrupt.shadow & shadow) {
        shadow = 0;
    }
    v->if_flag = (v->regs.rflags & RFLAGS_IF) != 0;
    if (v->events.interrupt.shadow != shadow) {
        v->events.interrupt.shadow = shadow;
        v->dirty |= KVM_PART_EVENTS;
    }
    v->irq_allowed = v->if_flag && !shadow;
    step_trap(v, x->tf_after ? (v->regs.rflags & RFLAGS_TF) != 0 : tf);
}

/** End the instruction under way: the stage idle, nothing kept. */
static void emul_end(struct kvm_vcpu *v)
{
    v->emul.stage = EMUL_IDLE;
    v->emul.resume = 0;
    v->emul.rcount = 0;
    wq_reset(v);
}

/**
 * Refuse the instruction `bytes` of `n` bytes, as Linux's
 * x86_emulate_instruction and handle_emulation_failure: the guest takes
 * #UD, and at CPL 0 the caller gets the internal error besides, with the
 * instruction's bytes and the exit's numbers; under
 * KVM_CAP_EXIT_ON_EMULATION_FAILURE the caller gets the error at every
 * level and the guest no #UD. One a #UD exit brought that was not decoded,
 * `decoded` 0, or is none of those run after a #UD, is the guest's #UD
 * alone, as the processor raised it.
 */
static void emul_fail(struct kvm_vcpu *v, const uint8_t *bytes, uint32_t n, int decoded)
{
    int failure = decoded || !v->emul.ud, report = v->vm->exit_on_emulation_error;

    v->exit.kind = KVM_X_AGAIN;
    if (!failure || !report) {
        kvm_queue_exception(v, UD_VECTOR, 0, 0);
    }
    if (failure) {
        v->stat[KVM_VCPU_STAT_INSN_EMULATION_FAIL]++;
    }
    if (failure && (report || kvm_emul_cpl(v) == 0)) {
        v->exit.kind = KVM_X_INTERNAL;
        kvm_decode_fail(&v->exit.internal, bytes, n, v->emul.info);
    }
    emul_end(v);
}

/**
 * Run the instruction in `v->emul.insn` once more from its start. A pass
 * that needs an MMIO read leaves `v->exit` that read and the registers,
 * the x87 and SSE state among them, as they were; one that faults leaves
 * the exception queued; one that cannot go on is refused; and one that
 * ends keeps its registers, makes its writes to RAM and gives `v->exit`
 * its first write to device memory. 0, or -EFAULT.
 */
static int emul_run(struct kvm_vcpu *v)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_regs regs = v->regs;
    struct kvm_sregs sregs = v->sregs;
    struct xc x;
    uint64_t cr2;
    int r;

    k_memset(&x, 0, sizeof(x));
    x.v = v;
    x.in = &em->insn;
    x.mode = kvm_emul_mode(v);
    x.cpl = (uint32_t)kvm_emul_cpl(v);
    x.next = v->regs.rip + em->insn.len;
    x.write = 1;
    wq_reset(v);
    em->stage = EMUL_IDLE;
    em->resume = 0;
    v->exit.kind = KVM_X_AGAIN;
    k_memcpy(em->fx, v->xsave, sizeof(em->fx));
    r = x_exec(&x);
    if (r == X_OK) {
        r = wq_commit(v);
    }
    if (r != X_OK) {
        /* A fault's CR2 stays; the rest is as the instruction found it. */
        cr2 = v->sregs.cr2;
        v->regs = regs;
        v->sregs = sregs;
        k_memcpy(v->xsave, em->fx, sizeof(em->fx));
        if (r == X_IO) {
            em->stage = EMUL_READ;
            return 0;
        }
        if (r == X_FAULT) {
            v->sregs.cr2 = cr2;
        }
        if (r == X_FAIL) {
            emul_fail(v, em->insn.bytes, em->insn.len, 1);
        }
        emul_end(v);
        return r < X_FAULT ? r : 0;
    }
    em->rcount = 0;
    x_commit(&x, &regs);
    if (wq_next(v)) {
        em->resume = wq_pending(v);
        em->stage = em->resume ? EMUL_WRITE : EMUL_IDLE;
    }
    return 0;
}

/** Whether Linux's emulator carries `in` out after a #UD, its
 * EmulateOnUD: SYSCALL, SYSENTER, SYSEXIT, MOVBE, RDPID, VMCALL and
 * VMMCALL. */
static int runs_on_ud(const struct kvm_insn *in)
{
    if (in->flags & KVM_INSN_UD) {
        return 0;
    }
    return in->op == KVM_OP_SYSCALL || in->op == KVM_OP_SYSENTER || in->op == KVM_OP_SYSEXIT ||
           in->op == KVM_OP_MOVBE || in->op == KVM_OP_HYPERCALL || in->op == KVM_OP_RDPID;
}

/**
 * Fetch the instruction at CS:RIP and decode it; for a #UD exit, `ud`,
 * keep only those Linux's emulator carries out after one. Then run it.
 */
static int emul_begin(struct kvm_vcpu *v, int ud)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_insn *in = &em->insn;
    uint8_t bytes[KVM_INSN_MAX];
    int n, known;

    emul_end(v);
    em->ud = ud;
    v->exit.kind = KVM_X_AGAIN;
    n = fetch(v, bytes);
    if (n < 0) {
        return 0;
    }
    v->stat[KVM_VCPU_STAT_INSN_EMULATION]++;
    known = kvm_decode(bytes, (uint32_t)n, kvm_emul_mode(v), in) == 0;
    if (known && ud) {
        known = runs_on_ud(in);
    }
    if (!known) {
        emul_fail(v, bytes, (uint32_t)n, 0);
        return 0;
    }
    return emul_run(v);
}

int kvm_emul_mmio(struct kvm_vcpu *v, const uint64_t info[5])
{
    k_memcpy(v->emul.info, info, sizeof(v->emul.info));
    return emul_begin(v, 0);
}

int kvm_emul_ud(struct kvm_vcpu *v, const uint64_t info[5])
{
    k_memcpy(v->emul.info, info, sizeof(v->emul.info));
    return emul_begin(v, 1);
}

/* ---- port I/O ---- */

/** Step the string register `n` by `count` elements of `size` bytes, down
 * under DF, within the instruction's address size. */
static void string_step(struct kvm_vcpu *v, int n, uint32_t size, uint32_t addr_size,
                        uint32_t count)
{
    uint64_t step = (uint64_t)size * count;

    reg_add(v, n, (v->regs.rflags & RFLAGS_DF) ? (uint64_t)0 - step : step, addr_size);
}

/** Count `done` elements of a REP string instruction off rCX: whether the
 * instruction is finished, with RIP stepped past it, or repeats. RF stays
 * set while it repeats and clears once it ends, as Linux's
 * x86_emulate_insn leaves it. */
static int string_done(struct kvm_vcpu *v, int rep, uint32_t addr_size, uint32_t done, uint32_t len)
{
    if (rep) {
        reg_add(v, 1, (uint64_t)0 - done, addr_size);
        if ((v->regs.rcx & size_mask(addr_size)) != 0) {
            v->regs.rflags |= RFLAGS_RF;
            return 0;
        }
    }
    v->regs.rflags &= ~RFLAGS_RF;
    kvm_emul_skip(v, len);
    return 1;
}

/**
 * Read the `count` elements of a string OUT from the guest's memory into
 * the exit's bytes and step the instruction. A source in device memory is
 * read one element at a time, through the MMIO answers kept, so `single`
 * is set on the run that follows an answer. 0, or -EFAULT.
 */
static int pio_outs(struct kvm_vcpu *v, const struct kvm_pio *p, uint32_t count, int single)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_exit *ex = &v->exit;
    uint64_t la = linear(v, p->seg, v->regs.rsi & size_mask(p->addr_size));
    uint32_t rpos = 0;
    int r;

    if (single) {
        count = 1;
    }
    r = la_read(v, la, ex->data, count * p->size, &rpos);
    if (r == X_IO && count > 1u) {
        em->rcount = 0;
        rpos = 0;
        count = 1;
        r = la_read(v, la, ex->data, p->size, &rpos);
    }
    ex->io.count = count;
    em->pio.count = count;
    if (r == X_IO) {
        em->stage = EMUL_PIO_READ;
        return 0;
    }
    emul_end(v);
    if (r != X_OK) {
        ex->kind = KVM_X_AGAIN;
        if (r == X_FAIL) {
            ex->kind = KVM_X_INTERNAL;
            kvm_decode_fail(&ex->internal, NULL, 0, NULL);
        }
        return r < X_FAULT ? r : 0;
    }
    ex->kind = KVM_X_IO;
    string_step(v, 6, p->size, p->addr_size, count);
    (void)string_done(v, p->rep, p->addr_size, count, p->len);
    return 0;
}

/**
 * The elements of the string port instruction `p` that one exit moves,
 * from the registers as they stand: rCX's count under REP, else one, and
 * at most the rest of the page of the first element, and of the port I/O
 * page, and the elements before the offset wraps at the address size; one
 * element when DF counts down. 0 when REP finds rCX 0.
 */
static uint64_t string_count(const struct kvm_vcpu *v, const struct kvm_pio *p)
{
    uint64_t m = size_mask(p->addr_size), count = 1, la, room, off;

    if (p->rep) {
        count = v->regs.rcx & m;
    }
    off = (p->in ? v->regs.rdi : v->regs.rsi) & m;
    la = linear(v, p->in ? KVM_SEG_ES : p->seg, off);
    room = (4096u - (la & 0xFFFu)) / p->size;
    if (room != 0 && room - 1u > (m - off) / p->size) {
        room = (m - off) / p->size + 1u;
    }
    if (room == 0 || (v->regs.rflags & RFLAGS_DF)) {
        room = 1;
    }
    return count > room ? room : count;
}

/** Begin the port I/O instruction `p`, as `kvm_emul_pio`; `single` is set
 * when a string OUT runs again with the answer of an MMIO read. */
static int pio_begin(struct kvm_vcpu *v, const struct kvm_pio *p, int single)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_exit *ex = &v->exit;
    uint64_t count = 1;

    ex->kind = KVM_X_IO;
    k_memset(&ex->io, 0, sizeof(ex->io));
    ex->io.direction = (uint8_t)(p->in ? KVM_EXIT_IO_IN : KVM_EXIT_IO_OUT);
    ex->io.size = (uint8_t)p->size;
    ex->io.port = p->port;
    em->pio = *p;
    if (p->string) {
        count = string_count(v, p);
        if (count == 0) {
            v->regs.rflags &= ~RFLAGS_RF;
            kvm_emul_skip(v, p->len);
            ex->kind = KVM_X_AGAIN;
            return 0;
        }
    }
    ex->io.count = (uint32_t)count;
    em->pio.count = (uint32_t)count;
    if (p->in) {
        em->stage = EMUL_PIO;
        return 0;
    }
    if (!p->string) {
        value_bytes(ex->data, v->regs.rax, p->size);
        /* Linux steps RIP before the caller sees the exit for port 0x7E
         * alone, which an old caller counts on. */
        if (p->port == 0x7Eu && KVM_QUIRK(v->vm, KVM_X86_QUIRK_OUT_7E_INC_RIP)) {
            kvm_emul_skip(v, p->len);
            return 0;
        }
        em->stage = EMUL_PIO_OUT;
        em->out_rip = linear(v, KVM_SEG_CS, v->regs.rip);
        return 0;
    }
    return pio_outs(v, p, (uint32_t)count, single);
}

int kvm_emul_pio(struct kvm_vcpu *v, const struct kvm_pio *p)
{
    emul_end(v);
    return pio_begin(v, p, 0);
}

/**
 * Finish a port read with the bytes the caller or a device answered in
 * `v->exit.data`: the accumulator, or the string at ES:rDI, whose elements
 * in device memory leave as MMIO writes, one for each. A string moves the
 * elements its registers allow now, at most those the exit asked for: a
 * caller may change rCX or rDI between the exit and the next KVM_RUN, and
 * as Linux's pio_in_emulated counts again from them, a smaller rCX moves
 * fewer elements, so that rCX reaches 0 and not past it, and rCX 0 ends
 * the instruction with none. X_OK; X_FAULT with the fault queued; or, for
 * a string that cannot be written, X_FAIL or -EFAULT, with the registers
 * as they were.
 */
static int pio_complete(struct kvm_vcpu *v)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_pio *p = &em->pio;
    uint64_t x;
    uint32_t count;
    int r;

    emul_end(v);
    if (!p->string) {
        x = p->size < 4u ? v->regs.rax : 0;
        x = (x & ~size_mask(p->size)) | bytes_value(v->exit.data, p->size);
        v->regs.rax = x;
        kvm_emul_skip(v, p->len);
        return X_OK;
    }
    x = string_count(v, p);
    count = x < p->count ? (uint32_t)x : p->count;
    if (count != 0) {
        r = wq_add(v, linear(v, KVM_SEG_ES, v->regs.rdi & size_mask(p->addr_size)), v->exit.data,
                   count * p->size, p->size);
        if (r == X_OK) {
            r = wq_commit(v);
        }
        if (r != X_OK) {
            wq_reset(v);
            return r;
        }
    }
    string_step(v, 7, p->size, p->addr_size, count);
    (void)string_done(v, p->rep, p->addr_size, count, p->len);
    if (wq_pending(v)) {
        em->stage = EMUL_WRITE;
        em->resume = 1;
    }
    return X_OK;
}

/** A port instruction ended outside the backend's `exit`, which reads
 * `step_trap`: under the caller's single step the next step reports it,
 * as Linux's kvm_skip_emulated_instruction does from a completion. */
static void pio_stepped(struct kvm_vcpu *v)
{
    struct kvm_emul *em = &v->emul;

    if (em->step_trap && em->stage == EMUL_IDLE) {
        em->stage = EMUL_STEP;
        em->resume = 1;
    }
    em->step_trap = 0;
}

int kvm_emul_complete(struct kvm_vcpu *v)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_emul_read *c;
    int r;

    if (em->stage == EMUL_PIO && v->exit.kind == KVM_X_IO) {
        r = pio_complete(v);
        if (r == X_FAIL || r < X_FAULT) {
            /* The next step, with no guest entered, reports it. */
            em->stage = EMUL_FAIL;
            em->fail = r;
            em->resume = 1;
            return 0;
        }
        if (r == X_OK) {
            pio_stepped(v);
        }
        em->step_trap = 0;
        return 0;
    }
    if (em->stage == EMUL_PIO_OUT) {
        /* As Linux's complete_fast_pio_out: a caller that moved RIP
         * meanwhile keeps its RIP. */
        emul_end(v);
        if (linear(v, KVM_SEG_CS, v->regs.rip) == em->out_rip) {
            kvm_emul_skip(v, em->pio.len);
            pio_stepped(v);
        }
        return 0;
    }
    if ((em->stage != EMUL_READ && em->stage != EMUL_PIO_READ) || v->exit.kind != KVM_X_MMIO ||
        em->rcount >= KVM_EMUL_READS) {
        return 0;
    }
    c = &em->rd[em->rcount++];
    k_memcpy(c->data, v->exit.mmio.data, c->len);
    em->stage = em->stage == EMUL_READ ? EMUL_RUN : EMUL_PIO_RUN;
    em->resume = 1;
    return 0;
}

int kvm_emul_resume(struct kvm_vcpu *v)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_pio p;

    em->resume = 0;
    v->exit.kind = KVM_X_AGAIN;
    switch (em->stage) {
    case EMUL_RUN:
        return emul_run(v);
    case EMUL_PIO_RUN:
        p = em->pio;
        return pio_begin(v, &p, 1);
    case EMUL_WRITE:
        if (wq_next(v)) {
            em->resume = wq_pending(v);
        }
        if (!em->resume) {
            em->stage = EMUL_IDLE;
        }
        return 0;
    case EMUL_STEP:
        em->stage = EMUL_IDLE;
        em->step_trap = 1;
        return 0;
    case EMUL_FAIL:
        /* As `pio_outs` reports the same failures. */
        em->stage = EMUL_IDLE;
        if (em->fail != X_FAIL) {
            return em->fail;
        }
        v->exit.kind = KVM_X_INTERNAL;
        kvm_decode_fail(&v->exit.internal, NULL, 0, NULL);
        return 0;
    default:
        return 0;
    }
}
