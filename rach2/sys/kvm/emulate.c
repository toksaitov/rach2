/*
 * emulate.c: the guest instructions a backend's exits hand to the kernel,
 * the same on VMX and SVM: CPUID, XSETBV, the guest's RDMSR and WRMSR,
 * port I/O with its string forms, and the MMIO accesses `kvm_decode`
 * decodes, with the walk of the guest's page tables and the reads and
 * writes of guest memory they need.
 */

#include "kvm.h"

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
#define RFLAGS_VM    0x20000ull
#define RFLAGS_ARITH (RFLAGS_CF | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF | RFLAGS_SF | RFLAGS_OF)

#define DR6_BS 0x4000ull

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
#define MSR_IA32_UCODE_REV       0x08Bu

/* IA32_FEATURE_CONTROL as a guest without VMX, SMX or LMCE reads it:
 * locked with nothing enabled, the state Linux's KVM reaches once such a
 * guest's firmware locks it. */
#define FEATURE_CONTROL_LOCKED 0x1ull

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

void kvm_emul_skip(struct kvm_vcpu *v, uint32_t len)
{
    uint64_t rip = v->regs.rip + len;

    v->regs.rip = kvm_emul_mode(v) == 64 ? rip : (rip & 0xFFFFFFFFull);
    if (v->events.interrupt.shadow) {
        v->events.interrupt.shadow = 0;
        v->dirty |= KVM_PART_EVENTS;
    }
    /* A trap flag set before the instruction raises #DB after it, as the
     * processor would have; under the caller's single step the backend
     * ends the exit as KVM_EXIT_DEBUG instead, as Linux's
     * kvm_vcpu_do_singlestep. */
    if ((v->guest_debug.control & KVM_GUESTDBG_ENABLE) &&
        (v->guest_debug.control & KVM_GUESTDBG_SINGLESTEP)) {
        v->emul.step_trap = 1;
    } else if (v->regs.rflags & RFLAGS_TF) {
        v->debugregs.dr6 |= DR6_BS;
        v->dirty |= KVM_PART_DEBUG;
        kvm_queue_exception(v, DB_VECTOR, 0, 0);
    }
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

/* ---- CPUID, XSETBV and MSRs ---- */

void kvm_emul_cpuid(struct kvm_vcpu *v)
{
    uint32_t function = (uint32_t)v->regs.rax, index = (uint32_t)v->regs.rcx;
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, function, index);
    const struct kvm_cpuid_entry2 *top;
    uint32_t a = 0, b = 0, c = 0, d = 0;

    if (!e) {
        /* A leaf past the highest basic one reads as that leaf, as on
         * Intel's processors and Linux's kvm_cpuid. */
        top = kvm_cpuid_find(v, function & 0x80000000u, 0);
        if (top && function > top->eax && !(function & 0x80000000u)) {
            e = kvm_cpuid_find(v, top->eax, index);
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
        } else if (function == 7 && index == 0) {
            c = (v->sregs.cr4 & CR4_PKE) ? (c | (1u << 4)) : (c & ~(1u << 4));
        }
    }
    v->regs.rax = a;
    v->regs.rbx = b;
    v->regs.rcx = c;
    v->regs.rdx = d;
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
    v->dirty |= KVM_PART_FPU;
    return 0;
}

int kvm_emul_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value)
{
    int r;

    r = kvm_lapic_msr(v, index, write, value, 0);
    if (r != 1) {
        return r;
    }
    switch (index) {
    case MSR_IA32_PLATFORM_ID:
    case MSR_EBL_CR_POWERON:
        if (write) {
            return -1;
        }
        *value = 0;
        return 0;
    case MSR_IA32_FEATURE_CONTROL:
        if (write) {
            return -1;
        }
        *value = FEATURE_CONTROL_LOCKED;
        return 0;
    case MSR_IA32_UCODE_REV:
        /* Linux's guests write 0 before reading the revision. */
        if (write) {
            return 0;
        }
        return kvm_msr_feature(index, value) == 0 ? 0 : -1;
    default:
        break;
    }
    return write ? kvm_msr_set(v, index, *value) : kvm_msr_get(v, index, value);
}

/* ---- the MMIO instructions ---- */

/* The stages of an instruction under way, `v->emul.stage`. */
#define EMUL_IDLE  0
#define EMUL_READ  1 /* the read of the memory operand waits for its answer */
#define EMUL_WRITE 2 /* the write follows at the next step */
#define EMUL_PIO   3 /* a port read waits for its answer */

