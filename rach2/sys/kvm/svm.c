/* AMD SVM: VMCBs, host state, nested paging, intercepts and event injection. */

#include "kvm.h"

#include "../arch/arch.h"
#include "../arch/x86/debugreg.h"
#include "../arch/x86/kcpu.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../fpu.h"
#include "../klog.h"
#include "../proc/smp.h"

#ifndef RACH2_HOSTTEST
#include "../arch/x86_64/percpu.h"
#endif

/* AMD64 APM Vol. 2, chapter 15 and appendix B: processor constants. */

#define MSR_IA32_SYSENTER_CS  0x174u
#define MSR_IA32_SYSENTER_ESP 0x175u
#define MSR_IA32_SYSENTER_EIP 0x176u
#define MSR_IA32_DEBUGCTL     0x1D9u
#define MSR_IA32_CR_PAT       0x277u
#define MSR_APIC_BASE         0x01Bu
#define MSR_EFER              0xC0000080u
#define MSR_STAR              0xC0000081u
#define MSR_LSTAR             0xC0000082u
#define MSR_CSTAR             0xC0000083u
#define MSR_SYSCALL_MASK      0xC0000084u
#define MSR_FS_BASE           0xC0000100u
#define MSR_KERNEL_GS_BASE    0xC0000102u
#define MSR_TSC_AUX           0xC0000103u
#define MSR_TSC_RATIO         0xC0000104u
#define MSR_VM_CR             0xC0010114u
#define MSR_VM_HSAVE_PA       0xC0010117u

/* VM_CR's SVMDIS, which the firmware sets to turn SVM off. */
#define VM_CR_SVMDIS 0x10u

/* Leaf 0x8000000A's EDX: nested paging, the next RIP saved at an exit,
 * the TSC ratio MSR, and the flush of one ASID's translations. */
#define SVM_FEAT_NP          (1u << 0)
#define SVM_FEAT_NRIPS       (1u << 3)
#define SVM_FEAT_TSCRATE     (1u << 4)
#define SVM_FEAT_FLUSHBYASID (1u << 6)

/* The TSC ratio MSR, which scales the TSC a guest reads and no other: 8
 * bits of integer over 32 of fraction, so 1 and the largest are these. */
#define TSC_RATIO_BITS 32u
#define TSC_RATIO_ONE  (1ull << TSC_RATIO_BITS)
#define TSC_RATIO_MAX  0xFFFFFFFFFFull

/* A segment as the VMCB keeps it: the attributes are the descriptor's
 * bits 8 to 15 and 20 to 23, packed into 12 bits. */
struct vmcb_seg {
    uint16_t selector;
    uint16_t attrib;
    uint32_t limit;
    uint64_t base;
};

/* The VMCB's control area, its first 1024 bytes. */
struct vmcb_control {
    uint32_t intercept_cr;  /* bit n: read of CRn; bit 16 + n: write */
    uint32_t intercept_dr;  /* bit n: read of DRn; bit 16 + n: write */
    uint32_t intercept_exc; /* bit n: exception n */
    uint32_t intercept3;    /* INTERCEPT3_* */
    uint32_t intercept4;    /* INTERCEPT4_* */
    uint32_t intercept5;
    uint8_t res_018[36];
    uint16_t pause_thresh;
    uint16_t pause_count;
    uint64_t iopm_pa;
    uint64_t msrpm_pa;
    uint64_t tsc_offset;
    uint32_t asid;
    uint8_t tlb_ctl; /* TLB_* */
    uint8_t res_05d[3];
    uint32_t int_ctl; /* V_TPR and the virtual interrupt, INT_CTL_* */
    uint32_t int_vector;
    uint32_t int_state; /* bit 0: the interrupt shadow */
    uint8_t res_06c[4];
    uint64_t exit_code;
    uint64_t exit_info_1;
    uint64_t exit_info_2;
    uint32_t exit_int_info; /* the event under delivery at the exit, EV_* */
    uint32_t exit_int_info_err;
    uint64_t nested_ctl;
    uint64_t avic_bar;
    uint64_t ghcb;
    uint32_t event_inj; /* the event the entry injects, EV_* */
    uint32_t event_inj_err;
    uint64_t nested_cr3;
    uint64_t virt_ext;
    uint32_t clean; /* the parts the processor may keep from the last run */
    uint32_t res_0c4;
    uint64_t next_rip;
    uint8_t insn_len;
    uint8_t insn_bytes[15];
    uint8_t res_0e0[800];
};

/* The VMCB's state save area, from offset 1024. */
struct vmcb_save {
    struct vmcb_seg es, cs, ss, ds, fs, gs, gdtr, ldtr, idtr, tr;
    uint8_t res_0a0[43];
    uint8_t cpl;
    uint32_t res_0cc;
    uint64_t efer;
    uint8_t res_0d8[112];
    uint64_t cr4, cr3, cr0, dr7, dr6, rflags, rip;
    uint8_t res_180[88];
    uint64_t rsp;
    uint64_t s_cet, ssp, isst;
    uint64_t rax;
    uint64_t star, lstar, cstar, sfmask, kernel_gs_base;
    uint64_t sysenter_cs, sysenter_esp, sysenter_eip;
    uint64_t cr2;
    uint8_t res_248[32];
    uint64_t g_pat;
    uint64_t dbgctl;
    uint8_t res_278[2440];
};

struct vmcb {
    struct vmcb_control c;
    struct vmcb_save s;
};

/* The layout is the processor's: a VMCB is one page, its save area at
 * 1024. */
typedef char vmcb_control_size_check[sizeof(struct vmcb_control) == 1024u ? 1 : -1];
typedef char vmcb_size_check[sizeof(struct vmcb) == 4096u ? 1 : -1];

/* The intercepts of the third word. */
#define INTERCEPT3_INTR     (1u << 0)
#define INTERCEPT3_NMI      (1u << 1)
#define INTERCEPT3_SMI      (1u << 2)
#define INTERCEPT3_VINTR    (1u << 4)
#define INTERCEPT3_RDPMC    (1u << 15)
#define INTERCEPT3_CPUID    (1u << 18)
#define INTERCEPT3_RSM      (1u << 19)
#define INTERCEPT3_IRET     (1u << 20)
#define INTERCEPT3_INVD     (1u << 22)
#define INTERCEPT3_HLT      (1u << 24)
#define INTERCEPT3_INVLPGA  (1u << 26)
#define INTERCEPT3_IOIO     (1u << 27)
#define INTERCEPT3_MSR      (1u << 28)
#define INTERCEPT3_TASK     (1u << 29)
#define INTERCEPT3_SHUTDOWN (1u << 31)

/* The intercepts of the fourth word. */
#define INTERCEPT4_VMRUN   (1u << 0)
#define INTERCEPT4_VMMCALL (1u << 1)
#define INTERCEPT4_VMLOAD  (1u << 2)
#define INTERCEPT4_VMSAVE  (1u << 3)
#define INTERCEPT4_STGI    (1u << 4)
#define INTERCEPT4_CLGI    (1u << 5)
#define INTERCEPT4_SKINIT  (1u << 6)
#define INTERCEPT4_RDTSCP  (1u << 7)
#define INTERCEPT4_WBINVD  (1u << 9)
#define INTERCEPT4_MONITOR (1u << 10)
#define INTERCEPT4_MWAIT   (1u << 11)
#define INTERCEPT4_XSETBV  (1u << 13)
#define INTERCEPT4_RDPRU   (1u << 14)

/* int_ctl: the virtual TPR, the virtual interrupt with its priority and
 * TPR ignored, and V_INTR_MASKING, which leaves the host's RFLAGS.IF in
 * charge of physical interrupts and the guest's of virtual ones. */
#define INT_CTL_V_TPR          0xFFu
#define INT_CTL_V_IRQ          (1u << 8)
#define INT_CTL_V_PRIO         (0xFu << 16)
#define INT_CTL_V_IGN_TPR      (1u << 20)
#define INT_CTL_V_INTR_MASKING (1u << 24)

/* int_state: the guest is in an STI or MOV SS shadow. */
#define INT_STATE_SHADOW 0x1u

/* nested_ctl: nested paging on. */
#define NESTED_CTL_NP 0x1u

/* tlb_ctl: nothing, every ASID's translations, or the guest's ASID's. */
#define TLB_NONE      0u
#define TLB_FLUSH_ALL 1u
#define TLB_FLUSH_ONE 3u

/* An event, as EVENTINJ and EXITINTINFO give it: vector, type, error
 * code valid, valid. */
#define EV_VECTOR(e) ((e) & 0xFFu)
#define EV_TYPE(e)   (((e) >> 8) & 7u)
#define EV_ERR       0x800u
#define EV_VALID     0x80000000u
#define EV_INTR      0u
#define EV_NMI       2u
#define EV_EXCEPTION 3u
#define EV_SOFT      4u

/* The exit codes. */
#define EXIT_READ_CR0    0x000u
#define EXIT_WRITE_CR0   0x010u
#define EXIT_READ_DR0    0x020u
#define EXIT_WRITE_DR0   0x030u
#define EXIT_EXCP_BASE   0x040u
#define EXIT_INTR        0x060u
#define EXIT_NMI         0x061u
#define EXIT_SMI         0x062u
#define EXIT_INIT        0x063u
#define EXIT_VINTR       0x064u
#define EXIT_RDPMC       0x06Fu
#define EXIT_CPUID       0x072u
#define EXIT_RSM         0x073u
#define EXIT_IRET        0x074u
#define EXIT_INVD        0x076u
#define EXIT_PAUSE       0x077u
#define EXIT_HLT         0x078u
#define EXIT_INVLPGA     0x07Au
#define EXIT_IOIO        0x07Bu
#define EXIT_MSR         0x07Cu
#define EXIT_TASK_SWITCH 0x07Du
#define EXIT_SHUTDOWN    0x07Fu
#define EXIT_VMRUN       0x080u
#define EXIT_VMMCALL     0x081u
#define EXIT_VMLOAD      0x082u
#define EXIT_VMSAVE      0x083u
#define EXIT_STGI        0x084u
#define EXIT_CLGI        0x085u
#define EXIT_SKINIT      0x086u
#define EXIT_RDTSCP      0x087u
#define EXIT_WBINVD      0x089u
#define EXIT_MONITOR     0x08Au
#define EXIT_MWAIT       0x08Bu
#define EXIT_MWAIT_COND  0x08Cu
#define EXIT_XSETBV      0x08Du
#define EXIT_RDPRU       0x08Eu
#define EXIT_NPF         0x400u
#define EXIT_INVALID     (~0ull)

/* EXITINFO2 of a task switch: the source was IRET or JMP, and the error
 * code in its low half is valid. */
#define TS_IRET     (1ull << 36)
#define TS_JMP      (1ull << 38)
#define TS_HAS_CODE (1ull << 44)

/* EXITINFO1 of an I/O intercept. */
#define IOIO_IN   0x01u
#define IOIO_STR  0x04u
#define IOIO_REP  0x08u
#define IOIO_SZ8  0x10u
#define IOIO_SZ16 0x20u
#define IOIO_A16  0x80u
#define IOIO_A32  0x100u

#define CR0_PE 0x00000001ull
#define CR0_MP 0x00000002ull
#define CR0_TS 0x00000008ull
#define CR0_ET 0x00000010ull
#define CR0_WP 0x00010000ull
#define CR0_NW 0x20000000ull
#define CR0_CD 0x40000000ull
#define CR0_PG 0x80000000ull

#define CR4_DE    (1ull << 3)
#define CR4_PSE   (1ull << 4)
#define CR4_PAE   (1ull << 5)
#define CR4_MCE   (1ull << 6)
#define CR4_PGE   (1ull << 7)
#define CR4_PCIDE (1ull << 17)
#define CR4_SMEP  (1ull << 20)

#define GEFER_SCE   0x001ull
#define GEFER_LME   0x100ull
#define GEFER_LMA   0x400ull
#define GEFER_NXE   0x800ull
#define GEFER_SVME  0x1000ull
#define GEFER_FFXSR 0x4000ull

#define RFLAGS_TF 0x100ull
#define RFLAGS_IF 0x200ull
#define RFLAGS_RF 0x10000ull

#define DR6_BD         0x2000ull
#define DR6_BS         0x4000ull
#define DR6_FIXED      0xFFFF0FF0ull
#define DR6_ACTIVE_LOW 0xFFFF0FF0ull
#define DR7_GD         0x2000ull
#define DR7_FIXED      0x400ull
#define DR7_ENABLES    0xFFull

#define DEBUGCTL_LBR 0x1ull
#define DEBUGCTL_BTF 0x2ull

#define DB_VECTOR  1
#define NMI_VECTOR 2
#define BP_VECTOR  3
#define OF_VECTOR  4
#define UD_VECTOR  6
#define DF_VECTOR  8
#define TS_VECTOR  10
#define NP_VECTOR  11
#define SS_VECTOR  12
#define GP_VECTOR  13
#define PF_VECTOR  14
#define AC_VECTOR  17
#define MC_VECTOR  18
#define CP_VECTOR  21

/* KVM's interrupt shadow, struct kvm_vcpu_events' `interrupt.shadow`. */
#define SHADOW_MOV_SS 0x1u
#define SHADOW_STI    0x2u

/* The guest's entry, svm_run.s. */
int kvm_svm_vmrun(struct kvm_regs *regs, uint64_t vmcb_pa, uint64_t host_pa);