/** The register operand `n` of `size` bytes, AH to BH with `high8`. */
static uint64_t reg_read(struct kvm_vcpu *v, int n, uint32_t size, int high8)
{
    if (high8 && size == 1u && n >= 4 && n < 8) {
        return (*gpr(v, n - 4) >> 8) & 0xFFu;
    }
    return *gpr(v, n) & size_mask(size);
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

/** Write `value` to the register operand `n` of `size` bytes as the
 * processor does: a 4-byte write clears the upper half, a 1- or 2-byte
 * write keeps the rest. */
static void reg_write(struct kvm_vcpu *v, int n, uint32_t size, int high8, uint64_t value)
{
    uint64_t *r;

    if (high8 && size == 1u && n >= 4 && n < 8) {
        r = gpr(v, n - 4);
        *r = (*r & ~0xFF00ull) | ((value & 0xFFu) << 8);
        return;
    }
    r = gpr(v, n);
    if (size >= 8u) {
        *r = value;
    } else if (size == 4u) {
        *r = value & 0xFFFFFFFFull;
    } else {
        *r = (*r & ~size_mask(size)) | (value & size_mask(size));
    }
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
    if (res == 0) {
        f |= RFLAGS_ZF;
    }
    if (res & sign) {
        f |= RFLAGS_SF;
    }
    if (parity(res)) {
        f |= RFLAGS_PF;
    }
    *rflags = f;
    return res;
}

/** The value `bytes` holds, `size` bytes little-endian. */
static uint64_t bytes_value(const uint8_t *bytes, uint32_t size)
{
    uint64_t x = 0;
    uint32_t i;

    for (i = 0; i < size && i < 8u; i++) {
        x |= (uint64_t)bytes[i] << (i * 8u);
    }
    return x;
}

/** Store `x` in `bytes`, `size` bytes little-endian. */
static void value_bytes(uint8_t *bytes, uint64_t x, uint32_t size)
{
    uint32_t i;

    for (i = 0; i < size && i < 8u; i++) {
        bytes[i] = (uint8_t)(x >> (i * 8u));
    }
}

/** Make `v->exit` the MMIO access of `size` bytes at `gpa`: a write of
 * `value`, or a read whose answer comes back through `kvm_emul_complete`. */
static void mmio_exit(struct kvm_vcpu *v, uint64_t gpa, uint32_t size, int write, uint64_t value)
{
    struct kvm_exit *ex = &v->exit;

    ex->kind = KVM_X_MMIO;
    k_memset(&ex->mmio, 0, sizeof(ex->mmio));
    ex->mmio.phys_addr = gpa;
    ex->mmio.len = size;
    ex->mmio.is_write = (uint8_t)(write != 0);
    if (write) {
        value_bytes(ex->mmio.data, value, size);
    }
}

/** The source operand of a store, an ALU operation or an XCHG: the
 * immediate or the register, `size` bytes. */
static uint64_t source(struct kvm_vcpu *v, const struct kvm_insn *in)
{
    if (in->flags & KVM_INSN_IMM) {
        return in->imm & size_mask(in->size);
    }
    return reg_read(v, in->reg, in->size, (in->flags & KVM_INSN_HIGH8) != 0);
}

/** Step the string register `n` by one element of `size` bytes, down
 * under DF, within the instruction's address size. */
static void string_step(struct kvm_vcpu *v, int n, uint32_t size, uint32_t addr_size,
                        uint32_t count)
{
    uint64_t *r = gpr(v, n), m = size_mask(addr_size), step = (uint64_t)size * count;
    uint64_t x = (v->regs.rflags & RFLAGS_DF) ? (*r - step) : (*r + step);

    *r = (*r & ~m) | (x & m);
}

/** Count `done` elements of a REP string instruction off rCX: whether the
 * instruction is finished, with RIP stepped past it, or repeats. */
static int string_done(struct kvm_vcpu *v, int rep, uint32_t addr_size, uint32_t done, uint32_t len)
{
    uint64_t m = size_mask(addr_size), *c = gpr(v, 1);

    if (rep) {
        *c = (*c & ~m) | ((*c - done) & m);
        if ((*c & m) != 0) {
            return 0;
        }
    }
    kvm_emul_skip(v, len);
    return 1;
}

/** End the instruction under way: the stage idle. */
static void emul_end(struct kvm_vcpu *v)
{
    v->emul.stage = EMUL_IDLE;
    v->emul.resume = 0;
}

/** The internal error of an MMIO access the kernel cannot complete, with
 * the instruction's bytes, as Linux's prepare_emulation_failure_exit. */
static void emul_fail(struct kvm_vcpu *v, const uint8_t *bytes, uint32_t n)
{
    v->exit.kind = KVM_X_INTERNAL;
    kvm_decode_fail(&v->exit.internal, bytes, n, v->emul.info);
    emul_end(v);
}

/**
 * Finish the instruction under way with `value`, the memory operand's
 * bytes the MMIO read gave or the read of guest memory: write the register
 * or the flags, or set up the write that follows. 0, or -1 when the access
 * to guest memory faulted, with the exception queued, or 1 when it was not
 * RAM, an internal error in `v->exit`.
 */
static int emul_finish(struct kvm_vcpu *v, uint64_t value)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_insn *in = &em->insn;
    uint64_t x, m, sp;
    uint32_t size = in->size;
    int r, high8 = (in->flags & KVM_INSN_HIGH8) != 0;
    uint8_t buf[8];

    value &= size_mask(size);
    switch (in->op) {
    case KVM_OP_LOAD:
        x = value;
        if ((in->flags & KVM_INSN_SX) && (value & (1ull << (size * 8u - 1u)))) {
            x |= ~size_mask(size);
        }
        reg_write(v, in->reg, in->reg_size, high8, x);
        kvm_emul_skip(v, in->len);
        emul_end(v);
        return 0;
    case KVM_OP_ALU:
        x = alu_run(in->alu, value, source(v, in), size, &v->regs.rflags);
        if (in->alu == KVM_ALU_CMP || in->alu == KVM_ALU_TEST) {
            kvm_emul_skip(v, in->len);
            emul_end(v);
        } else if (in->flags & KVM_INSN_TO_REG) {
            reg_write(v, in->reg, size, high8, x);
            kvm_emul_skip(v, in->len);
            emul_end(v);
        } else {
            em->value = x;
            em->stage = EMUL_WRITE;
            em->resume = 1;
        }
        return 0;
    case KVM_OP_UNARY:
        em->value = alu_run(in->alu, value, 0, size, &v->regs.rflags);
        em->stage = EMUL_WRITE;
        em->resume = 1;
        return 0;
    case KVM_OP_XCHG:
        em->value = reg_read(v, in->reg, size, high8);
        reg_write(v, in->reg, size, high8, value);
        em->stage = EMUL_WRITE;
        em->resume = 1;
        return 0;
    case KVM_OP_MOVS:
        em->value = value;
        if (em->dst_mmio) {
            em->stage = EMUL_WRITE;
            em->resume = 1;
            return 0;
        }
        value_bytes(buf, value, size);
        r = gpa_rw(v, em->dst_gpa, buf, size, 1);
        if (r != 0) {
            emul_end(v);
            return r < 0 ? -1 : 1;
        }
        string_step(v, 6, size, in->addr_size, 1);
        string_step(v, 7, size, in->addr_size, 1);
        (void)string_done(v, (in->flags & (KVM_INSN_REP | KVM_INSN_REPNE)) != 0, in->addr_size, 1,
                          in->len);
        emul_end(v);
        return 0;
    case KVM_OP_PUSH:
        /* PUSH of memory: the MMIO operand read, then the stack written. */
        m = stack_mask(v);
        sp = (v->regs.rsp - size) & m;
        value_bytes(buf, value, size);
        r = kvm_guest_rw(v, KVM_SEG_SS, sp, buf, size, 1);
        if (r != 0) {
            emul_end(v);
            return r < 0 ? -1 : 1;
        }
        v->regs.rsp = (v->regs.rsp & ~m) | sp;
        kvm_emul_skip(v, in->len);
        emul_end(v);
        return 0;
    default:
        break;
    }
    emul_end(v);
    return 1;
}