/* panic.h's hook, declared alone: its console headers name a global `x`. */
extern void (*k_panic_hook)(void);

/* ---- the processor's state ---- */

/* What the probe found, the same on every processor. */
static struct {
    uint32_t max_asid; /* the highest ASID, one less than leaf 0x8000000A's count */
    int nrips;         /* the exit saves the next RIP */
    int flushbyasid;   /* TLB_FLUSH_ONE is offered */
    int tscrate;       /* the TSC ratio MSR is there */
    int rdtscp;        /* the host has RDTSCP, and so TSC_AUX */
    uint64_t host_mce; /* the host's CR4.MCE, which the guest's CR4 keeps */
} caps;

/* Each processor's host save area, which VM_HSAVE_PA names, made at the
 * first VM; whether EFER.SVME and VM_HSAVE_PA are set there; whether a
 * run is inside its section, for the panic hook; its ASIDs, handed out
 * from `asid_next` in the generation `asid_gen`; its TSC_AUX; and the TSC
 * ratio its MSR holds. */
static void *hsave[NR_CPUS];
static uint64_t hsave_pa[NR_CPUS];
static volatile int svm_live[NR_CPUS];
static volatile int svm_in_run[NR_CPUS];
static uint32_t asid_next[NR_CPUS];
static uint32_t asid_gen[NR_CPUS];
static uint64_t host_aux[NR_CPUS];
static uint64_t tsc_ratio[NR_CPUS];

/* The I/O permission map, every port intercepted, which every vCPU
 * shares; each has its own MSR permission map (`svm_msr_filter_changed`). */
static uint8_t *iopm;
static uintptr_t iopm_pa;

/* ---- a vCPU's share ---- */

/* One exit, read from the VMCB after the run. */
struct svm_exit {
    uint64_t code;
    uint64_t info1, info2;
    uint32_t int_info, int_err;
    uint64_t next_rip;
};

struct svm_vcpu {
    struct vmcb *vmcb;
    uint64_t vmcb_pa;
    void *host; /* the page VMSAVE keeps the host's FS, GS, TR and MSRs in */
    uint64_t host_pa;
    void *host_fpu;
    uint8_t *msrpm; /* every MSR intercepted but those `svm_msr_filter_changed` passes */
    uintptr_t msrpm_pa;
    int built;         /* the VMCB's lasting fields are written */
    int cpu;           /* the processor whose ASID it holds, or -1 */
    uint32_t asid_gen; /* the generation of its ASID there */
    int flush;         /* drop the guest's translations at the entry */
    int kgs_table;     /* msr.c's KERNEL_GS_BASE is the guest's, after `save` */
    uint64_t tsc_aux;  /* the guest's TSC_AUX, from msr.c */
    uint64_t debugctl;
    uint32_t inj, inj_err; /* the event the next entry injects */
    uint64_t inj_next_rip; /* NextRIP for a software event under NRIPS */
    int soft_rewind;       /* RIP was stepped past a software event without NRIPS */
    uint64_t soft_old_rip, soft_next_rip;
    int want_vintr;    /* the interrupt window is armed */
    int awaiting_iret; /* an NMI handler's IRET was intercepted at `iret_rip` */
    uint64_t iret_rip;
    int nmi_ss;             /* single-stepping for the NMI window */
    uint64_t nmi_ss_rflags; /* the guest's RFLAGS before that */
    int tf_set;             /* the entry set RFLAGS.TF for KVM_GUESTDBG_SINGLESTEP */
    int guest_dr;           /* DR0 to DR3 were loaded for the run */
    struct svm_exit ex;
};

/** The backend's share of `v`. */
static struct svm_vcpu *sv(const struct kvm_vcpu *v)
{
    return (struct svm_vcpu *)v->backend;
}

/** Whether the caller debugs `v` with the guest-debug control `flag`
 * (KVM_GUESTDBG_*), which needs KVM_GUESTDBG_ENABLE too. */
static int debugging(const struct kvm_vcpu *v, uint32_t flag)
{
    return (v->guest_debug.control & KVM_GUESTDBG_ENABLE) && (v->guest_debug.control & flag);
}

/** The DR7 the guest runs under: the caller's while it sets hardware
 * breakpoints, else the guest's own. */
static uint64_t dr7_of(const struct kvm_vcpu *v)
{
    return debugging(v, KVM_GUESTDBG_USE_HW_BP) ? v->guest_debug.debugreg[7] : v->debugregs.dr7;
}

/** CR8 of `v`: the in-kernel APIC's TPR, or the record's without one. */
static uint64_t get_cr8(struct kvm_vcpu *v)
{
    return v->vm->irqchip != KVM_IRQCHIP_NONE ? kvm_lapic_cr8(v) : v->sregs.cr8;
}

/* ---- the processor's instructions ---- */

#ifndef RACH2_HOSTTEST

/** Debug register `n`, 0 to 3 or 7: `mov %drN, %rax` is 0F 21 /r. */
static uint64_t hw_read_dr(int n)
{
    uint64_t v = 0;

    switch (n) {
    case 0:
        __asm__ __volatile__(".byte 0x0f, 0x21, 0xc0" : "=a"(v));
        break;
    case 1:
        __asm__ __volatile__(".byte 0x0f, 0x21, 0xc8" : "=a"(v));
        break;
    case 2:
        __asm__ __volatile__(".byte 0x0f, 0x21, 0xd0" : "=a"(v));
        break;
    case 3:
        __asm__ __volatile__(".byte 0x0f, 0x21, 0xd8" : "=a"(v));
        break;
    default:
        __asm__ __volatile__(".byte 0x0f, 0x21, 0xf8" : "=a"(v));
        break;
    }
    return v;
}

/** Write debug register `n`: `mov %rax, %drN` is 0F 23 /r. */
static void hw_write_dr(int n, uint64_t v)
{
    switch (n) {
    case 0:
        __asm__ __volatile__(".byte 0x0f, 0x23, 0xc0" : : "a"(v));
        break;
    case 1:
        __asm__ __volatile__(".byte 0x0f, 0x23, 0xc8" : : "a"(v));
        break;
    case 2:
        __asm__ __volatile__(".byte 0x0f, 0x23, 0xd0" : : "a"(v));
        break;
    case 3:
        __asm__ __volatile__(".byte 0x0f, 0x23, 0xd8" : : "a"(v));
        break;
    default:
        __asm__ __volatile__(".byte 0x0f, 0x23, 0xf8" : : "a"(v));
        break;
    }
}

/** CR4: `mov %cr4, %rax` as bytes, which TinyCC's assembler does not name. */
static uint64_t hw_read_cr4(void)
{
    uint64_t v;

    __asm__ __volatile__(".byte 0x0f, 0x20, 0xe0" : "=a"(v));
    return v;
}

/* `xsave64 (%rdi)` and `xrstor64 (%rdi)` as bytes, as fpu.c writes them. */
#define XSAVE64  ".byte 0x48, 0x0f, 0xae, 0x27"
#define XRSTOR64 ".byte 0x48, 0x0f, 0xae, 0x2f"

/** XCR0. */
static uint64_t hw_xgetbv(void)
{
    uint32_t lo, hi;

    __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0u));
    return ((uint64_t)hi << 32) | lo;
}

/** Write XCR0. */
static void hw_xsetbv(uint64_t v)
{
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xd1"
                         :
                         : "c"(0u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* The host's XCR0 while a guest's is loaded. */
static uint64_t host_xcr0[NR_CPUS];

/** Save the host's floating-point state into `host` and load the guest's
 * image `guest` before setting its XCR0, using XRSTOR or FXRSTOR. */
static void fpu_load(void *host, const void *guest, uint64_t xcr0)
{
    uint64_t mask;

    fpu_save(host);
    if (kvm_xcr0_supported()) {
        mask = hw_xgetbv();
        host_xcr0[arch_cpu_id()] = mask;
        /* XRSTOR checks the whole header against XCR0, including saved
         * components that the guest currently has disabled. */
        __asm__ __volatile__(XRSTOR64
                             :
                             : "D"(guest), "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32))
                             : "memory");
        if (xcr0 != mask) {
            hw_xsetbv(xcr0);
        }
    } else {
        __asm__ __volatile__("fxrstorq (%0)" : : "r"(guest) : "memory");
    }
}

/** Save the guest's state into its image and load the host's back. */
static void fpu_store(void *host, void *guest, uint64_t xcr0)
{
    uint64_t mask;

    if (kvm_xcr0_supported()) {
        mask = host_xcr0[arch_cpu_id()];
        if (xcr0 != mask) {
            hw_xsetbv(mask);
        }
        __asm__ __volatile__(XSAVE64
                             :
                             : "D"(guest), "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32))
                             : "memory");
    } else {
        __asm__ __volatile__("fxsaveq (%0)" : : "r"(guest) : "memory");
    }
    fpu_restore(host);
}

/** The host's machine-check handler, for a machine check the run took:
 * INT 18. */
static void hw_machine_check(void)
{
    __asm__ __volatile__(".byte 0xcd, 0x12");
}

/** WBINVD. */
static void hw_wbinvd(void)
{
    __asm__ __volatile__(".byte 0x0f, 0x09" : : : "memory");
}

/** STGI, for the panic hook. */
static void hw_stgi(void)
{
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xdc");
}
#endif

/* ---- segments and control registers ---- */

/** The VMCB's attributes of the segment `s`: none for a segment not
 * present, as Linux's svm_set_segment gives it. */
static uint16_t seg_attrib(const struct kvm_segment *s)
{
    if (s->unusable || !s->present) {
        return 0;
    }
    return (uint16_t)((s->type & 15u) | ((uint32_t)(s->s & 1u) << 4) |
                      ((uint32_t)(s->dpl & 3u) << 5) | (1u << 7) | ((uint32_t)(s->avl & 1u) << 8) |
                      ((uint32_t)(s->l & 1u) << 9) | ((uint32_t)(s->db & 1u) << 10) |
                      ((uint32_t)(s->g & 1u) << 11));
}

/** Write the segment `s` into the VMCB's `d`. */
static void seg_write(struct vmcb_seg *d, const struct kvm_segment *s)
{
    d->selector = s->selector;
    d->attrib = seg_attrib(s);
    d->limit = s->limit;
    d->base = s->base;
}

/** Read the VMCB's segment `d` into `s`, as Linux's svm_get_segment: a
 * segment is usable when present. */
static void seg_read(const struct vmcb_seg *d, struct kvm_segment *s)
{
    uint32_t a = d->attrib;

    s->selector = d->selector;
    s->base = d->base;
    s->limit = d->limit;
    s->type = (uint8_t)(a & 15u);
    s->s = (uint8_t)((a >> 4) & 1u);
    s->dpl = (uint8_t)((a >> 5) & 3u);
    s->present = (uint8_t)((a >> 7) & 1u);
    s->avl = (uint8_t)((a >> 8) & 1u);
    s->l = (uint8_t)((a >> 9) & 1u);
    s->db = (uint8_t)((a >> 10) & 1u);
    s->g = (uint8_t)((a >> 11) & 1u);
    s->unusable = (uint8_t)!s->present;
    s->padding = 0;
}

/** `v`'s CR0 as the processor runs the guest under it: without CD and NW
 * under KVM_X86_QUIRK_CD_NW_CLEARED, since nested paging gives the memory
 * types, and the guest's own once the quirk is turned off, as Linux's
 * svm_set_cr0 has it. */
static uint64_t hw_cr0(const struct kvm_vcpu *v)
{
    uint64_t cr0 = v->sregs.cr0;

    return KVM_QUIRK(v->vm, KVM_X86_QUIRK_CD_NW_CLEARED) ? cr0 & ~(CR0_CD | CR0_NW) : cr0;
}

/** The intercepts of the control registers: CR0's reads and writes while
 * the processor's CR0 is not the guest's, as Linux's svm_set_cr0 sets
 * them; CR4's, whose MCE the host keeps on; and CR8's writes, which go to
 * the APIC. CR3 and CR8's reads do not exit. */
static uint32_t cr_intercepts(const struct kvm_vcpu *v)
{
    uint32_t i = (1u << 4) | (1u << (16 + 4)) | (1u << (16 + 8));

    if (hw_cr0(v) != v->sregs.cr0) {
        i |= 1u | (1u << 16);
    }
    return i;
}

/** Whether `v` pages with PAE's four PDPTEs: PG and PAE, not long mode. */
static int pae_paging(const struct kvm_vcpu *v)
{
    return (v->sregs.cr0 & CR0_PG) && (v->sregs.cr4 & CR4_PAE) && !(v->sregs.efer & GEFER_LMA);
}

/** Whether `v`'s PDPTEs at CR3 load, as the processor checks them when a
 * write of CR0 or CR4 turns PAE paging on: 1 or 0. */
static int pdptes_ok(struct kvm_vcpu *v)
{
    uint64_t p[4];

    return kvm_guest_pdptes(v, v->sregs.cr3, p) == 0;
}

/* ---- the VMCB built and loaded ---- */

/** The value of msr.c's MSR `index` of `v`, 0 when it keeps none. */
static uint64_t table_msr(struct kvm_vcpu *v, uint32_t index)
{
    uint64_t value = 0;

    if (kvm_msr_get(v, index, &value, 1) != 0) {
        return 0;
    }
    return value;
}

/** Whether `v`'s CPUID offers RDTSCP; without it RDTSCP is #UD. */
static int guest_rdtscp(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 0x80000001u, 0);

    return e && ((e->edx >> 27) & 1u);
}

/** Write the VMCB fields that last for the vCPU's life: the intercepts,
 * the permission maps, nested paging and the interrupt masking. */
static void vmcb_build(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    struct vmcb_control *c = &s->vmcb->c;

    k_memset(s->vmcb, 0, sizeof(*s->vmcb));
    c->intercept_dr = 0x00FF00FFu;
    /* #DB and #AC always, so that a guest cannot hold the processor in an
     * endless loop of either, as Linux keeps them since CVE-2015-8104 and
     * CVE-2015-5307; #UD, for the instructions emulate.c runs after one;
     * #MC, which is the host's. */
    c->intercept_exc =
        (1u << DB_VECTOR) | (1u << UD_VECTOR) | (1u << AC_VECTOR) | (1u << MC_VECTOR);
    c->intercept3 = INTERCEPT3_INTR | INTERCEPT3_NMI | INTERCEPT3_SMI | INTERCEPT3_RDPMC |
                    INTERCEPT3_CPUID | INTERCEPT3_RSM | INTERCEPT3_INVD | INTERCEPT3_HLT |
                    INTERCEPT3_INVLPGA | INTERCEPT3_IOIO | INTERCEPT3_MSR | INTERCEPT3_TASK |
                    INTERCEPT3_SHUTDOWN;
    if (v->hlt_in_guest) {
        c->intercept3 &= ~INTERCEPT3_HLT;
    }
    c->intercept4 = INTERCEPT4_VMRUN | INTERCEPT4_VMMCALL | INTERCEPT4_VMLOAD | INTERCEPT4_VMSAVE |
                    INTERCEPT4_STGI | INTERCEPT4_CLGI | INTERCEPT4_SKINIT | INTERCEPT4_WBINVD |
                    INTERCEPT4_MONITOR | INTERCEPT4_MWAIT | INTERCEPT4_XSETBV | INTERCEPT4_RDPRU;
    c->iopm_pa = iopm_pa;
    c->msrpm_pa = s->msrpm_pa;
    c->int_ctl = INT_CTL_V_INTR_MASKING;
    c->nested_ctl = NESTED_CTL_NP;
    s->built = 1;
    s->cpu = -1;
    s->want_vintr = 0;
    s->awaiting_iret = 0;
    s->nmi_ss = 0;
    v->dirty = KVM_PART_ALL;
}

/** Write the parts of `v`'s state the record changed since the last
 * entry, and clear their marks. The control registers, EFER, CR2, RSP,
 * RIP and RFLAGS are written at every entry (`entry_fields`). */
static void load_dirty(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    struct vmcb_control *c = &s->vmcb->c;
    struct vmcb_save *g = &s->vmcb->s;
    uint32_t d = v->dirty;

    if (d & KVM_PART_SREGS) {
        seg_write(&g->es, &v->sregs.es);
        seg_write(&g->cs, &v->sregs.cs);
        seg_write(&g->ss, &v->sregs.ss);
        seg_write(&g->ds, &v->sregs.ds);
        seg_write(&g->fs, &v->sregs.fs);
        seg_write(&g->gs, &v->sregs.gs);
        seg_write(&g->ldtr, &v->sregs.ldt);
        seg_write(&g->tr, &v->sregs.tr);
        g->gdtr.base = v->sregs.gdt.base;
        g->gdtr.limit = v->sregs.gdt.limit;
        g->idtr.base = v->sregs.idt.base;
        g->idtr.limit = v->sregs.idt.limit;
        /* The privilege level is SS's DPL, as Linux's svm_set_segment. */
        g->cpl = (uint8_t)(v->sregs.ss.dpl & 3u);
        s->flush = 1;
    }
    if (d & KVM_PART_EVENTS) {
        c->int_state = v->events.interrupt.shadow ? INT_STATE_SHADOW : 0u;
        /* NMI blocking is the kernel's: an intercepted IRET ends it, as
         * Linux's svm_set_nmi_mask arms it. */
        if (v->events.nmi.masked && !s->awaiting_iret) {
            c->intercept3 |= INTERCEPT3_IRET;
        } else {
            c->intercept3 &= ~INTERCEPT3_IRET;
        }
        if (!v->events.nmi.masked) {
            s->awaiting_iret = 0;
        }
    }
    if (d & KVM_PART_DEBUG) {
        c->intercept_exc &= ~(1u << BP_VECTOR);
        if (debugging(v, KVM_GUESTDBG_USE_SW_BP)) {
            c->intercept_exc |= 1u << BP_VECTOR;
        }
    }
    if (d & KVM_PART_MSRS) {
        g->star = table_msr(v, MSR_STAR);
        g->lstar = table_msr(v, MSR_LSTAR);
        g->cstar = table_msr(v, MSR_CSTAR);
        g->sfmask = table_msr(v, MSR_SYSCALL_MASK);
        if (s->kgs_table) {
            g->kernel_gs_base = table_msr(v, MSR_KERNEL_GS_BASE);
        }
        g->sysenter_cs = table_msr(v, MSR_IA32_SYSENTER_CS);
        g->sysenter_esp = table_msr(v, MSR_IA32_SYSENTER_ESP);
        g->sysenter_eip = table_msr(v, MSR_IA32_SYSENTER_EIP);
        g->g_pat = table_msr(v, MSR_IA32_CR_PAT);
        s->tsc_aux = table_msr(v, MSR_TSC_AUX);
    }
    if (d & KVM_PART_TSC) {
        c->tsc_offset = (uint64_t)v->tsc_offset;
    }
    if (d & KVM_PART_CPUID) {
        c->intercept4 &= ~INTERCEPT4_RDTSCP;
        if (!guest_rdtscp(v)) {
            c->intercept4 |= INTERCEPT4_RDTSCP;
        }
    }
    /* KVM_PART_REGS is the record's at every entry; KVM_PART_FPU the image
     * and XCR0, loaded at every entry; KVM_PART_CR8 the TPR, written to
     * V_TPR at every entry; KVM_PART_MP the run state, which the run loop
     * keeps. */
    v->dirty = 0;
}

/**
 * Give the vCPU an ASID of the processor `cpu` it enters on, as Linux's
 * pre_svm_run and new_asid: a new one after a move to another processor or
 * when the processor's ASIDs have been handed out anew, so that no
 * translation it made elsewhere or before can be used; with `flush`, its
 * translations dropped by TLB_FLUSH_ONE where offered, or by a new ASID.
 * When the ASIDs run out the generation turns and every ASID's
 * translations go. With interrupts off.
 */
static void asid_load(struct svm_vcpu *s, int cpu, int flush)
{
    struct vmcb_control *c = &s->vmcb->c;

    c->tlb_ctl = TLB_NONE;
    if (asid_gen[cpu] == 0) {
        asid_gen[cpu] = 1;
        asid_next[cpu] = 1;
        c->tlb_ctl = TLB_FLUSH_ALL;
    }
    if (s->cpu != cpu || s->asid_gen != asid_gen[cpu] || (flush && !caps.flushbyasid)) {
        if (asid_next[cpu] > caps.max_asid) {
            asid_gen[cpu]++;
            asid_next[cpu] = 1;
            c->tlb_ctl = TLB_FLUSH_ALL;
        }
        c->asid = asid_next[cpu]++;
        s->asid_gen = asid_gen[cpu];
        s->cpu = cpu;
    } else if (flush) {
        c->tlb_ctl = TLB_FLUSH_ONE;
    }
}

/** Write the state of the next entry: the event to inject, the interrupt
 * window, V_TPR, nested paging's root, the control registers, EFER, CR2,
 * RAX, RSP, RIP and RFLAGS, the debug registers the VMCB holds, and every
 * part marked dirty to the processor. */
static void entry_fields(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    struct vmcb_control *c = &s->vmcb->c;
    struct vmcb_save *g = &s->vmcb->s;
    uint64_t rflags = v->regs.rflags;

    if (v->vm->irqchip == KVM_IRQCHIP_NONE) {
        s->want_vintr = v->run->request_interrupt_window &&
                        (s->inj || !(v->regs.rflags & RFLAGS_IF) || v->events.interrupt.shadow);
    }
    c->event_inj = s->inj;
    c->event_inj_err = s->inj_err;
    if ((s->inj & EV_VALID) && EV_TYPE(s->inj) == EV_INTR) {
        v->irq_counts[EV_VECTOR(s->inj)][KVM_VEC_INJECT]++;
    }
    if (caps.nrips && (s->inj & EV_VALID) &&
        (EV_TYPE(s->inj) == EV_SOFT ||
         (EV_TYPE(s->inj) == EV_EXCEPTION &&
          (EV_VECTOR(s->inj) == BP_VECTOR || EV_VECTOR(s->inj) == OF_VECTOR)))) {
        c->next_rip = s->inj_next_rip;
    }
    /* The window is a virtual interrupt the guest is never given: its
     * intercept comes first, once the guest can take one, as Linux's
     * svm_enable_irq_window sets it. */
    c->int_ctl &= ~(INT_CTL_V_TPR | INT_CTL_V_IRQ | INT_CTL_V_PRIO | INT_CTL_V_IGN_TPR);
    c->intercept3 &= ~INTERCEPT3_VINTR;
    if (s->want_vintr) {
        c->int_ctl |= INT_CTL_V_IRQ | INT_CTL_V_PRIO | INT_CTL_V_IGN_TPR;
        c->int_vector = 0;
        c->intercept3 |= INTERCEPT3_VINTR;
    }
    c->int_ctl |= (uint32_t)(get_cr8(v) & 0xFu);
    c->intercept_cr = cr_intercepts(v);
    c->nested_cr3 = kvm_tdp_root(v->vm);
    c->clean = 0;
    g->cr0 = hw_cr0(v);
    g->cr3 = v->sregs.cr3;
    g->cr4 = v->sregs.cr4 | caps.host_mce;
    g->efer = v->sregs.efer | GEFER_SVME;
    g->cr2 = v->sregs.cr2;
    g->rax = v->regs.rax;
    g->rsp = v->regs.rsp;
    g->rip = v->regs.rip;
    /* The caller's single step is the guest's TF, hidden from the guest's
     * RFLAGS after the exit, as Linux's __kvm_set_rflags sets it; the NMI
     * window's step adds TF and RF, as Linux's svm_enable_nmi_window. */
    s->tf_set = debugging(v, KVM_GUESTDBG_SINGLESTEP) && !(rflags & RFLAGS_TF);
    if (s->tf_set) {
        rflags |= RFLAGS_TF;
    }
    if (s->nmi_ss) {
        rflags |= RFLAGS_TF | RFLAGS_RF;
    }
    g->rflags = rflags;
    /* DR6 starts every run with no bit of an event, so that a #DB's bits
     * are the run's, as Linux's svm_set_dr6 with DR6_ACTIVE_LOW. */
    g->dr6 = DR6_FIXED;
    g->dr7 = dr7_of(v);
}

/** Load DR0 to DR3 for the run when the DR7 it runs under enables a
 * breakpoint: the caller's breakpoints while it sets them, else the
 * guest's. The VMCB holds DR6 and DR7. */
static void dr_load(struct kvm_vcpu *v, uint64_t *host_dr7, uint64_t host_db[4])
{
#ifndef RACH2_HOSTTEST
    struct svm_vcpu *s = sv(v);
    int i, hw = debugging(v, KVM_GUESTDBG_USE_HW_BP);

    *host_dr7 = debugreg_loaded() ? hw_read_dr(7) : DR7_ONE;
    s->guest_dr = (dr7_of(v) & DR7_ENABLES) != 0;
    if (!s->guest_dr) {
        return;
    }
    for (i = 0; i < 4; i++) {
        host_db[i] = hw_read_dr(i);
        hw_write_dr(i, hw ? v->guest_debug.debugreg[i] : v->debugregs.db[i]);
    }
#else
    (void)v;
    (void)host_dr7;
    (void)host_db;
#endif
}

/** Put the host's debug registers back: DR0 to DR3 when the run loaded
 * the guest's, and DR7, which the exit leaves with no breakpoint, when the
 * host had one. */
static void dr_store(struct kvm_vcpu *v, uint64_t host_dr7, const uint64_t host_db[4])
{
#ifndef RACH2_HOSTTEST
    int i;

    if (sv(v)->guest_dr) {
        for (i = 0; i < 4; i++) {
            hw_write_dr(i, host_db[i]);
        }
    }
    if (host_dr7 & DR7_ENABLES) {
        hw_write_dr(7, host_dr7);
    }
#else
    (void)v;
    (void)host_dr7;
    (void)host_db;
#endif
}

/** Read what the run that just ended left into `v`'s record: the exit,
 * RAX, RSP, RIP and RFLAGS, the segments and tables, CR0 when the guest
 * writes it without an exit, CR2, CR3 and EFER, and the interrupt shadow.
 * CR4, DR6 and DR7 change only by an exit, which writes the record. */
static void after_run(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    struct vmcb_control *c = &s->vmcb->c;
    struct vmcb_save *g = &s->vmcb->s;
    uint64_t rflags = g->rflags;

    s->ex.code = c->exit_code;
    s->ex.info1 = c->exit_info_1;
    s->ex.info2 = c->exit_info_2;
    s->ex.int_info = c->exit_int_info;
    s->ex.int_err = c->exit_int_info_err;
    s->ex.next_rip = caps.nrips ? c->next_rip : 0;
    s->inj = 0;
    c->event_inj = 0;
    v->regs.rax = g->rax;
    v->regs.rsp = g->rsp;
    v->regs.rip = g->rip;
    if (s->tf_set) {
        rflags &= ~RFLAGS_TF;
    }
    if (s->nmi_ss) {
        if (!(s->nmi_ss_rflags & RFLAGS_TF)) {
            rflags &= ~RFLAGS_TF;
        }
        if (!(s->nmi_ss_rflags & RFLAGS_RF)) {
            rflags &= ~RFLAGS_RF;
        }
    }
    v->regs.rflags = rflags;
    seg_read(&g->es, &v->sregs.es);
    seg_read(&g->cs, &v->sregs.cs);
    seg_read(&g->ss, &v->sregs.ss);
    seg_read(&g->ds, &v->sregs.ds);
    seg_read(&g->fs, &v->sregs.fs);
    seg_read(&g->gs, &v->sregs.gs);
    seg_read(&g->ldtr, &v->sregs.ldt);
    seg_read(&g->tr, &v->sregs.tr);
    /* TR is busy and a usable data segment accessed in the processor's
     * cache whatever the descriptor says, an unusable SS has no B bit, and
     * SS's DPL is the CPL, as Linux's svm_get_segment reports them. */
    v->sregs.tr.type |= 2u;
    if (!v->sregs.ds.unusable) {
        v->sregs.ds.type |= 1u;
    }
    if (!v->sregs.es.unusable) {
        v->sregs.es.type |= 1u;
    }
    if (!v->sregs.fs.unusable) {
        v->sregs.fs.type |= 1u;
    }
    if (!v->sregs.gs.unusable) {
        v->sregs.gs.type |= 1u;
    }
    if (v->sregs.ss.unusable) {
        v->sregs.ss.db = 0;
    }
    v->sregs.ss.dpl = g->cpl;
    v->sregs.gdt.base = g->gdtr.base;
    v->sregs.gdt.limit = (uint16_t)g->gdtr.limit;
    v->sregs.idt.base = g->idtr.base;
    v->sregs.idt.limit = (uint16_t)g->idtr.limit;
    if (!(c->intercept_cr & (1u << 16))) {
        v->sregs.cr0 = g->cr0;
    }
    v->sregs.cr2 = g->cr2;
    v->sregs.cr3 = g->cr3;
    v->sregs.efer = g->efer & ~GEFER_SVME;
    v->events.interrupt.shadow =
        (uint8_t)((c->int_state & INT_STATE_SHADOW) ? (SHADOW_STI | SHADOW_MOV_SS) : 0u);
    s->kgs_table = 0;
}

/**
 * Run the guest until its next exit, with interrupts off and without the
 * big lock: EFER.SVME and the host save area set on this processor at its
 * first run, an ASID of the processor, the VMCB brought up to date, the
 * guest's FPU, debug registers and TSC_AUX in, VMRUN between VMSAVE and
 * VMLOAD of the host's state with GIF clear; at the exit the host's state
 * back and the exit read. The host's interrupt or NMI that ended the run
 * stays pending through GIF's clear and the restored RFLAGS.IF until the
 * run loop turns interrupts on, as Linux's svm_vcpu_run leaves them; a
 * machine check the run took reaches the host's handler. While
 * `v->emul.resume` is set no guest runs: the next step of the instruction
 * under way is `exit`'s. 0, or -EIO when SVM cannot be used here.
 */
static int svm_enter(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    uint64_t host_dr7 = 0, host_db[4] = {0, 0, 0, 0}, ratio;
    int cpu = (int)arch_cpu_id(), tlb;

    if (v->emul.resume) {
        return 0;
    }
    if (cpu >= NR_CPUS || !hsave[cpu]) {
        return -EIO;
    }
    tlb = kvm_vcpu_test_clear(v, KVM_REQ_TLB_FLUSH);
#ifndef RACH2_HOSTTEST
    if (!svm_live[cpu]) {
        msr_write(MSR_EFER, msr_read(MSR_EFER) | GEFER_SVME);
        msr_write(MSR_VM_HSAVE_PA, hsave_pa[cpu]);
        if (caps.rdtscp) {
            host_aux[cpu] = msr_read(MSR_TSC_AUX);
        }
        if (caps.tscrate) {
            msr_write(MSR_TSC_RATIO, TSC_RATIO_ONE);
            tsc_ratio[cpu] = TSC_RATIO_ONE;
        }
        svm_live[cpu] = 1;
    }
#endif
    if (!s->built) {
        vmcb_build(v);
    }
    load_dirty(v);
    asid_load(s, cpu, tlb || s->flush);
    s->flush = 0;
    entry_fields(v);
#ifndef RACH2_HOSTTEST
    fpu_load(s->host_fpu, v->xsave, v->xcr0);
    dr_load(v, &host_dr7, host_db);
    if (caps.rdtscp && s->tsc_aux != host_aux[cpu]) {
        msr_write(MSR_TSC_AUX, s->tsc_aux);
    }
    ratio = v->tsc_ratio ? v->tsc_ratio : TSC_RATIO_ONE;
    if (caps.tscrate && ratio != tsc_ratio[cpu]) {
        msr_write(MSR_TSC_RATIO, ratio);
        tsc_ratio[cpu] = ratio;
    }
    svm_in_run[cpu] = 1;
    (void)kvm_svm_vmrun(&v->regs, s->vmcb_pa, s->host_pa);
    svm_in_run[cpu] = 0;
    if (caps.rdtscp && s->tsc_aux != host_aux[cpu]) {
        msr_write(MSR_TSC_AUX, host_aux[cpu]);
    }
    dr_store(v, host_dr7, host_db);
    fpu_store(s->host_fpu, v->xsave, v->xcr0);
#endif
    after_run(v);
#ifndef RACH2_HOSTTEST
    if (s->ex.code == EXIT_EXCP_BASE + MC_VECTOR) {
        hw_machine_check();
    }
#endif
    return 0;
}

/* ---- the instructions the exits do not describe ---- */

/*
 * One instruction of the guest that an intercept named, read from its
 * bytes where the processor gives neither its length (NRIPS) nor its
 * operands (decode assists): the prefixes, the opcode, the ModRM byte with
 * REX's extensions, and, for a memory operand, its segment and offset.
 */
struct sys_insn {
    uint8_t bytes[KVM_INSN_MAX];
    uint32_t n;         /* the bytes fetched */
    uint32_t len;       /* the instruction's */
    uint32_t op;        /* a one-byte opcode, or 0x0F00 | the second byte */
    int lock;           /* a LOCK prefix */
    uint32_t opsize;    /* 2, 4 or 8 */
    uint32_t addr_size; /* 2, 4 or 8 */
    int seg;            /* the segment override, KVM_SEG_*, or -1 */
    int has_modrm;
    uint32_t mod, reg, rm; /* reg and rm with REX.R and REX.B */
    int mem;               /* the ModRM names memory */
    uint32_t mem_seg;      /* its segment, the default or the override */
    uint64_t ea;           /* its offset */
};

/** The register `n`, by the processor's number, as `v` holds it, all 64
 * bits. */
static uint64_t *gpr(struct kvm_vcpu *v, uint32_t n)
{
    static const uint8_t slot[16] = {0, 2, 3, 1, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};

    return &((uint64_t *)&v->regs)[slot[n & 15u]];
}

/** The register `n` as a control or debug register move reads it: its low
 * half outside 64-bit mode. */
static uint64_t gpr_read(struct kvm_vcpu *v, uint32_t n)
{
    uint64_t x = *gpr(v, n);

    return kvm_emul_mode(v) == 64 ? x : (x & 0xFFFFFFFFull);
}

/** Write the register `n` of `v`: a 32-bit write outside 64-bit mode. */
static void gpr_write(struct kvm_vcpu *v, uint32_t n, uint64_t value)
{
    *gpr(v, n) = kvm_emul_mode(v) == 64 ? value : (value & 0xFFFFFFFFull);
}

/** Decode a 16-bit memory operand after ModRM. Return its end offset,
 * or -1 when the displacement is not among the fetched bytes. */
static int modrm16(struct kvm_vcpu *v, struct sys_insn *d, uint32_t i)
{
    static const int bases[8] = {3, 3, 5, 5, 6, 7, 5, 3};
    static const int indices[8] = {6, 7, 6, 7, -1, -1, -1, -1};
    uint32_t base = d->rm & 7u, n = 0;
    uint64_t ea = 0;
    int64_t disp = 0;
    int direct = d->mod == 0 && base == 6;
    int ss = 0;

    if (d->mod == 1) {
        n = 1;
    } else if (d->mod == 2 || direct) {
        n = 2;
    }
    if (i + n > d->n) {
        return -1;
    }
    if (n == 1) {
        disp = (int8_t)d->bytes[i];
    } else if (n == 2) {
        disp = (int16_t)(d->bytes[i] | (d->bytes[i + 1] << 8));
    }
    if (!direct) {
        ea = *gpr(v, (uint32_t)bases[base]);
        if (indices[base] >= 0) {
            ea += *gpr(v, (uint32_t)indices[base]);
        }
        ss = base == 2 || base == 3 || base == 6;
    }
    d->ea = (ea + (uint64_t)disp) & 0xFFFFull;
    d->mem_seg = d->seg >= 0 ? (uint32_t)d->seg : ss ? KVM_SEG_SS : KVM_SEG_DS;
    return (int)(i + n);
}

/** Decode a 32- or 64-bit memory operand after ModRM. Return its end
 * offset or -1 for missing bytes. A RIP-relative operand has mem == 2
 * until the caller adds the instruction's end address. */
static int modrm_long(struct kvm_vcpu *v, struct sys_insn *d, uint32_t i, int rex)
{
    uint32_t base = d->rm & 7u, m, index, n = 0;
    uint64_t ea = 0, mask;
    int64_t disp = 0;
    int has_base = 1, rip_rel = 0, ss = 0;

    if (base == 4) {
        if (i >= d->n) {
            return -1;
        }
        m = d->bytes[i++];
        index = ((m >> 3) & 7u) | ((rex & 2) ? 8u : 0u);
        if (index != 4) {
            ea = *gpr(v, index) * (1u << (m >> 6));
        }
        base = m & 7u;
        if (base == 5 && d->mod == 0) {
            has_base = 0;
        }
    } else if (base == 5 && d->mod == 0) {
        has_base = 0;
        rip_rel = kvm_emul_mode(v) == 64;
    }
    base |= (rex & 1) ? 8u : 0u;
    if (has_base) {
        ea += *gpr(v, base);
        ss = (base & 7u) == 4 || (base & 7u) == 5;
    }
    if (d->mod == 1) {
        n = 1;
    } else if (d->mod == 2 || !has_base) {
        n = 4;
    }
    if (i + n > d->n) {
        return -1;
    }
    if (n == 1) {
        disp = (int8_t)d->bytes[i];
    } else if (n == 4) {
        disp = (int32_t)((uint32_t)d->bytes[i] | ((uint32_t)d->bytes[i + 1] << 8) |
                         ((uint32_t)d->bytes[i + 2] << 16) | ((uint32_t)d->bytes[i + 3] << 24));
    }
    mask = d->addr_size == 8u ? ~0ull : 0xFFFFFFFFull;
    d->ea = (ea + (uint64_t)disp) & mask;
    d->mem_seg = d->seg >= 0 ? (uint32_t)d->seg : ss ? KVM_SEG_SS : KVM_SEG_DS;
    if (rip_rel) {
        d->ea = (uint64_t)disp;
        d->mem_seg = d->seg >= 0 ? (uint32_t)d->seg : KVM_SEG_DS;
        d->mem = 2;
    }
    return (int)(i + n);
}

/** Decode ModRM, its SIB and displacement at byte i. Return their byte
 * count or -1 for missing bytes. Set the register numbers and a memory
 * operand's segment and offset; mem == 2 needs the instruction end added. */
static int modrm(struct kvm_vcpu *v, struct sys_insn *d, uint32_t i, int rex)
{
    uint32_t start = i, m;
    int end;

    if (i >= d->n) {
        return -1;
    }
    m = d->bytes[i++];
    d->has_modrm = 1;
    d->mod = m >> 6;
    d->reg = ((m >> 3) & 7u) | ((rex & 4) ? 8u : 0u);
    d->rm = (m & 7u) | ((rex & 1) ? 8u : 0u);
    if (d->mod == 3u) {
        return (int)(i - start);
    }
    d->mem = 1;
    end = d->addr_size == 2u ? modrm16(v, d, i) : modrm_long(v, d, i, rex);
    return end < 0 ? -1 : end - (int)start;
}

/** Read legacy and REX prefixes and set operand and address sizes.
 * Return the opcode offset or -1 when no opcode was fetched. */
static int sys_prefix(struct kvm_vcpu *v, struct sys_insn *d, int *rex_out)
{
    int mode = kvm_emul_mode(v), rex = 0, opsize16 = 0, addr16 = 0, w;
    uint32_t i = 0, b;

    for (;;) {
        if (i >= d->n) {
            return -1;
        }
        b = d->bytes[i];
        if (mode == 64 && (b & 0xF0u) == 0x40u) {
            rex = (int)b;
            i++;
            continue;
        }
        switch (b) {
        case 0x66:
            opsize16 = 1;
            break;
        case 0x67:
            addr16 = 1;
            break;
        case 0xF0:
            d->lock = 1;
            break;
        case 0xF2:
        case 0xF3:
            break;
        case 0x26:
            d->seg = KVM_SEG_ES;
            break;
        case 0x2E:
            d->seg = KVM_SEG_CS;
            break;
        case 0x36:
            d->seg = KVM_SEG_SS;
            break;
        case 0x3E:
            d->seg = KVM_SEG_DS;
            break;
        case 0x64:
            d->seg = KVM_SEG_FS;
            break;
        case 0x65:
            d->seg = KVM_SEG_GS;
            break;
        default:
            goto opcode;
        }
        /* A legacy prefix after REX leaves REX without effect. */
        rex = 0;
        i++;
    }
opcode:
    w = (rex & 8) != 0;
    if (mode == 16) {
        d->opsize = opsize16 ? 4u : 2u;
        d->addr_size = addr16 ? 4u : 2u;
    } else {
        d->opsize = w ? 8u : opsize16 ? 2u : 4u;
        d->addr_size = mode == 64 ? (addr16 ? 4u : 8u) : (addr16 ? 2u : 4u);
    }
    *rex_out = rex;
    return (int)i;
}

/**
 * Decode the instruction at `v`'s CS:RIP that an intercept named, from the
 * set the intercepts of this file hand over: HLT, INT3, INT n, INTO, IRET,
 * ICEBP, CALL and JMP far, direct and indirect; CLTS, INVD, WBINVD, the
 * moves of control and debug registers, WRMSR, RDMSR, RDPMC, CPUID, RSM,
 * and group 7's SMSW, LMSW, INVLPG and register forms. 0 with `d` filled;
 * -1 with #PF queued when the bytes cannot be read; -2 for an instruction
 * outside the set or longer than the bytes read.
 */
static int sys_decode(struct kvm_vcpu *v, struct sys_insn *d)
{
    int mode = kvm_emul_mode(v), rex = 0, r;
    uint32_t i, b;

    k_memset(d, 0, sizeof(*d));
    d->seg = -1;
    r = kvm_emul_fetch(v, d->bytes);
    if (r < 0) {
        return -1;
    }
    d->n = (uint32_t)r;
    r = sys_prefix(v, d, &rex);
    if (r < 0) {
        return -2;
    }
    i = (uint32_t)r;
    b = d->bytes[i++];
    d->op = b;
    switch (b) {
    case 0xF4: /* HLT */
    case 0xCC: /* INT3 */
    case 0xCE: /* INTO */
    case 0xCF: /* IRET */
    case 0xF1: /* ICEBP */
        break;
    case 0xCD: /* INT n */
        i++;
        break;
    case 0x9A: /* CALL far ptr16:16/32 */
    case 0xEA: /* JMP far */
        if (mode == 64) {
            return -2;
        }
        i += d->opsize == 2u ? 4u : 6u;
        break;
    case 0xFF: /* CALL or JMP far m16:16/32/64 */
        r = modrm(v, d, i, rex);
        if (r < 0 || (d->reg & 7u) == 0 || ((d->reg & 7u) != 3 && (d->reg & 7u) != 5)) {
            return -2;
        }
        i += (uint32_t)r;
        break;
    case 0x0F:
        if (i >= d->n) {
            return -2;
        }
        b = d->bytes[i++];
        d->op = 0x0F00u | b;
        switch (b) {
        case 0x06: /* CLTS */
        case 0x08: /* INVD */
        case 0x09: /* WBINVD */
        case 0x30: /* WRMSR */
        case 0x32: /* RDMSR */
        case 0x33: /* RDPMC */
        case 0xA2: /* CPUID */
        case 0xAA: /* RSM */
            break;
        case 0x20: /* MOV from CRn */
        case 0x21: /* MOV from DRn */
        case 0x22: /* MOV to CRn */
        case 0x23: /* MOV to DRn */
            /* The ModRM names registers whatever its mod field says. */
            if (i >= d->n) {
                return -2;
            }
            b = d->bytes[i++];
            d->has_modrm = 1;
            d->mod = 3;
            d->reg = ((b >> 3) & 7u) | ((rex & 4) ? 8u : 0u);
            d->rm = (b & 7u) | ((rex & 1) ? 8u : 0u);
            break;
        case 0x01: /* group 7 */
            r = modrm(v, d, i, rex);
            if (r < 0) {
                return -2;
            }
            i += (uint32_t)r;
            break;
        default:
            return -2;
        }
        break;
    default:
        return -2;
    }
    if (i > d->n || i > KVM_INSN_MAX) {
        return -2;
    }
    d->len = i;
    if (d->mem == 2) {
        d->ea += v->regs.rip + d->len;
        d->mem = 1;
    }
    return 0;
}

static void exit_info(const struct kvm_vcpu *v, uint64_t info[5]);

/** An instruction the backend could not read: the internal error of
 * Linux's handle_emulation_failure at CPL 0 and #UD above it, as for MMIO,
 * with the exit's code, qualifications and event. */
static void emulation_failed(struct kvm_vcpu *v, const struct sys_insn *d)
{
    uint64_t info[5];

    if (kvm_emul_cpl(v) > 0) {
        kvm_queue_exception(v, UD_VECTOR, 0, 0);
        v->exit.kind = KVM_X_AGAIN;
        return;
    }
    exit_info(v, info);
    v->exit.kind = KVM_X_INTERNAL;
    kvm_decode_fail(&v->exit.internal, d ? d->bytes : NULL, d ? d->n : 0, info);
}

/**
 * The length of the instruction the exit of `v` named, for the kernel to
 * step past it: NextRIP's distance from RIP where the processor saved it,
 * else the decoded length. The length, or 0 with `v->exit` set: #PF queued
 * for bytes that cannot be read, or the decoder's failure.
 */
static uint32_t insn_len(struct kvm_vcpu *v)
{
    struct svm_exit *e = &sv(v)->ex;
    struct sys_insn d;
    uint64_t n;
    int r;

    if (e->next_rip) {
        n = (e->next_rip - v->regs.rip) & 0xFFFFFFFFull;
        if (n > 0 && n <= KVM_INSN_MAX) {
            return (uint32_t)n;
        }
    }
    r = sys_decode(v, &d);
    if (r == 0) {
        return d.len;
    }
    if (r == -1) {
        v->exit.kind = KVM_X_AGAIN;
    } else {
        emulation_failed(v, &d);
    }
    return 0;
}

/** Step `v` past the instruction its exit named: 1, or 0 with `v->exit`
 * set as `insn_len` leaves it. */
static int skip(struct kvm_vcpu *v)
{
    uint32_t len = insn_len(v);

    if (len == 0) {
        return 0;
    }
    kvm_emul_skip(v, len);
    return 1;
}

/* ---- the exits ---- */

/** Queue #GP(0) for `v`'s instruction, which does not complete. */
static void gp(struct kvm_vcpu *v)
{
    kvm_queue_exception(v, GP_VECTOR, 1, 0);
}

/** An exit the backend does not handle: the internal error Linux gives
 * for an unexpected exit reason, with the code and the processor. */
static void unexpected(struct kvm_vcpu *v)
{
    struct kvm_exit *ex = &v->exit;

    ex->kind = KVM_X_INTERNAL;
    k_memset(&ex->internal, 0, sizeof(ex->internal));
    ex->internal.suberror = KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
    ex->internal.ndata = 2;
    ex->internal.data[0] = sv(v)->ex.code;
    ex->internal.data[1] = (uint64_t)(int64_t)v->cpu;
}

/**
 * Take the event the exit found under delivery back into `v`'s record, to
 * inject again at the next entry, as Linux's svm_complete_interrupts: an
 * NMI, an exception with its error code, an interrupt, or a software one.
 * A software event this file stepped RIP past without NRIPS has RIP put
 * back on its instruction, so that the record keeps RIP there as for every
 * event. An NMI's blocking ends once RIP has moved from the IRET whose
 * intercept the handler's end gave.
 */
static void complete_events(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    uint32_t info = s->ex.int_info, type = EV_TYPE(info), vec = EV_VECTOR(info);
    int rewind = s->soft_rewind;

    s->soft_rewind = 0;
    if (s->awaiting_iret && v->regs.rip != s->iret_rip) {
        s->awaiting_iret = 0;
        v->events.nmi.masked = 0;
        kvm_vcpu_kick(v, KVM_REQ_EVENT);
    }
    v->events.exception.injected = 0;
    v->events.exception.pending = 0;
    v->events.interrupt.injected = 0;
    v->events.nmi.injected = 0;
    if (!(info & EV_VALID)) {
        return;
    }
    if (rewind && v->regs.rip == s->soft_next_rip &&
        (type == EV_SOFT || (type == EV_EXCEPTION && (vec == BP_VECTOR || vec == OF_VECTOR)))) {
        v->regs.rip = s->soft_old_rip;
    }
    switch (type) {
    case EV_NMI:
        v->events.nmi.injected = 1;
        break;
    case EV_EXCEPTION:
        v->events.exception.injected = 1;
        v->events.exception.nr = (uint8_t)vec;
        v->events.exception.has_error_code = (uint8_t)((info & EV_ERR) != 0);
        v->events.exception.error_code = s->ex.int_err;
        break;
    case EV_INTR:
    case EV_SOFT:
        v->events.interrupt.injected = 1;
        v->events.interrupt.nr = (uint8_t)vec;
        v->events.interrupt.soft = (uint8_t)(type == EV_SOFT);
        if (type == EV_INTR) {
            v->irq_counts[vec & 0xFFu][KVM_VEC_SAVE]++;
        }
        break;
    default:
        break;
    }
    kvm_vcpu_kick(v, KVM_REQ_EVENT);
}

/**
 * The guest's write of `cr0` to CR0, with Linux's checks of kvm_set_cr0:
 * #GP for high bits, NW without CD, PG without PE, long mode's PG without
 * PAE or with a 64-bit CS, PCIDE when paging ends, and PAE's PDPTEs that do
 * not load. Long mode begins and ends with PG while EFER.LME is set. A
 * change of PG, PE or WP drops the guest's translations, which the
 * processor would have done had it run the write. 0, or -1 with #GP queued.
 */
static int set_cr0(struct kvm_vcpu *v, uint64_t cr0)
{
    uint64_t old = v->sregs.cr0, efer = v->sregs.efer;
    uint64_t old_efer = efer;

    cr0 |= CR0_ET;
    if ((cr0 >> 32) || ((cr0 & CR0_NW) && !(cr0 & CR0_CD)) || ((cr0 & CR0_PG) && !(cr0 & CR0_PE))) {
        gp(v);
        return -1;
    }
    if (!(old & CR0_PG) && (cr0 & CR0_PG) && (efer & GEFER_LME)) {
        if (!(v->sregs.cr4 & CR4_PAE) || v->sregs.cs.l) {
            gp(v);
            return -1;
        }
        efer |= GEFER_LMA;
    }
    if ((old & CR0_PG) && !(cr0 & CR0_PG)) {
        if (v->sregs.cr4 & CR4_PCIDE) {
            gp(v);
            return -1;
        }
        efer &= ~GEFER_LMA;
    }
    v->sregs.cr0 = cr0;
    v->sregs.efer = efer;
    if (pae_paging(v) && ((old ^ cr0) & (CR0_PG | CR0_PE | CR0_CD | CR0_NW)) && !pdptes_ok(v)) {
        v->sregs.cr0 = old;
        v->sregs.efer = old_efer;
        gp(v);
        return -1;
    }
    if ((old ^ cr0) & (CR0_PG | CR0_PE | CR0_WP)) {
        sv(v)->flush = 1;
    }
    return 0;
}

/**
 * The guest's write of `cr4` to CR4, with Linux's checks of kvm_set_cr4:
 * #GP for a bit the guest's CPUID reserves, PAE cleared in long mode, PCIDE
 * set outside long mode or with CR3's PCID nonzero, and PDPTEs that do not
 * load. A change of PGE, PCIDE, PAE, PSE or SMEP drops the guest's
 * translations, as the processor does. 0, or -1 with #GP queued.
 */
static int set_cr4(struct kvm_vcpu *v, uint64_t cr4)
{
    uint64_t old = v->sregs.cr4;
    int lma = (v->sregs.efer & GEFER_LMA) != 0;

    if ((cr4 & kvm_cr4_reserved(v)) || (lma && !(cr4 & CR4_PAE)) ||
        ((cr4 & CR4_PCIDE) && !(old & CR4_PCIDE) && (!lma || (v->sregs.cr3 & 0xFFFu)))) {
        gp(v);
        return -1;
    }
    v->sregs.cr4 = cr4;
    if (pae_paging(v) && ((old ^ cr4) & (CR4_PGE | CR4_PSE | CR4_PAE | CR4_SMEP)) &&
        !pdptes_ok(v)) {
        v->sregs.cr4 = old;
        gp(v);
        return -1;
    }
    if ((old ^ cr4) & (CR4_PGE | CR4_PCIDE | CR4_PAE | CR4_PSE | CR4_SMEP)) {
        sv(v)->flush = 1;
    }
    return 0;
}

/** Write `val` to the guest's CR8, the APIC's TPR: #GP for a value past
 * four bits. 0 or -1. */
static int set_cr8(struct kvm_vcpu *v, uint64_t val)
{
    if (val & ~0xFull) {
        gp(v);
        return -1;
    }
    if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
        kvm_lapic_set_cr8(v, val);
    } else {
        v->sregs.cr8 = val;
    }
    /* A lower priority may let an interrupt through. */
    kvm_vcpu_kick(v, KVM_REQ_EVENT);
    return 0;
}