/**
 * The next step of the instruction under way, after its read was answered
 * or at its start: the write it makes, into `v->exit`, with the instruction
 * finished, or the end of a MOVS whose destination is RAM.
 */
static int emul_step(struct kvm_vcpu *v)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_insn *in = &em->insn;
    uint32_t size = in->size;
    uint64_t m;

    em->resume = 0;
    if (em->stage != EMUL_WRITE) {
        v->exit.kind = KVM_X_AGAIN;
        return 0;
    }
    mmio_exit(v, in->op == KVM_OP_MOVS ? em->dst_gpa : em->gpa, size, 1, em->value);
    switch (in->op) {
    case KVM_OP_MOVS:
        string_step(v, 6, size, in->addr_size, 1);
        string_step(v, 7, size, in->addr_size, 1);
        (void)string_done(v, (in->flags & (KVM_INSN_REP | KVM_INSN_REPNE)) != 0, in->addr_size, 1,
                          in->len);
        break;
    case KVM_OP_POP:
        m = stack_mask(v);
        v->regs.rsp = (v->regs.rsp & ~m) | ((v->regs.rsp + size) & m);
        kvm_emul_skip(v, in->len);
        break;
    default:
        kvm_emul_skip(v, in->len);
        break;
    }
    emul_end(v);
    return 0;
}