/**
 * Carry out a decoded MOV to or from a control register. Return 0 on
 * success, -1 with a fault queued, or -2 for an unsupported register.
 */
static int mov_cr(struct kvm_vcpu *v, const struct sys_insn *d)
{
    uint64_t val;
    uint32_t cr;
    int r;

    cr = d->reg;
    if (cr == 0 && d->lock && kvm_emul_mode(v) != 64) {
        cr = 8;
    }
    if (d->op == 0x0F22) {
        val = gpr_read(v, d->rm);
        r = cr == 0 ? set_cr0(v, val) : cr == 4 ? set_cr4(v, val) : cr == 8 ? set_cr8(v, val) : -2;
    } else {
        r = 0;
        if (cr == 0) {
            gpr_write(v, d->rm, v->sregs.cr0);
        } else if (cr == 4) {
            gpr_write(v, d->rm, v->sregs.cr4);
        } else if (cr == 8) {
            gpr_write(v, d->rm, get_cr8(v));
        } else if (cr == 2) {
            gpr_write(v, d->rm, v->sregs.cr2);
        } else if (cr == 3) {
            gpr_write(v, d->rm, v->sregs.cr3);
        } else {
            r = -2;
        }
    }
    return r;
}

/**
 * A control register's intercept: the instruction decoded and carried out,
 * as Linux's emulator does without decode assists: MOV to and from CR0,
 * CR4 and CR8, LOCK MOV CR0 as CR8 outside 64-bit mode, CLTS, LMSW and
 * SMSW. CR0 reads give the guest's CR0, CD and NW among them.
 */
static int exit_cr(struct kvm_vcpu *v)
{
    struct sys_insn d;
    uint64_t val;
    uint16_t w;
    int r;

    v->exit.kind = KVM_X_AGAIN;
    r = sys_decode(v, &d);
    if (r == -1) {
        return 0;
    }
    if (r != 0) {
        emulation_failed(v, &d);
        return 0;
    }
    r = 0;
    switch (d.op) {
    case 0x0F20:
    case 0x0F22:
        r = mov_cr(v, &d);
        break;
    case 0x0F06:
        r = set_cr0(v, v->sregs.cr0 & ~CR0_TS);
        break;
    case 0x0F01:
        if ((d.reg & 7u) == 6) {
            /* LMSW: the low four bits, PE set but never cleared. */
            if (d.mem) {
                if (kvm_guest_rw(v, d.mem_seg, d.ea, &w, 2, 0) != 0) {
                    return 0;
                }
                val = w;
            } else {
                val = *gpr(v, d.rm);
            }
            r = set_cr0(v, (v->sregs.cr0 & ~0xEull) | (val & 0xFu) | (v->sregs.cr0 & CR0_PE));
        } else if ((d.reg & 7u) == 4) {
            /* SMSW: all of CR0 into a register, its low 16 bits into
             * memory. */
            if (d.mem) {
                w = (uint16_t)v->sregs.cr0;
                if (kvm_guest_rw(v, d.mem_seg, d.ea, &w, 2, 1) != 0) {
                    return 0;
                }
            } else if (d.opsize == 2u) {
                *gpr(v, d.rm) = (*gpr(v, d.rm) & ~0xFFFFull) | (v->sregs.cr0 & 0xFFFFu);
            } else {
                gpr_write(v, d.rm, d.opsize == 8u ? v->sregs.cr0 : v->sregs.cr0 & 0xFFFFFFFFull);
            }
        } else {
            r = -2;
        }
        break;
    default:
        r = -2;
        break;
    }
    if (r == -2) {
        kvm_queue_exception(v, UD_VECTOR, 0, 0);
        return 0;
    }
    if (r == 0) {
        kvm_emul_skip(v, d.len);
    }
    return 0;
}