/** Begin POP to memory, whose instruction `bytes` of `n` bytes `v` runs:
 * the stack read, then the MMIO operand written. 0 or -EFAULT. */
static int mmio_pop(struct kvm_vcpu *v, const uint8_t *bytes, uint32_t n)
{
    struct kvm_emul *em = &v->emul;
    uint8_t buf[8];
    int r;

    r = kvm_guest_rw(v, KVM_SEG_SS, v->regs.rsp & stack_mask(v), buf, em->insn.size, 0);
    if (r != 0) {
        if (r > 0) {
            emul_fail(v, bytes, n);
        }
        return r < 0 && r != -1 ? r : 0;
    }
    em->value = bytes_value(buf, em->insn.size);
    em->stage = EMUL_WRITE;
    return emul_step(v);
}

/**
 * Begin MOVS, whose instruction `bytes` of `n` bytes `v` runs, with either
 * operand outside RAM: the source read from MMIO, or from RAM and written
 * to MMIO at the next step. Both in RAM means the exit was a slot's fault
 * that is gone, and the instruction runs again. 0 or -EFAULT.
 */
static int mmio_movs(struct kvm_vcpu *v, const uint8_t *bytes, uint32_t n)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_insn *in = &em->insn;
    struct kvm_slot_view slot;
    uint64_t src_gpa = 0, dst_gpa = 0, m = size_mask(in->addr_size);
    uint8_t buf[8];
    int r;

    r = kvm_guest_translate(v, linear(v, in->seg, v->regs.rsi & m), access_bits(v, 0, 0), &src_gpa);
    if (r == 0) {
        r = kvm_guest_translate(v, linear(v, KVM_SEG_ES, v->regs.rdi & m), access_bits(v, 1, 0),
                                &dst_gpa);
    }
    if (r == 1) {
        emul_fail(v, bytes, n);
    }
    if (r != 0) {
        return r < 0 && r != -1 ? r : 0;
    }
    r = gpa_rw(v, src_gpa, buf, in->size, 0);
    if (r < 0) {
        return r;
    }
    em->src_mmio = r == 1;
    em->dst_mmio =
        !kvm_slot_find(v->vm, dst_gpa >> 12, &slot) || (slot.flags & KVM_MEM_READONLY) != 0;
    em->src_gpa = src_gpa;
    em->dst_gpa = dst_gpa;
    if (!em->src_mmio && !em->dst_mmio) {
        emul_end(v);
        return 0;
    }
    if (em->src_mmio) {
        em->stage = EMUL_READ;
        mmio_exit(v, src_gpa, in->size, 0, 0);
        return 0;
    }
    if (emul_finish(v, bytes_value(buf, in->size)) > 0) {
        emul_fail(v, bytes, n);
    }
    return em->resume ? emul_step(v) : 0;
}

int kvm_emul_mmio(struct kvm_vcpu *v, uint64_t gpa, const uint64_t info[5])
{
    struct kvm_emul *em = &v->emul;
    struct kvm_insn *in = &em->insn;
    uint8_t bytes[KVM_INSN_MAX];
    int n;

    k_memcpy(em->info, info, sizeof(em->info));
    v->exit.kind = KVM_X_AGAIN;
    n = fetch(v, bytes);
    if (n < 0) {
        return 0;
    }
    if (kvm_decode(bytes, (uint32_t)n, kvm_emul_mode(v), in) != 0) {
        /* Linux's handle_emulation_failure: #UD above CPL 0, the internal
         * error at CPL 0. */
        if (kvm_emul_cpl(v) > 0) {
            kvm_queue_exception(v, UD_VECTOR, 0, 0);
            return 0;
        }
        emul_fail(v, bytes, (uint32_t)n);
        return 0;
    }
    em->gpa = gpa;
    em->stage = EMUL_IDLE;
    em->resume = 0;
    em->fail = 0;
    switch (in->op) {
    case KVM_OP_LOAD:
    case KVM_OP_ALU:
    case KVM_OP_UNARY:
    case KVM_OP_XCHG:
    case KVM_OP_PUSH:
        em->stage = EMUL_READ;
        mmio_exit(v, gpa, in->size, 0, 0);
        return 0;
    case KVM_OP_STORE:
        em->value = source(v, in);
        em->stage = EMUL_WRITE;
        return emul_step(v);
    case KVM_OP_STOS:
        mmio_exit(v, gpa, in->size, 1, reg_read(v, 0, in->size, 0));
        string_step(v, 7, in->size, in->addr_size, 1);
        (void)string_done(v, (in->flags & (KVM_INSN_REP | KVM_INSN_REPNE)) != 0, in->addr_size, 1,
                          in->len);
        return 0;
    case KVM_OP_POP:
        return mmio_pop(v, bytes, (uint32_t)n);
    case KVM_OP_MOVS:
        return mmio_movs(v, bytes, (uint32_t)n);
    default:
        break;
    }
    emul_fail(v, bytes, (uint32_t)n);
    return 0;
}