/** A debug register's intercept, every one of which exits, as Linux's
 * emulator carries them out: DR4 and DR5 alias DR6 and DR7 unless CR4.DE
 * makes them #UD, DR7.GD raises #DB, and the upper halves of DR6 and DR7
 * are #GP. */
static int exit_dr(struct kvm_vcpu *v)
{
    struct sys_insn d;
    uint32_t dr;
    uint64_t val;
    int r;

    v->exit.kind = KVM_X_AGAIN;
    r = sys_decode(v, &d);
    if (r == -1) {
        return 0;
    }
    if (r != 0 || (d.op != 0x0F21 && d.op != 0x0F23)) {
        emulation_failed(v, &d);
        return 0;
    }
    dr = d.reg;
    if (dr > 7) {
        kvm_queue_exception(v, UD_VECTOR, 0, 0);
        return 0;
    }
    if (dr == 4 || dr == 5) {
        if (v->sregs.cr4 & CR4_DE) {
            kvm_queue_exception(v, UD_VECTOR, 0, 0);
            return 0;
        }
        dr += 2;
    }
    if (dr7_of(v) & DR7_GD) {
        /* The exit comes before DR7.GD's trap, which is made here: the
         * caller's, while it sets the breakpoints, as a debug exit, and
         * the guest's own as its #DB, as Linux's handle_dr. */
        if (debugging(v, KVM_GUESTDBG_USE_HW_BP)) {
            struct kvm_run_debug *dbg = &v->exit.debug;

            v->exit.kind = KVM_X_DEBUG;
            k_memset(dbg, 0, sizeof(*dbg));
            dbg->exception = DB_VECTOR;
            dbg->pc = v->sregs.cs.base + v->regs.rip;
            dbg->dr6 = DR6_BD | DR6_FIXED;
            dbg->dr7 = dr7_of(v);
            return 0;
        }
        v->debugregs.dr6 = (v->debugregs.dr6 & ~0xFull) | DR6_BD | DR6_FIXED;
        v->debugregs.dr7 &= ~DR7_GD;
        v->dirty |= KVM_PART_DEBUG;
        kvm_queue_exception(v, DB_VECTOR, 0, 0);
        return 0;
    }
    if (d.op == 0x0F23) {
        val = gpr_read(v, d.rm);
        if (dr < 4) {
            v->debugregs.db[dr] = val;
        } else if (val >> 32) {
            gp(v);
            return 0;
        } else if (dr == 6) {
            v->debugregs.dr6 = val | DR6_FIXED;
        } else {
            v->debugregs.dr7 = (val & 0xFFFF2BFFull) | DR7_FIXED;
        }
        v->dirty |= KVM_PART_DEBUG;
    } else {
        val = dr < 4 ? v->debugregs.db[dr] : dr == 6 ? v->debugregs.dr6 : v->debugregs.dr7;
        gpr_write(v, d.rm, val);
    }
    kvm_emul_skip(v, d.len);
    return 0;
}

/**
 * Port I/O: EXITINFO1's direction, size, string, REP, address size and
 * port, and EXITINFO2's next RIP, which the processor always gives. OUTS's
 * segment is the override among the instruction's prefixes, or DS.
 */
static int exit_io(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    uint64_t q = s->ex.info1;
    uint8_t bytes[KVM_INSN_MAX];
    struct kvm_pio p;
    int n, i;

    k_memset(&p, 0, sizeof(p));
    p.in = (q & IOIO_IN) != 0;
    p.size = (q & IOIO_SZ8) ? 1u : (q & IOIO_SZ16) ? 2u : 4u;
    p.string = (q & IOIO_STR) != 0;
    p.rep = (q & IOIO_REP) != 0;
    p.port = (uint16_t)(q >> 16);
    p.len = (uint32_t)((s->ex.info2 - v->regs.rip) & 0xFu);
    if (p.string) {
        p.addr_size = (q & IOIO_A16) ? 2u : (q & IOIO_A32) ? 4u : 8u;
        p.seg = KVM_SEG_DS;
        if (!p.in) {
            n = kvm_emul_fetch(v, bytes);
            if (n < 0) {
                v->exit.kind = KVM_X_AGAIN;
                return 0;
            }
            for (i = 0; i < n && i < (int)p.len; i++) {
                if (bytes[i] == 0x26) {
                    p.seg = KVM_SEG_ES;
                } else if (bytes[i] == 0x2E) {
                    p.seg = KVM_SEG_CS;
                } else if (bytes[i] == 0x36) {
                    p.seg = KVM_SEG_SS;
                } else if (bytes[i] == 0x3E) {
                    p.seg = KVM_SEG_DS;
                } else if (bytes[i] == 0x64) {
                    p.seg = KVM_SEG_FS;
                } else if (bytes[i] == 0x65) {
                    p.seg = KVM_SEG_GS;
                } else if (bytes[i] == 0x6E || bytes[i] == 0x6F) {
                    break;
                }
            }
        }
    }
    return kvm_emul_pio(v, &p);
}

/**
 * The guest's EFER, as Linux's set_efer: #GP for a bit the guest's CPUID
 * does not offer, or a change of LME while paging; LMA stays as paging
 * set it. 0 or -1.
 */
static int set_efer(struct kvm_vcpu *v, uint64_t efer)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 0x80000001u, 0);
    uint64_t ok = 0;

    if (e && ((e->edx >> 11) & 1u)) {
        ok |= GEFER_SCE;
    }
    if (e && ((e->edx >> 29) & 1u)) {
        ok |= GEFER_LME | GEFER_LMA;
    }
    if (e && ((e->edx >> 20) & 1u)) {
        ok |= GEFER_NXE;
    }
    if (e && ((e->edx >> 25) & 1u)) {
        ok |= GEFER_FFXSR;
    }
    if ((efer & ~ok) || ((v->sregs.cr0 & CR0_PG) && ((efer ^ v->sregs.efer) & GEFER_LME))) {
        return -1;
    }
    v->sregs.efer = (efer & ~GEFER_LMA) | (v->sregs.efer & GEFER_LMA);
    return 0;
}

/** Make msr.c's KERNEL_GS_BASE the guest's, which VMSAVE stored, before
 * anything there changes, since the next entry loads the VMCB from
 * msr.c. */
static void kgs_sync(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);

    if (!s->kgs_table) {
        (void)kvm_msr_set(v, MSR_KERNEL_GS_BASE, s->vmcb->s.kernel_gs_base, 1);
        s->kgs_table = 1;
    }
}

/** The guest's RDMSR or WRMSR of `index`, by `write`, with `*value` its
 * EDX:EAX: EFER and DEBUGCTL here, the rest through `kvm_emul_msr`. 0, with
 * a read's value in `*value`, or a KVM_MSR_* failure. */
static int msr_access(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value)
{
    switch (index) {
    case MSR_EFER:
        if (write) {
            return set_efer(v, *value);
        }
        *value = v->sregs.efer;
        return 0;
    case MSR_IA32_DEBUGCTL:
        /* No LBR and no BTF, which Linux drops with a note; any other bit
         * is #GP. */
        if (write) {
            return (*value & ~(DEBUGCTL_LBR | DEBUGCTL_BTF)) ? KVM_MSR_INVAL : 0;
        }
        *value = sv(v)->debugctl;
        return 0;
    default:
        return kvm_emul_msr(v, index, write, value);
    }
}

/** RDMSR and WRMSR, by EXITINFO1, as Linux's kvm_emulate_rdmsr and
 * kvm_emulate_wrmsr: an access KVM_X86_SET_MSR_FILTER denies fails at
 * once, any other is `msr_access`'s. A failure goes to the caller when
 * KVM_CAP_X86_USER_SPACE_MSR asked for its reason, and is #GP otherwise,
 * as Linux's default without ignore_msrs. */
static int exit_msr(struct kvm_vcpu *v)
{
    int write = (sv(v)->ex.info1 & 1u) != 0, r;
    uint32_t index = (uint32_t)v->regs.rcx, len;
    uint64_t value = (v->regs.rax & 0xFFFFFFFFull) | (v->regs.rdx << 32);

    v->exit.kind = KVM_X_AGAIN;
    if (index - MSR_FS_BASE < 3u && kvm_msr_allowed(v, index, write)) {
        /* The map predates the filter, and KVM_REQ_MSR_FILTER brings it up
         * to date before the instruction runs again. */
        return 0;
    }
    if (write) {
        kgs_sync(v);
    }
    r = kvm_msr_allowed(v, index, write) ? msr_access(v, index, write, &value) : KVM_MSR_FILTERED;
    if (r != 0) {
        if (v->vm->user_space_msr) {
            /* The caller's answer skips the instruction. */
            len = insn_len(v);
            if (len == 0 || kvm_msr_user_exit(v, index, write, value, r, len)) {
                return 0;
            }
        }
        gp(v);
        return 0;
    }
    if (!write) {
        v->regs.rax = value & 0xFFFFFFFFull;
        v->regs.rdx = value >> 32;
    }
    (void)skip(v);
    return 0;
}

/** Dispatch an enabled Hyper-V hypercall, else a KVM hypercall, whose
 * answer goes in RAX. */
static int exit_vmmcall(struct kvm_vcpu *v)
{
    uint64_t ret, rax = v->regs.rax;
    uint32_t len;

    if (kvm_hv_hypercall_enabled(v)) {
        len = sv(v)->ex.next_rip ? (uint32_t)(sv(v)->ex.next_rip - v->regs.rip) : 3u;
        return kvm_hv_hypercall(v, len);
    }
    ret = kvm_emul_hypercall(v);

    v->regs.rax = kvm_emul_mode(v) == 64 ? ret : (ret & 0xFFFFFFFFull);
    if (!skip(v)) {
        v->regs.rax = rax;
    }
    return 0;
}