/* ---- port I/O ---- */

int kvm_emul_pio(struct kvm_vcpu *v, const struct kvm_pio *p)
{
    struct kvm_emul *em = &v->emul;
    struct kvm_exit *ex = &v->exit;
    uint64_t m = size_mask(p->addr_size), count = 1, la, room;
    int r;

    ex->kind = KVM_X_IO;
    k_memset(&ex->io, 0, sizeof(ex->io));
    ex->io.direction = (uint8_t)(p->in ? KVM_EXIT_IO_IN : KVM_EXIT_IO_OUT);
    ex->io.size = (uint8_t)p->size;
    ex->io.port = p->port;
    em->pio = *p;
    if (p->string) {
        if (p->rep) {
            count = v->regs.rcx & m;
            if (count == 0) {
                kvm_emul_skip(v, p->len);
                ex->kind = KVM_X_AGAIN;
                return 0;
            }
        }
        /* At most the rest of the page of the first element, and of the
         * port I/O page, one element when DF counts down. */
        la = p->in ? linear(v, KVM_SEG_ES, v->regs.rdi & m) : linear(v, p->seg, v->regs.rsi & m);
        room = (4096u - (la & 0xFFFu)) / p->size;
        if (room == 0 || (v->regs.rflags & RFLAGS_DF)) {
            room = 1;
        }
        if (count > room) {
            count = room;
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
        kvm_emul_skip(v, p->len);
        return 0;
    }
    r = kvm_guest_rw(v, p->seg, v->regs.rsi & m, ex->data, (uint32_t)count * p->size, 0);
    if (r != 0) {
        ex->kind = KVM_X_AGAIN;
        if (r == 1) {
            /* A string OUT from MMIO, which Linux's emulator does too;
             * here the internal error. */
            ex->kind = KVM_X_INTERNAL;
            kvm_decode_fail(&ex->internal, NULL, 0, NULL);
        }
        return r < 0 && r != -1 ? r : 0;
    }
    string_step(v, 6, p->size, p->addr_size, (uint32_t)count);
    (void)string_done(v, p->rep, p->addr_size, (uint32_t)count, p->len);
    return 0;
}

/** Finish a port read with the bytes the caller or a device answered in
 * `v->exit.data`: the accumulator, or the string at ES:rDI. */
static void pio_complete(struct kvm_vcpu *v)
{
    struct kvm_pio *p = &v->emul.pio;
    uint64_t x;
    int r;

    emul_end(v);
    if (!p->string) {
        x = p->size < 4u ? v->regs.rax : 0;
        x = (x & ~size_mask(p->size)) | bytes_value(v->exit.data, p->size);
        v->regs.rax = x;
        kvm_emul_skip(v, p->len);
        return;
    }
    r = kvm_guest_rw(v, KVM_SEG_ES, v->regs.rdi & size_mask(p->addr_size), v->exit.data,
                     p->count * p->size, 1);
    if (r != 0) {
        return;
    }
    string_step(v, 7, p->size, p->addr_size, p->count);
    (void)string_done(v, p->rep, p->addr_size, p->count, p->len);
}

int kvm_emul_complete(struct kvm_vcpu *v)
{
    struct kvm_emul *em = &v->emul;
    int r;

    if (em->stage == EMUL_PIO && v->exit.kind == KVM_X_IO) {
        pio_complete(v);
        return 0;
    }
    if (em->stage != EMUL_READ || v->exit.kind != KVM_X_MMIO) {
        return 0;
    }
    r = emul_finish(v, bytes_value(v->exit.mmio.data, em->insn.size));
    if (r > 0) {
        /* The other operand of MOVS or PUSH is not RAM: report it at the
         * next step. */
        em->fail = 1;
        em->resume = 1;
    }
    return 0;
}

int kvm_emul_resume(struct kvm_vcpu *v)
{
    if (v->emul.fail) {
        v->emul.fail = 0;
        emul_fail(v, v->emul.insn.bytes, v->emul.insn.len);
        return 0;
    }
    return emul_step(v);
}