/** The exit's numbers for an internal error's report, as Linux's
 * svm_get_exit_info gives them: the code, EXITINFO1 and EXITINFO2, and
 * the event under delivery with its error code, 0 unless the event is
 * valid and has one. */
static void exit_info(const struct kvm_vcpu *v, uint64_t info[5])
{
    const struct svm_exit *e = &sv(v)->ex;
    uint32_t coded = EV_VALID | EV_ERR;

    info[0] = e->code;
    info[1] = e->info1;
    info[2] = e->info2;
    info[3] = e->int_info;
    info[4] = (e->int_info & coded) == coded ? e->int_err : 0;
}

/** A nested page fault: EXITINFO2's guest-physical address, which
 * `kvm_tdp_fault` maps, or an access to a frame no slot holds, MMIO, which
 * emulate.c carries out. */
static int exit_npf(struct kvm_vcpu *v)
{
    struct svm_exit *e = &sv(v)->ex;
    uint64_t info[5];
    int r;

    r = kvm_tdp_fault(v->vm, e->info2 >> 12, (e->info1 & 2u) != 0);
    if (r < 0) {
        return r;
    }
    if (r == 0) {
        v->exit.kind = KVM_X_AGAIN;
        return 0;
    }
    exit_info(v, info);
    return kvm_emul_mmio(v, info);
}

/**
 * An exception's intercept: the host's machine check, which `enter` passed
 * on; #AC given back to the guest; #UD, whose instruction emulate.c runs
 * when it is one Linux's emulator runs after a #UD, as Linux's
 * ud_interception; the #DB of the NMI window's step, which ends it; the
 * #DB or #BP the caller's debugging takes, which goes to the caller as
 * KVM_EXIT_DEBUG with the address, DR6 and DR7, as Linux's db_interception
 * and bp_interception give it; and the guest's own #DB, given back with
 * its bits, which the processor left in the VMCB's DR6, in the guest's.
 */
static int exit_exception(struct kvm_vcpu *v, uint32_t vec)
{
    struct svm_vcpu *s = sv(v);
    struct kvm_run_debug *d = &v->exit.debug;
    uint64_t info[5];
    int caller;

    v->exit.kind = KVM_X_AGAIN;
    switch (vec) {
    case MC_VECTOR:
        return 0;
    case UD_VECTOR:
        exit_info(v, info);
        return kvm_emul_ud(v, info);
    case AC_VECTOR:
        kvm_queue_exception(v, AC_VECTOR, 1, 0);
        return 0;
    case DB_VECTOR:
        caller = debugging(v, KVM_GUESTDBG_SINGLESTEP | KVM_GUESTDBG_USE_HW_BP);
        if (!caller && !s->nmi_ss) {
            kvm_queue_db(v, s->vmcb->s.dr6 ^ DR6_ACTIVE_LOW);
            return 0;
        }
        if (s->nmi_ss) {
            s->nmi_ss = 0;
            kvm_vcpu_kick(v, KVM_REQ_EVENT);
        }
        if (!caller) {
            return 0;
        }
        break;
    case BP_VECTOR:
        if (!debugging(v, KVM_GUESTDBG_USE_SW_BP)) {
            unexpected(v);
            return 0;
        }
        break;
    default:
        unexpected(v);
        return 0;
    }
    v->exit.kind = KVM_X_DEBUG;
    k_memset(d, 0, sizeof(*d));
    d->exception = vec;
    d->pc = v->sregs.cs.base + v->regs.rip;
    d->dr6 = vec == DB_VECTOR ? s->vmcb->s.dr6 : 0;
    d->dr7 = dr7_of(v);
    return 0;
}

/**
 * A task switch, which the kernel carries out, as Linux's
 * task_switch_interception: the TSS selector from EXITINFO1, and from
 * EXITINFO2 the source, IRET or JMP, else a gate when an event was under
 * delivery, else CALL, with an exception's error code; through a gate, the
 * event taken off the queue; an instruction that caused it stepped past in
 * the EIP the old task saves. A TSS or table in no slot is the internal
 * error Linux reports.
 */
static int exit_task_switch(struct kvm_vcpu *v)
{
    struct svm_exit *e = &sv(v)->ex;
    uint32_t info = e->int_info, type = EV_TYPE(info), vec = EV_VECTOR(info);
    int valid = (info & EV_VALID) != 0, reason, has_error = 0, r;
    uint64_t next = v->regs.rip;
    uint32_t len;

    if (e->info2 & TS_IRET) {
        reason = KVM_TS_IRET;
    } else if (e->info2 & TS_JMP) {
        reason = KVM_TS_JMP;
    } else if (valid) {
        reason = KVM_TS_GATE;
    } else {
        reason = KVM_TS_CALL;
    }
    if (reason == KVM_TS_GATE) {
        switch (type) {
        case EV_NMI:
            v->events.nmi.injected = 0;
            break;
        case EV_EXCEPTION:
            has_error = (e->info2 & TS_HAS_CODE) != 0;
            v->events.exception.injected = 0;
            v->events.exception.pending = 0;
            break;
        case EV_INTR:
        case EV_SOFT:
            v->events.interrupt.injected = 0;
            break;
        default:
            break;
        }
    }
    if (reason != KVM_TS_GATE || type == EV_SOFT ||
        (type == EV_EXCEPTION && (vec == OF_VECTOR || vec == BP_VECTOR))) {
        len = insn_len(v);
        if (len == 0) {
            return 0;
        }
        next = (next + len) & 0xFFFFFFFFull;
    }
    r = kvm_task_switch(v, (uint16_t)e->info1, reason, valid && type == EV_SOFT ? (int)vec : -1,
                        next, has_error ? (int64_t)(uint32_t)e->info2 : -1);
    v->exit.kind = KVM_X_AGAIN;
    if (r > 0) {
        v->exit.kind = KVM_X_INTERNAL;
        k_memset(&v->exit.internal, 0, sizeof(v->exit.internal));
        v->exit.internal.suberror = KVM_INTERNAL_ERROR_EMULATION;
    }
    return r < 0 && r != -1 ? r : 0;
}

/**
 * The intercepts of instructions the kernel answers alone, each stepped
 * past or faulted: CPUID, HLT, INVD, MONITOR, MWAIT, WBINVD, RDPMC, XSETBV,
 * VMMCALL, and the SVM instructions of a nested guest, RDTSCP the guest's
 * CPUID lacks, RDPRU and RSM, which are #UD. Whether `code` is one of
 * them, with `v->exit` set.
 */
static int exit_insn(struct kvm_vcpu *v, uint64_t code)
{
    switch (code) {
    case EXIT_CPUID:
        if (kvm_emul_cpuid(v) == 0) {
            (void)skip(v);
        }
        return 1;
    case EXIT_HLT:
        if (skip(v)) {
            v->exit.kind = KVM_X_HLT;
        }
        return 1;
    case EXIT_MONITOR:
    case EXIT_MWAIT:
    case EXIT_MWAIT_COND:
        /* MONITOR and MWAIT as no-ops, as Linux's kvm_emulate_monitor_mwait:
         * #UD only with KVM_X86_QUIRK_MWAIT_NEVER_UD_FAULTS turned off, for a
         * guest whose CPUID lacks MWAIT. */
        if (!KVM_QUIRK(v->vm, KVM_X86_QUIRK_MWAIT_NEVER_UD_FAULTS) && !kvm_msr_guest_mwait(v)) {
            kvm_queue_exception(v, UD_VECTOR, 0, 0);
            return 1;
        }
        (void)skip(v);
        return 1;
    case EXIT_INVD:
        /* A no-op, as Linux's kvm_emulate_invd. */
        (void)skip(v);
        return 1;
    case EXIT_WBINVD:
        /* A cache flush matters only to a device that does not snoop, as
         * Linux's kvm_emulate_wbinvd judges. */
#ifndef RACH2_HOSTTEST
        if (v->vm->noncoherent) {
            hw_wbinvd();
        }
#endif
        (void)skip(v);
        return 1;
    case EXIT_RDPMC:
        /* No performance counter is offered, so every counter is #GP. */
        gp(v);
        return 1;
    case EXIT_XSETBV:
        if (kvm_emul_xsetbv(v) == 0) {
            (void)skip(v);
        }
        return 1;
    case EXIT_VMMCALL:
        (void)exit_vmmcall(v);
        return 1;
    case EXIT_VMRUN:
    case EXIT_VMLOAD:
    case EXIT_VMSAVE:
    case EXIT_STGI:
    case EXIT_CLGI:
    case EXIT_SKINIT:
    case EXIT_INVLPGA:
    case EXIT_RDTSCP:
    case EXIT_RDPRU:
    case EXIT_RSM:
        kvm_queue_exception(v, UD_VECTOR, 0, 0);
        return 1;
    default:
        return 0;
    }
}

/**
 * Name the exit `enter` read: handle it in the kernel, KVM_X_AGAIN, or
 * give it to the run loop. Each exit the processor gives with these
 * intercepts has its case. 0, or -errno to end KVM_RUN.
 */
static int exit_code(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    uint64_t code = s->ex.code;

    if (v->emul.resume) {
        return kvm_emul_resume(v);
    }
    if (code == EXIT_INVALID) {
        v->exit.kind = KVM_X_FAIL_ENTRY;
        v->exit.hw_reason = code;
        return 0;
    }
    complete_events(v);
    v->if_flag = (v->regs.rflags & RFLAGS_IF) != 0;
    v->irq_allowed = v->if_flag && !v->events.interrupt.shadow;
    v->exit.kind = KVM_X_AGAIN;
    if (exit_insn(v, code)) {
        return 0;
    }
    if (code < EXIT_READ_DR0) {
        return exit_cr(v);
    }
    if (code >= EXIT_READ_DR0 && code < EXIT_EXCP_BASE) {
        return exit_dr(v);
    }
    if (code >= EXIT_EXCP_BASE && code < EXIT_EXCP_BASE + 32u) {
        return exit_exception(v, (uint32_t)(code - EXIT_EXCP_BASE));
    }
    switch (code) {
    case EXIT_INTR:
        v->at_boundary = 1;
        return 0;
    case EXIT_NMI:
    case EXIT_SMI:
    case EXIT_INIT:
    case EXIT_PAUSE:
        return 0;
    case EXIT_VINTR:
        s->want_vintr = 0;
        v->exit.kind = KVM_X_IRQ_WINDOW;
        return 0;
    case EXIT_IRET:
        /* The NMI handler's IRET: blocking ends once it has run, which
         * the next exit sees by RIP, as Linux's iret_interception. */
        s->awaiting_iret = 1;
        s->iret_rip = v->regs.rip;
        s->vmcb->c.intercept3 &= ~INTERCEPT3_IRET;
        kvm_vcpu_kick(v, KVM_REQ_EVENT);
        return 0;
    case EXIT_SHUTDOWN:
        /* The VMCB is undefined after a shutdown, so it is built anew, as
         * Linux's shutdown_interception. */
        s->built = 0;
        v->exit.kind = KVM_X_SHUTDOWN;
        return 0;
    case EXIT_IOIO:
        return exit_io(v);
    case EXIT_MSR:
        return exit_msr(v);
    case EXIT_NPF:
        return exit_npf(v);
    case EXIT_TASK_SWITCH:
        return exit_task_switch(v);
    default:
        break;
    }
    unexpected(v);
    return 0;
}

/**
 * The backend's `exit`, under the big lock with interrupts on: the exit
 * named, and an instruction the kernel stepped under the caller's single
 * step ended as KVM_EXIT_DEBUG with DR6.BS, as Linux's
 * kvm_vcpu_do_singlestep. 0, or -errno to end KVM_RUN.
 */
static int svm_exit(struct kvm_vcpu *v)
{
    struct kvm_run_debug *d = &v->exit.debug;
    int r;

    v->emul.step_trap = 0;
    r = exit_code(v);
    if (r == 0 && v->emul.step_trap && v->exit.kind == KVM_X_AGAIN) {
        v->exit.kind = KVM_X_DEBUG;
        k_memset(d, 0, sizeof(*d));
        d->exception = DB_VECTOR;
        d->pc = v->sregs.cs.base + v->regs.rip;
        d->dr6 = DR6_BS | DR6_FIXED;
        d->dr7 = dr7_of(v);
    }
    v->emul.step_trap = 0;
    return r;
}

/** The answer to the read of the last exit, for the instruction under
 * way. */
static void svm_complete(struct kvm_vcpu *v)
{
    (void)kvm_emul_complete(v);
}

/** Write back the parts `parts` of the guest's state: KERNEL_GS_BASE from
 * the VMCB, which SWAPGS changes without an exit. The record holds the
 * rest, read from the VMCB at each exit. */
static void svm_save(struct kvm_vcpu *v, uint32_t parts)
{
    if ((parts & KVM_PART_MSRS) && sv(v)->built) {
        kgs_sync(v);
    }
}

/** Whether `v` can take an NMI at the next entry: no NMI blocking, and no
 * interrupt shadow, as Linux's svm_nmi_blocked. */
static int nmi_open(const struct kvm_vcpu *v)
{
    return !v->events.nmi.masked && !v->events.interrupt.shadow;
}

/** Whether `v` can take an interrupt at the next entry: RFLAGS.IF and no
 * shadow, as Linux's svm_interrupt_blocked. */
static int irq_open(const struct kvm_vcpu *v)
{
    return (v->regs.rflags & RFLAGS_IF) && !v->events.interrupt.shadow;
}

/** Whether the exception `nr` pushes an error code. */
static int has_error_code(uint32_t nr)
{
    return nr == DF_VECTOR || nr == TS_VECTOR || nr == NP_VECTOR || nr == SS_VECTOR ||
           nr == GP_VECTOR || nr == PF_VECTOR || nr == AC_VECTOR || nr == CP_VECTOR;
}

/**
 * Ready a software event, INT n, INT3 or INTO, whose instruction is at
 * RIP, for an injection that pushes the next instruction's address: under
 * NRIPS that address goes in NextRIP; without it RIP is stepped past the
 * instruction, which the exit puts back if the event is still under
 * delivery, as Linux's svm_update_soft_interrupt_rip. 1, or 0 when the
 * instruction cannot be read, and the event is dropped so that the guest
 * runs it again.
 */
static int soft_ready(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    struct sys_insn d;
    int r;

    r = sys_decode(v, &d);
    if (r != 0 || (d.op != 0xCD && d.op != 0xCC && d.op != 0xCE)) {
        if (r == -1) {
            /* The #PF the fetch queued belongs to no instruction the guest
             * runs now. */
            v->events.exception.pending = 0;
        }
        return 0;
    }
    if (caps.nrips) {
        s->inj_next_rip = v->regs.rip + d.len;
    } else {
        s->soft_old_rip = v->regs.rip;
        v->regs.rip += d.len;
        if (kvm_emul_mode(v) != 64) {
            v->regs.rip &= 0xFFFFFFFFull;
        }
        s->soft_next_rip = v->regs.rip;
        s->soft_rewind = 1;
    }
    return 1;
}

/** EVENTINJ for the exception `v` has injected or pending, with its error
 * code; INT3 and INTO readied as software events. 0 when one of those
 * cannot be read. */
static uint32_t inject_exception(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    uint32_t nr = v->events.exception.nr, info = nr | (EV_EXCEPTION << 8) | EV_VALID;

    if ((nr == BP_VECTOR || nr == OF_VECTOR) && !soft_ready(v)) {
        v->events.exception.injected = 0;
        v->events.exception.pending = 0;
        return 0;
    }
    kvm_exception_rf(v);
    /* No error code in real mode, as Linux's kvm_inject_exception drops it
     * outside protected mode. */
    if ((v->events.exception.has_error_code || v->events.exception.pending) && has_error_code(nr) &&
        (v->sregs.cr0 & CR0_PE)) {
        info |= EV_ERR;
        s->inj_err = v->events.exception.error_code;
    }
    v->events.exception.injected = 1;
    v->events.exception.pending = 0;
    return info;
}

/** With the local APIC in the kernel and no other event to inject,
 * EVENTINJ for the vector the APIC gives when `v` can take it, 0 otherwise,
 * with the window armed while one waits. */
static uint32_t inject_lapic(struct kvm_vcpu *v)
{
    int vec = kvm_lapic_pending(v);

    if (vec < 0) {
        return 0;
    }
    if (!irq_open(v)) {
        sv(v)->want_vintr = 1;
        return 0;
    }
    vec = kvm_lapic_ack(v);
    if (vec < 0) {
        return 0;
    }
    v->events.interrupt.injected = 1;
    v->events.interrupt.nr = (uint8_t)vec;
    v->events.interrupt.soft = 0;
    return (uint32_t)vec | (EV_INTR << 8) | EV_VALID;
}

/** Open the window of an NMI that waits, as Linux's svm_enable_nmi_window:
 * while blocked with the handler's IRET still to come, its intercept is
 * the window; otherwise the guest is stepped over what blocks it, an IRET
 * that ran, an event injected or an interrupt shadow. */
static void nmi_window(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);

    if (v->events.nmi.masked && !s->awaiting_iret) {
        return;
    }
    if (!s->nmi_ss) {
        s->nmi_ss = 1;
        s->nmi_ss_rflags = v->regs.rflags;
    }
}

/**
 * Choose the event the next entry injects, as Linux's
 * kvm_check_and_inject_events orders them: an exception, then an NMI, then
 * an interrupt, the one the run was delivering first; with the local APIC
 * in the kernel the vector it gives when the guest can take it. An NMI
 * blocks NMIs until its handler's IRET; an NMI or an interrupt the guest
 * cannot take now opens its window.
 */
static void svm_inject(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);
    uint32_t info = 0;
    int split = v->vm->irqchip != KVM_IRQCHIP_NONE;
    /* No new NMI or interrupt while the caller's KVM_GUESTDBG_BLOCKIRQ
     * holds them, as Linux's kvm_check_and_inject_events. */
    int block = debugging(v, KVM_GUESTDBG_BLOCKIRQ);

    if (!s->built) {
        vmcb_build(v);
    }
    s->want_vintr = 0;
    s->inj_err = 0;
    if (v->events.exception.injected || v->events.exception.pending) {
        info = inject_exception(v);
    }
    if (info) {
        /* the exception */
    } else if (v->events.nmi.injected) {
        info = NMI_VECTOR | (EV_NMI << 8) | EV_VALID;
    } else if (v->nmi_queued && nmi_open(v) && !block) {
        v->nmi_queued--;
        v->events.nmi.injected = 1;
        info = NMI_VECTOR | (EV_NMI << 8) | EV_VALID;
    } else if (v->events.interrupt.injected) {
        if (!v->events.interrupt.soft) {
            info = v->events.interrupt.nr | (EV_INTR << 8) | EV_VALID;
        } else if (soft_ready(v)) {
            info = v->events.interrupt.nr | (EV_SOFT << 8) | EV_VALID;
        } else {
            v->events.interrupt.injected = 0;
        }
    }
    if (EV_TYPE(info) == EV_NMI && info) {
        /* The delivery blocks NMIs until the handler's IRET, which the
         * kernel intercepts, as Linux's svm_inject_nmi. */
        v->events.nmi.masked = 1;
        s->awaiting_iret = 0;
    }
    if (v->nmi_queued && (info || !nmi_open(v))) {
        nmi_window(v);
    }
    if (split && info) {
        if (kvm_lapic_pending(v) >= 0) {
            s->want_vintr = 1;
        }
    } else if (split && !block) {
        info = inject_lapic(v);
    }
    s->inj = info;
    v->dirty |= KVM_PART_EVENTS;
}

/** Single stepping and event injection are handled by the backend. */
static int32_t svm_check_extension(long cap)
{
    return cap == KVM_CAP_X86_ROBUST_SINGLESTEP ? 1 : 0;
}

/** The backend's share of a new VM: the host save areas of the processors
 * and the I/O permission map, made at the first VM and kept. 0 or
 * -ENOMEM. */
static int svm_vm_init(struct kvm_vm *vm)
{
    uintptr_t pa;
    uint32_t i, ncpu = (uint32_t)smp_cpu_count();

    vm->backend = NULL;
    if (!iopm) {
        iopm = kvm_map_pages(3, &iopm_pa);
        if (!iopm) {
            return -ENOMEM;
        }
    }
    for (i = 0; i < ncpu && i < NR_CPUS; i++) {
        if (hsave[i]) {
            continue;
        }
        hsave[i] = kalloc_pages(1, &pa);
        if (!hsave[i]) {
            return -ENOMEM;
        }
        k_memset(hsave[i], 0, 4096);
        hsave_pa[i] = pa;
    }
    return 0;
}

/**
 * Have the guest of `v` reach FS_BASE, GS_BASE and KERNEL_GS_BASE, which
 * VMLOAD and VMSAVE move with the VMCB, directly but for the accesses
 * KVM_X86_SET_MSR_FILTER denies, which exit, as Linux's
 * svm_msr_filter_changed. The MSRs from 0xC0000000 have two bits each from
 * byte 2048 of the map, the read's then the write's.
 */
static void svm_msr_filter_changed(struct kvm_vcpu *v)
{
    uint8_t *b = sv(v)->msrpm + 2048 + (0x100 * 2) / 8;
    uint8_t bits = 0;
    uint32_t i;

    for (i = 0; i < 3u; i++) {
        bits |= (uint8_t)((kvm_msr_allowed(v, MSR_FS_BASE + i, 0) ? 0u : 1u) << (2u * i));
        bits |= (uint8_t)((kvm_msr_allowed(v, MSR_FS_BASE + i, 1) ? 0u : 2u) << (2u * i));
    }
    *b = (uint8_t)((*b & ~0x3Fu) | bits);
}

/** Make the backend's share of `v`: the VMCB, the page of the host's
 * state, the MSR permission map and the host's FPU area. 0 or -ENOMEM. */
static int svm_vcpu_init(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = kzalloc(sizeof(*s));
    uintptr_t pa;

    if (!s) {
        return -ENOMEM;
    }
    v->backend = s;
    s->vmcb = kalloc_pages(1, &pa);
    s->vmcb_pa = pa;
    s->host = kalloc_pages(1, &pa);
    s->host_pa = pa;
    s->msrpm = kvm_map_pages(2, &s->msrpm_pa);
    s->host_fpu = fpu_area_alloc();
    if (!s->vmcb || !s->host || !s->msrpm || !s->host_fpu) {
        return -ENOMEM;
    }
    k_memset(s->vmcb, 0, 4096);
    k_memset(s->host, 0, 4096);
    svm_msr_filter_changed(v);
    s->cpu = -1;
    s->kgs_table = 1;
    return 0;
}

/** Free the backend's share of `v`. */
static void svm_vcpu_free(struct kvm_vcpu *v)
{
    struct svm_vcpu *s = sv(v);

    if (!s) {
        return;
    }
    if (s->vmcb) {
        kfree_pages(s->vmcb, 1);
    }
    if (s->host) {
        kfree_pages(s->host, 1);
    }
    if (s->msrpm) {
        kfree_pages(s->msrpm, 2);
    }
    fpu_area_free(s->host_fpu);
    kfree(s);
    v->backend = NULL;
}

/** The panic hook: GIF set again and SVM off on the processor that
 * panics, when a panic comes inside a run's section, so that the panic's
 * NMIs and a reboot find the processor as outside KVM, as Linux's
 * svm_emergency_disable_virtualization_cpu leaves it. */
static void svm_panic(void)
{
#ifndef RACH2_HOSTTEST
    int cpu = (int)arch_cpu_id();

    if (cpu < NR_CPUS && svm_live[cpu]) {
        if (svm_in_run[cpu]) {
            hw_stgi();
        }
        svm_live[cpu] = 0;
        msr_write(MSR_EFER, msr_read(MSR_EFER) & ~GEFER_SVME);
    }
#endif
}

/**
 * Whether the processor has SVM as this backend needs it and the firmware
 * left it on: SVM in leaf 0x80000001, VM_CR's SVMDIS clear, nested paging,
 * and at least two ASIDs. NRIPS, the flush by ASID and the TSC ratio are
 * taken where they are. 1 or 0.
 */
static int svm_probe(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0, lo = 0, hi = 0;

    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a < 0x8000000Au) {
        return 0;
    }
    kcpu_id(0x80000001u, &a, &b, &c, &d);
    if (!(c & (1u << 2))) {
        return 0;
    }
    k_memset(&caps, 0, sizeof(caps));
    caps.rdtscp = (d >> 27) & 1u;
    kcpu_rdmsr(MSR_VM_CR, &lo, &hi);
    if (lo & VM_CR_SVMDIS) {
        return 0;
    }
    kcpu_id(0x8000000Au, &a, &b, &c, &d);
    if (!(d & SVM_FEAT_NP) || b < 2u) {
        return 0;
    }
    caps.max_asid = b - 1u;
    caps.nrips = (d & SVM_FEAT_NRIPS) != 0;
    caps.flushbyasid = (d & SVM_FEAT_FLUSHBYASID) != 0;
    caps.tscrate = (d & SVM_FEAT_TSCRATE) != 0;
#ifndef RACH2_HOSTTEST
    caps.host_mce = hw_read_cr4() & CR4_MCE;
#endif
    k_panic_hook = svm_panic;
    return 1;
}

/** `kvm_backend`'s tsc_scaling: the TSC ratio MSR's largest value and its
 * 32 bits of fraction, or 0 on a processor without the MSR. */
static uint64_t svm_tsc_scaling(uint32_t *frac_bits)
{
    *frac_bits = TSC_RATIO_BITS;
    return caps.tscrate ? TSC_RATIO_MAX : 0;
}

/** The bits of a memory type in a 4 KiB leaf: PCD and PWT for UC, PWT for
 * WT, none for WB, with the guest's PAT combined with them. */
static uint64_t svm_cache(int type, int ignore_guest_pat)
{
    (void)ignore_guest_pat;
    return type == KVM_MT_UC ? 0x18u : 0u;
}

/* The long-mode format: P, R/W, U/S, PS, A, D and NX. */
static const struct kvm_tdp_format svm_tdp = {
    0x1u, 0x1u, 0x2u, 0, 1ull << 63, 0x4u, 0x80u, 0x000FFFFFFFFFF000ull, 0x20u, 0x40u, svm_cache};

const struct kvm_backend kvm_svm = {
    "svm",     svm_probe,       svm_vm_init,           NULL,     svm_vcpu_init, svm_vcpu_free,
    svm_enter, svm_exit,        svm_complete,          svm_save, svm_inject,    svm_check_extension,
    &svm_tdp,  svm_tsc_scaling, svm_msr_filter_changed};
