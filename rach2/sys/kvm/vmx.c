/*
 * vmx.c: Intel's VMX backend: VMX turned on around each guest run, the
 * VMCS built from the capability MSRs, EPT, VPID and the unrestricted
 * guest, the preemption timer bounding every run, the exits, and the
 * events injected. Every VMX instruction and every other touch of the
 * processor goes through `vmx_op`, one table of operations, which the host
 * test's model of the Lenovo's processor stands in for.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../fpu.h"
#include "../klog.h"
#include "../proc/smp.h"

/* ---- the processor's numbers (Intel SDM Vol. 3C, appendices A to C) ---- */

#define MSR_IA32_FEATURE_CONTROL 0x03Au
#define MSR_IA32_SYSENTER_CS     0x174u
#define MSR_IA32_SYSENTER_ESP    0x175u
#define MSR_IA32_SYSENTER_EIP    0x176u
#define MSR_IA32_DEBUGCTL        0x1D9u
#define MSR_IA32_CR_PAT          0x277u
#define MSR_IA32_VMX_BASIC       0x480u
#define MSR_IA32_VMX_PINBASED    0x481u
#define MSR_IA32_VMX_PROCBASED   0x482u
#define MSR_IA32_VMX_EXIT        0x483u
#define MSR_IA32_VMX_ENTRY       0x484u
#define MSR_IA32_VMX_MISC        0x485u
#define MSR_IA32_VMX_CR0_FIXED0  0x486u
#define MSR_IA32_VMX_CR0_FIXED1  0x487u
#define MSR_IA32_VMX_CR4_FIXED0  0x488u
#define MSR_IA32_VMX_CR4_FIXED1  0x489u
#define MSR_IA32_VMX_PROCBASED2  0x48Bu
#define MSR_IA32_VMX_EPT_VPID    0x48Cu
#define MSR_IA32_VMX_TRUE_PIN    0x48Du
#define MSR_IA32_VMX_TRUE_PROC   0x48Eu
#define MSR_IA32_VMX_TRUE_EXIT   0x48Fu
#define MSR_IA32_VMX_TRUE_ENTRY  0x490u
#define MSR_EFER                 0xC0000080u
#define MSR_STAR                 0xC0000081u
#define MSR_LSTAR                0xC0000082u
#define MSR_CSTAR                0xC0000083u
#define MSR_SYSCALL_MASK         0xC0000084u
#define MSR_FS_BASE              0xC0000100u
#define MSR_GS_BASE              0xC0000101u
#define MSR_KERNEL_GS_BASE       0xC0000102u
#define MSR_TSC_AUX              0xC0000103u
#define MSR_APIC_BASE            0x01Bu

/* IA32_FEATURE_CONTROL: its lock and the enable of VMX outside SMX. */
#define FC_LOCKED          0x1u
#define FC_VMX_OUTSIDE_SMX 0x4u

/* IA32_VMX_BASIC: the region's bytes, the true controls, the information
 * of INS and OUTS. */
#define BASIC_SIZE(b)  ((uint32_t)(((b) >> 32) & 0x1FFFu))
#define BASIC_TRUE     (1ull << 55)
#define BASIC_INS_OUTS (1ull << 54)
#define BASIC_32BIT    (1ull << 48)

/* IA32_VMX_EPT_VPID_CAP. */
#define EPT_CAP_WALK4        (1ull << 6)
#define EPT_CAP_WB           (1ull << 14)
#define EPT_CAP_2M           (1ull << 16)
#define EPT_CAP_INVEPT       (1ull << 20)
#define EPT_CAP_AD           (1ull << 21)
#define EPT_CAP_INVEPT_CTX   (1ull << 25)
#define EPT_CAP_INVEPT_ALL   (1ull << 26)
#define VPID_CAP_INVVPID     (1ull << 32)
#define VPID_CAP_INVVPID_CTX (1ull << 41)
#define VPID_CAP_INVVPID_ALL (1ull << 42)

/* The VMCS's fields. */
#define VIRTUAL_PROCESSOR_ID          0x0000u
#define GUEST_ES_SELECTOR             0x0800u
#define GUEST_TR_SELECTOR             0x080Eu
#define HOST_ES_SELECTOR              0x0C00u
#define HOST_CS_SELECTOR              0x0C02u
#define HOST_SS_SELECTOR              0x0C04u
#define HOST_DS_SELECTOR              0x0C06u
#define HOST_FS_SELECTOR              0x0C08u
#define HOST_GS_SELECTOR              0x0C0Au
#define HOST_TR_SELECTOR              0x0C0Cu
#define MSR_BITMAP                    0x2004u
#define VM_EXIT_MSR_STORE_ADDR        0x2006u
#define VM_EXIT_MSR_LOAD_ADDR         0x2008u
#define VM_ENTRY_MSR_LOAD_ADDR        0x200Au
#define TSC_OFFSET                    0x2010u
#define TSC_MULTIPLIER                0x2032u
#define XSS_EXIT_BITMAP               0x202Cu
#define EPT_POINTER                   0x201Au
#define GUEST_PHYSICAL_ADDRESS        0x2400u
#define VMCS_LINK_POINTER             0x2800u
#define GUEST_IA32_DEBUGCTL           0x2802u
#define GUEST_IA32_PERF_GLOBAL_CTRL   0x2808u
#define GUEST_IA32_PAT                0x2804u
#define GUEST_IA32_EFER               0x2806u
#define GUEST_PDPTR0                  0x280Au
#define HOST_IA32_PAT                 0x2C00u
#define HOST_IA32_EFER                0x2C02u
#define HOST_IA32_PERF_GLOBAL_CTRL    0x2C04u
#define PIN_BASED_VM_EXEC_CONTROL     0x4000u
#define CPU_BASED_VM_EXEC_CONTROL     0x4002u
#define EXCEPTION_BITMAP              0x4004u
#define PAGE_FAULT_ERROR_CODE_MASK    0x4006u
#define PAGE_FAULT_ERROR_CODE_MATCH   0x4008u
#define CR3_TARGET_COUNT              0x400Au
#define VM_EXIT_CONTROLS              0x400Cu
#define VM_EXIT_MSR_STORE_COUNT       0x400Eu
#define VM_EXIT_MSR_LOAD_COUNT        0x4010u
#define VM_ENTRY_CONTROLS             0x4012u
#define VM_ENTRY_MSR_LOAD_COUNT       0x4014u
#define VM_ENTRY_INTR_INFO_FIELD      0x4016u
#define VM_ENTRY_EXCEPTION_ERROR_CODE 0x4018u
#define VM_ENTRY_INSTRUCTION_LEN      0x401Au
#define SECONDARY_VM_EXEC_CONTROL     0x401Eu
#define VM_INSTRUCTION_ERROR          0x4400u
#define VM_EXIT_REASON                0x4402u
#define VM_EXIT_INTR_INFO             0x4404u
#define VM_EXIT_INTR_ERROR_CODE       0x4406u
#define IDT_VECTORING_INFO_FIELD      0x4408u
#define IDT_VECTORING_ERROR_CODE      0x440Au
#define VM_EXIT_INSTRUCTION_LEN       0x440Cu
#define VMX_INSTRUCTION_INFO          0x440Eu
#define GUEST_ES_LIMIT                0x4800u
#define GUEST_GDTR_LIMIT              0x4810u
#define GUEST_IDTR_LIMIT              0x4812u
#define GUEST_ES_AR_BYTES             0x4814u
#define GUEST_INTERRUPTIBILITY_INFO   0x4824u
#define GUEST_ACTIVITY_STATE          0x4826u
#define GUEST_SYSENTER_CS             0x482Au
#define VMX_PREEMPTION_TIMER_VALUE    0x482Eu
#define HOST_IA32_SYSENTER_CS         0x4C00u
#define CR0_GUEST_HOST_MASK           0x6000u
#define CR4_GUEST_HOST_MASK           0x6002u
#define CR0_READ_SHADOW               0x6004u
#define CR4_READ_SHADOW               0x6006u
#define EXIT_QUALIFICATION            0x6400u
#define GUEST_CR0                     0x6800u
#define GUEST_CR3                     0x6802u
#define GUEST_CR4                     0x6804u
#define GUEST_ES_BASE                 0x6806u
#define GUEST_GDTR_BASE               0x6816u
#define GUEST_IDTR_BASE               0x6818u
#define GUEST_DR7                     0x681Au
#define GUEST_RSP                     0x681Cu
#define GUEST_RIP                     0x681Eu
#define GUEST_RFLAGS                  0x6820u
#define GUEST_PENDING_DBG_EXCEPTIONS  0x6822u
#define GUEST_SYSENTER_ESP            0x6824u
#define GUEST_SYSENTER_EIP            0x6826u
#define HOST_CR0                      0x6C00u
#define HOST_CR3                      0x6C02u
#define HOST_CR4                      0x6C04u
#define HOST_FS_BASE                  0x6C06u
#define HOST_GS_BASE                  0x6C08u
#define HOST_TR_BASE                  0x6C0Au
#define HOST_GDTR_BASE                0x6C0Cu
#define HOST_IDTR_BASE                0x6C0Eu
#define HOST_IA32_SYSENTER_ESP        0x6C10u
#define HOST_IA32_SYSENTER_EIP        0x6C12u
#define HOST_RIP                      0x6C16u

/* The guest segment registers' fields are 2 apart in each group, in the
 * order ES, CS, SS, DS, FS, GS, LDTR, TR. */
#define SEG_ES   0
#define SEG_CS   1
#define SEG_SS   2
#define SEG_DS   3
#define SEG_FS   4
#define SEG_GS   5
#define SEG_LDTR 6
#define SEG_TR   7

/* The pin-based controls. */
#define PIN_EXT_INTR   0x01u
#define PIN_NMI        0x08u
#define PIN_VIRT_NMI   0x20u
#define PIN_PREEMPTION 0x40u

/* The primary processor-based controls. */
#define PROC_INTR_WINDOW 0x00000004u
#define PROC_TSC_OFFSET  0x00000008u
#define PROC_HLT         0x00000080u
#define PROC_INVLPG      0x00000200u
#define PROC_MWAIT       0x00000400u
#define PROC_RDPMC       0x00000800u
#define PROC_CR3_LOAD    0x00008000u
#define PROC_CR3_STORE   0x00010000u
#define PROC_CR8_LOAD    0x00080000u
#define PROC_CR8_STORE   0x00100000u
#define PROC_NMI_WINDOW  0x00400000u
#define PROC_MOV_DR      0x00800000u
#define PROC_UNCOND_IO   0x01000000u
#define PROC_MSR_BITMAP  0x10000000u
#define PROC_MONITOR     0x20000000u
#define PROC_SECONDARY   0x80000000u

/* The secondary controls. */
#define PROC2_EPT         0x00000002u
#define PROC2_RDTSCP      0x00000008u
#define PROC2_VPID        0x00000020u
#define PROC2_WBINVD      0x00000040u
#define PROC2_UG          0x00000080u
#define PROC2_INVPCID     0x00001000u
#define PROC2_XSAVES      0x00100000u
#define PROC2_TSC_SCALING 0x02000000u

/* The TSC multiplier, 16 bits of integer over 48 of fraction: 1. */
#define TSC_RATIO_BITS 48u
#define TSC_RATIO_ONE  (1ull << TSC_RATIO_BITS)

/* The exit controls. */
#define EXIT_SAVE_DEBUG 0x00000004u
#define EXIT_HOST_64    0x00000200u
#define EXIT_LOAD_PERF  0x00001000u
#define EXIT_LOAD_PAT   0x00080000u
#define EXIT_LOAD_EFER  0x00200000u

/* The entry controls. */
#define ENTRY_LOAD_DEBUG 0x00000004u
#define ENTRY_IA32E      0x00000200u
#define ENTRY_LOAD_PERF  0x00002000u
#define ENTRY_LOAD_PAT   0x00004000u
#define ENTRY_LOAD_EFER  0x00008000u

/* The interruption information of an exit, an IDT vectoring or an entry:
 * vector, type, error code valid, valid. */
#define INTR_VECTOR(i)   ((i) & 0xFFu)
#define INTR_TYPE(i)     (((i) >> 8) & 7u)
#define INTR_ERRCODE     0x800u
#define INTR_NMI_UNBLOCK 0x1000u
#define INTR_VALID       0x80000000u
#define TYPE_EXT_INTR    0u
#define TYPE_NMI         2u
#define TYPE_HW_EXC      3u
#define TYPE_SOFT_INTR   4u
#define TYPE_PRIV_SOFT   5u
#define TYPE_SOFT_EXC    6u

/* The interruptibility state's bits. */
#define INTR_STI       0x1u
#define INTR_MOV_SS    0x2u
#define INTR_BLOCK_NMI 0x8u

/* KVM's interrupt shadow, struct kvm_vcpu_events' `interrupt.shadow`. */
#define SHADOW_MOV_SS 0x1u
#define SHADOW_STI    0x2u

/* The basic exit reasons. */
#define EXIT_EXCEPTION_NMI    0
#define EXIT_EXTERNAL_INTR    1
#define EXIT_TRIPLE_FAULT     2
#define EXIT_INIT_SIGNAL      3
#define EXIT_SIPI             4
#define EXIT_INTR_WINDOW      7
#define EXIT_NMI_WINDOW       8
#define EXIT_TASK_SWITCH      9
#define EXIT_CPUID            10
#define EXIT_GETSEC           11
#define EXIT_HLT              12
#define EXIT_INVD             13
#define EXIT_INVLPG           14
#define EXIT_RDPMC            15
#define EXIT_RDTSC            16
#define EXIT_VMCALL           18
#define EXIT_VMCLEAR          19
#define EXIT_VMXON            27
#define EXIT_CR_ACCESS        28
#define EXIT_DR_ACCESS        29
#define EXIT_IO               30
#define EXIT_MSR_READ         31
#define EXIT_MSR_WRITE        32
#define EXIT_INVALID_STATE    33
#define EXIT_MSR_LOAD_FAIL    34
#define EXIT_MWAIT            36
#define EXIT_MONITOR          39
#define EXIT_PAUSE            40
#define EXIT_MCE_DURING_ENTRY 41
#define EXIT_EPT_VIOLATION    48
#define EXIT_EPT_MISCONFIG    49
#define EXIT_INVEPT           50
#define EXIT_PREEMPTION_TIMER 52
#define EXIT_INVVPID          53
#define EXIT_WBINVD           54
#define EXIT_XSETBV           55
#define EXIT_FAILED_ENTRY     0x80000000u

/* The VMX instructions' failures: VMfailInvalid with CF, VMfailValid with
 * ZF and the error in VM_INSTRUCTION_ERROR. */
#define VMX_OK           0
#define VMX_FAIL_INVALID 1
#define VMX_FAIL_VALID   2

/* INVEPT's and INVVPID's types. */
#define INV_SINGLE 1u
#define INV_ALL    2u

#define CR0_PE 0x00000001ull
#define CR0_MP 0x00000002ull
#define CR0_EM 0x00000004ull
#define CR0_TS 0x00000008ull
#define CR0_ET 0x00000010ull
#define CR0_NE 0x00000020ull
#define CR0_WP 0x00010000ull
#define CR0_NW 0x20000000ull
#define CR0_CD 0x40000000ull
#define CR0_PG 0x80000000ull

#define CR4_DE    (1ull << 3)
#define CR4_PSE   (1ull << 4)
#define CR4_PAE   (1ull << 5)
#define CR4_PGE   (1ull << 7)
#define CR4_VMXE  (1ull << 13)
#define CR4_PCIDE (1ull << 17)
#define CR4_SMEP  (1ull << 20)
#define CR4_SMAP  (1ull << 21)
#define CR4_PKE   (1ull << 22)

#define GEFER_SCE 0x001ull
#define GEFER_LME 0x100ull
#define GEFER_LMA 0x400ull
#define GEFER_NXE 0x800ull

#define RFLAGS_IF 0x200ull
#define RFLAGS_TF 0x100ull

#define DR6_BD      0x2000ull
#define DR6_BS      0x4000ull
#define DR6_FIXED   0xFFFF0FF0ull
#define DR7_GD      0x2000ull
#define DR7_FIXED   0x400ull
#define DR7_ENABLES 0xFFull

#define DEBUGCTL_LBR 0x1ull

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

/* The exceptions that always exit, as Linux's vmx_update_exception_bitmap
 * has them with EPT: #DB and #AC, so that a guest cannot hold the
 * processor in an endless loop of either (CVE-2015-8104, CVE-2015-5307);
 * #UD, for the instructions emulate.c runs after one; and #MC, which is
 * the host's. */
#define EXC_ALWAYS ((1u << DB_VECTOR) | (1u << UD_VECTOR) | (1u << AC_VECTOR) | (1u << MC_VECTOR))

/* The guest's run, bounded by the preemption timer: about 4 million TSC
 * cycles, 2 ms at the Lenovo's 1.9 GHz, past the host's 1 ms tick, which
 * ends a run first unless something keeps it from the processor. */
#define VMX_RUN_BOUND_TSC (1u << 22)

/* The host's selectors in the VMCS: the kernel's code, data and TSS
 * (arch/x86_64/percpu.h); DS, ES, FS and GS are 0, which the entry checks
 * need, and get their values back after the exit. */
#define HOST_CS 0x10u
#define HOST_SS 0x18u
#define HOST_TR 0x40u

/* The guest MSRs the entry loads and the exit stores, and the host's the
 * exit loads, as 16-byte entries of the MSR area's page: the guest's at 0,
 * the host's at 2048. KERNEL_GS_BASE changes in the guest by SWAPGS
 * without an exit, so the exit stores it; the others change only by a
 * WRMSR that exits. The host's KERNEL_GS_BASE is put back by
 * `restore_host`. TSC_AUX is last, and only where the host has RDTSCP. */
static const uint32_t guest_msrs[] = {MSR_STAR,         MSR_LSTAR,          MSR_CSTAR,
                                      MSR_SYSCALL_MASK, MSR_KERNEL_GS_BASE, MSR_TSC_AUX};
static const uint32_t host_msrs[] = {MSR_STAR, MSR_LSTAR, MSR_CSTAR, MSR_SYSCALL_MASK, MSR_TSC_AUX};
#define N_GUEST_MSRS 6u
#define N_HOST_MSRS  5u
#define KGS_SLOT     4u
#define HOST_LIST    128u /* the host list's first word, in 16-byte entries: 2048 bytes */

/* ---- the table of operations ---- */

/*
 * The host's state that a VM exit changes and the backend puts back: the
 * data and extra segment selectors, which the VMCS's host fields leave 0;
 * the FS and GS selectors with their bases; the GDT and IDT registers,
 * whose limits the exit sets to 0xFFFF; the LDT, which the exit leaves
 * none; and the task register.
 */
struct vmx_host {
    uint16_t ds, es, fs, gs, ldt, tr;
    uint16_t gdt_limit, idt_limit;
    uint64_t gdt_base, idt_base;
    uint64_t fs_base, gs_base, kgs_base;
};

/*
 * Every touch of the processor the backend makes. The VMX instructions
 * return VMX_OK, VMX_FAIL_INVALID or VMX_FAIL_VALID, as their flags say;
 * `run` enters the guest from `regs` and its CR2, VMLAUNCH or VMRESUME as
 * `launched` says, and returns VMX_OK at the exit, with the guest's
 * registers saved, or the entry instruction's failure. `fpu_load` saves the
 * host's state into `host` and loads the guest's image with XCR0 `xcr0`;
 * `fpu_store` does the reverse. `nmi` and `machine_check` raise the host's
 * own vector 2 or 18, for an exit that took one from the host.
 */
struct vmx_ops {
    int (*vmxon)(uint64_t *pa);
    int (*vmxoff)(void);
    int (*vmclear)(uint64_t *pa);
    int (*vmptrld)(uint64_t *pa);
    int (*vmread)(uint64_t field, uint64_t *value);
    int (*vmwrite)(uint64_t field, uint64_t value);
    int (*invept)(uint64_t type, const uint64_t *desc);
    int (*invvpid)(uint64_t type, const uint64_t *desc);
    int (*run)(struct kvm_regs *regs, uint64_t *cr2, int launched);
    uint64_t (*rdmsr)(uint32_t msr);
    void (*wrmsr)(uint32_t msr, uint64_t value);
    uint64_t (*read_cr)(int n);
    void (*write_cr4)(uint64_t value);
    uint64_t (*read_dr)(int n);
    void (*write_dr)(int n, uint64_t value);
    void (*save_host)(struct vmx_host *h);
    void (*restore_host)(const struct vmx_host *h);
    void (*fpu_load)(void *host, const void *guest, uint64_t xcr0);
    void (*fpu_store)(void *host, void *guest, uint64_t xcr0);
    void (*nmi)(void);
    void (*machine_check)(void);
    void (*wbinvd)(void);
    uint64_t (*exit_rip)(void);
};

#include "../arch/x86/kcpu.h"

#ifndef RACH2_HOSTTEST
#include "../arch/x86_64/percpu.h"

/* vmx_run.s */
int kvm_vmx_vmxon(uint64_t *pa);
int kvm_vmx_vmxoff(void);
int kvm_vmx_vmclear(uint64_t *pa);
int kvm_vmx_vmptrld(uint64_t *pa);
int kvm_vmx_vmread(uint64_t field, uint64_t *value);
int kvm_vmx_vmwrite(uint64_t field, uint64_t value);
int kvm_vmx_invept(uint64_t type, const uint64_t *desc);
int kvm_vmx_invvpid(uint64_t type, const uint64_t *desc);
int kvm_vmx_vmrun(struct kvm_regs *regs, uint64_t *cr2, int launched);
void kvm_vmx_exit(void);

/** CR0, CR3 or CR4, by `n`. The control registers as bytes: TinyCC's
 * assembler names none of them in 64-bit code. */
static uint64_t hw_read_cr(int n)
{
    uint64_t v;

    if (n == 0) {
        __asm__ __volatile__(".byte 0x0f, 0x20, 0xc0" : "=a"(v));
    } else if (n == 3) {
        __asm__ __volatile__(".byte 0x0f, 0x20, 0xd8" : "=a"(v));
    } else {
        __asm__ __volatile__(".byte 0x0f, 0x20, 0xe0" : "=a"(v));
    }
    return v;
}

/** Write CR4. */
static void hw_write_cr4(uint64_t v)
{
    __asm__ __volatile__(".byte 0x0f, 0x22, 0xe0" : : "a"(v) : "memory");
}

/** Debug register `n`, 0 to 3, 6 or 7: `mov %drN, %rax` is 0F 21 /r. */
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
    case 6:
        __asm__ __volatile__(".byte 0x0f, 0x21, 0xf0" : "=a"(v));
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
    case 6:
        __asm__ __volatile__(".byte 0x0f, 0x23, 0xf0" : : "a"(v));
        break;
    default:
        __asm__ __volatile__(".byte 0x0f, 0x23, 0xf8" : : "a"(v));
        break;
    }
}

/* The 10-byte operand of SGDT, SIDT, LGDT and LIDT. */
struct desc_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/** The host's selectors and descriptor tables before an entry: SGDT,
 * SIDT, SLDT and STR as bytes, and the segment bases. */
static void hw_save_host(struct vmx_host *h)
{
    struct desc_ptr d;
    uint64_t sel;

    __asm__ __volatile__(".byte 0x0f, 0x01, 0x07" : : "D"(&d) : "memory");
    h->gdt_limit = d.limit;
    h->gdt_base = d.base;
    __asm__ __volatile__(".byte 0x0f, 0x01, 0x0f" : : "D"(&d) : "memory");
    h->idt_limit = d.limit;
    h->idt_base = d.base;
    __asm__ __volatile__("xorl %%eax, %%eax; .byte 0x0f, 0x00, 0xc0" : "=a"(sel));
    h->ldt = (uint16_t)sel;
    __asm__ __volatile__("xorl %%eax, %%eax; .byte 0x0f, 0x00, 0xc8" : "=a"(sel));
    h->tr = (uint16_t)sel;
    h->ds = (uint16_t)x86_64_ds_read();
    h->es = (uint16_t)x86_64_es_read();
    h->fs = (uint16_t)x86_64_fs_read();
    h->gs = (uint16_t)x86_64_gs_read();
    h->fs_base = msr_read(MSR_FS_BASE);
    h->gs_base = msr_read(MSR_GS_BASE);
    h->kgs_base = msr_read(MSR_KERNEL_GS_BASE);
}

/** Put back what the exit changed of the host's state, as Linux's
 * vmx_prepare_switch_to_host does: the tables' limits, the LDT, and the
 * selectors with their bases. */
static void hw_restore_host(const struct vmx_host *h)
{
    struct desc_ptr d;

    d.limit = h->gdt_limit;
    d.base = h->gdt_base;
    __asm__ __volatile__(".byte 0x0f, 0x01, 0x17" : : "D"(&d) : "memory");
    d.limit = h->idt_limit;
    d.base = h->idt_base;
    __asm__ __volatile__(".byte 0x0f, 0x01, 0x1f" : : "D"(&d) : "memory");
    if (h->ldt) {
        x86_64_lldt(h->ldt);
    }
    if (h->ds) {
        x86_64_ds_load(h->ds);
    }
    if (h->es) {
        x86_64_es_load(h->es);
    }
    if (h->fs) {
        x86_64_fs_load(h->fs);
        msr_write(MSR_FS_BASE, h->fs_base);
    }
    if (h->gs) {
        x86_64_gs_load(h->gs);
    }
    msr_write(MSR_KERNEL_GS_BASE, h->kgs_base);
}

/* `xsave64 (%rdi)`, `xrstor64 (%rdi)` and `xsetbv` as bytes, as fpu.c
 * writes them. */
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

/* The host's XCR0 while a guest's is loaded, or 0 without XSAVE. */
static uint64_t host_xcr0[NR_CPUS];

/** Save the host's floating-point state into `host` and load the guest's
 * image `guest` before setting its XCR0. */
static void hw_fpu_load(void *host, const void *guest, uint64_t xcr0)
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
static void hw_fpu_store(void *host, void *guest, uint64_t xcr0)
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

/** The host's NMI handler, for an NMI the guest's run took: INT 2. */
static void hw_nmi(void)
{
    __asm__ __volatile__(".byte 0xcd, 0x02");
}

/** The host's machine-check handler: INT 18. */
static void hw_machine_check(void)
{
    __asm__ __volatile__(".byte 0xcd, 0x12");
}

/** WBINVD. */
static void hw_wbinvd(void)
{
    __asm__ __volatile__(".byte 0x0f, 0x09" : : : "memory");
}

/** The address the exit resumes the host at, vmx_run.s's. */
static uint64_t hw_exit_rip(void)
{
    return (uint64_t)(uintptr_t)kvm_vmx_exit;
}

static const struct vmx_ops vmx_hw = {
    kvm_vmx_vmxon,    kvm_vmx_vmxoff,  kvm_vmx_vmclear, kvm_vmx_vmptrld, kvm_vmx_vmread,
    kvm_vmx_vmwrite,  kvm_vmx_invept,  kvm_vmx_invvpid, kvm_vmx_vmrun,   msr_read,
    msr_write,        hw_read_cr,      hw_write_cr4,    hw_read_dr,      hw_write_dr,
    hw_save_host,     hw_restore_host, hw_fpu_load,     hw_fpu_store,    hw_nmi,
    hw_machine_check, hw_wbinvd,       hw_exit_rip};

static const struct vmx_ops *vmx_op = &vmx_hw;

/** The host's per-processor bases: its GS base, TSS and GDT. */
static void host_cpu_bases(int cpu, uint64_t *gs, uint64_t *tr, uint64_t *gdt)
{
    *gs = (uint64_t)(uintptr_t)&cpus[cpu];
    *tr = (uint64_t)(uintptr_t)&cpus[cpu].tss;
    *gdt = (uint64_t)(uintptr_t)cpus[cpu].gdt;
}
#else
/* The host test's model stands in for the processor. */
static const struct vmx_ops *vmx_op;
static void host_cpu_bases(int cpu, uint64_t *gs, uint64_t *tr, uint64_t *gdt);
#endif

/* panic.h's hook, declared alone: its console headers name a global `x`. */
extern void (*k_panic_hook)(void);

/* ---- what the probe found ---- */

/*
 * The controls and capabilities of the processor, read once at the probe
 * from the capability MSRs, the same on every processor.
 */
static struct {
    uint32_t revision; /* the VMCS revision identifier */
    uint32_t pin, proc, proc2, exit, entry;
    uint64_t cr0_fixed0, cr0_fixed1, cr4_fixed0, cr4_fixed1;
    uint64_t ept_vpid;
    uint32_t timer_shift; /* the preemption timer counts the TSC >> this */
    int ins_outs;         /* the exit describes INS and OUTS */
    int ad;               /* EPT's accessed and dirty bits */
    int rdtscp;           /* the host has RDTSCP, and so TSC_AUX */
    int perf_global;      /* the controls that load IA32_PERF_GLOBAL_CTRL */
} caps;

/* Each processor's VMXON region, made at the first VM, and whether its
 * IA32_FEATURE_CONTROL is checked. */
static void *vmxon_region[NR_CPUS];
static uint64_t vmxon_pa[NR_CPUS];
static int fc_checked[NR_CPUS];

/* The VPIDs in use, 1 to 65535, a bit each; 0 is the host's. */
static uint8_t vpid_used[8192];

/* ---- a vCPU's share ---- */

/* The parts of the VMCS this file changes itself, apart from the vCPU's
 * KVM_PART_* bits, for the next entry to write. */
#define VX_CR0   0x01u
#define VX_CR4   0x02u
#define VX_EFER  0x04u
#define VX_PDPTE 0x08u
#define VX_INTR  0x10u /* the interruptibility state */
#define VX_ALL   0x1Fu

/* One exit, read while the VMCS is loaded. */
struct vmx_exit {
    uint32_t reason;
    uint64_t qual;
    uint64_t gpa;
    uint32_t intr_info, intr_error;
    uint32_t idt_info, idt_error;
    uint32_t insn_len, insn_info;
    uint32_t interruptibility;
    uint32_t insn_error; /* VM_INSTRUCTION_ERROR of a failed entry */
    int failed;          /* VMLAUNCH failed: VMX_FAIL_* */
};

struct vmx_vcpu {
    void *vmcs;
    uint64_t vmcs_pa;
    uint64_t *msrs; /* the MSR area: the guest's list, then at HOST_LIST the host's */
    uintptr_t msrs_pa;
    uint8_t *msr_bitmap; /* every MSR exits but those `vmx_msr_filter_changed` passes */
    uintptr_t msr_bitmap_pa;
    void *host_fpu;
    uint16_t vpid;
    int built;       /* the VMCS's lasting fields are written */
    int last_cpu;    /* the processor whose host fields the VMCS holds, or -1 */
    int fresh;       /* the record's special registers are the VMCS's */
    int kgs_table;   /* msr.c's KERNEL_GS_BASE is the guest's, after `save` */
    int flush_vpid;  /* drop the guest's linear translations at the entry */
    uint32_t vdirty; /* VX_* */
    uint32_t proc, proc2, entry, exc_bitmap; /* the controls as the VMCS has them */
    uint32_t proc_want;                      /* the primary controls for the next entry */
    uint32_t inj_info, inj_error, inj_len;   /* the event the next entry injects */
    uint64_t pdpte[4];
    uint64_t debugctl; /* GUEST_IA32_DEBUGCTL as the VMCS has it */
    int guest_dr;      /* DR0 to DR3 were loaded for the run */
    int tf_set;        /* the entry set RFLAGS.TF for KVM_GUESTDBG_SINGLESTEP */
    struct vmx_exit ex;
};

/** The backend's share of `v`. */
static struct vmx_vcpu *vx(const struct kvm_vcpu *v)
{
    return (struct vmx_vcpu *)v->backend;
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

/* ---- the VMCS's fields ---- */

/** VMREAD `field`: its value, 0 when the instruction fails. */
static uint64_t vmcs_read(uint32_t field)
{
    uint64_t value = 0;

    if (vmx_op->vmread(field, &value) != VMX_OK) {
        return 0;
    }
    return value;
}

/* The first VMWRITE, INVEPT or INVVPID that failed, which `vmx_exit`
 * reports once: its name, its field or type, and VM_INSTRUCTION_ERROR.
 * Each fails only on an operand this file chose wrong. */
static const char *failed_insn;
static uint64_t failed_arg;
static uint32_t failed_error;
static int failed_told;

/** Keep the failure of the instruction `insn` on `arg` for `vmx_exit` to
 * report, since the instructions run with interrupts off; the run goes on,
 * as Linux's vmwrite_error, invept_error and invvpid_error warn once and
 * go on. */
static void insn_failed(const char *insn, uint64_t arg)
{
    if (!failed_insn) {
        failed_insn = insn;
        failed_arg = arg;
        failed_error = (uint32_t)vmcs_read(VM_INSTRUCTION_ERROR);
    }
}

/** VMWRITE `value` into `field`. A failure leaves the field as it was. */
static void vmcs_write(uint32_t field, uint64_t value)
{
    if (vmx_op->vmwrite(field, value) != VMX_OK) {
        insn_failed("vmwrite", field);
    }
}

/** The access rights of the segment `s`, as the VMCS keeps them: unusable
 * for a segment not present, as Linux's vmx_segment_access_rights gives
 * it, and accessed for any but the LDT, which the unrestricted guest's
 * entry needs, as Linux's __vmx_set_segment sets it. */
static uint32_t seg_ar(const struct kvm_segment *s, int ldt)
{
    uint32_t ar, type = s->type;

    if (s->unusable || !s->present) {
        return 0x10000u;
    }
    if (!ldt) {
        type |= 1u;
    }
    ar = (type & 15u) | ((uint32_t)(s->s & 1u) << 4) | ((uint32_t)(s->dpl & 3u) << 5) | (1u << 7) |
         ((uint32_t)(s->avl & 1u) << 12) | ((uint32_t)(s->l & 1u) << 13) |
         ((uint32_t)(s->db & 1u) << 14) | ((uint32_t)(s->g & 1u) << 15);
    return ar;
}

/** Write the segment `s` into the guest's segment register `n` (SEG_*). */
static void seg_write(int n, const struct kvm_segment *s)
{
    vmcs_write(GUEST_ES_SELECTOR + 2u * (uint32_t)n, s->selector);
    vmcs_write(GUEST_ES_BASE + 2u * (uint32_t)n, s->base);
    vmcs_write(GUEST_ES_LIMIT + 2u * (uint32_t)n, s->limit);
    vmcs_write(GUEST_ES_AR_BYTES + 2u * (uint32_t)n, seg_ar(s, n == SEG_LDTR));
}

/** Read the guest's segment register `n` into `s`, as Linux's
 * vmx_get_segment: a segment is present when it is usable. */
static void seg_read(int n, struct kvm_segment *s)
{
    uint32_t ar = (uint32_t)vmcs_read(GUEST_ES_AR_BYTES + 2u * (uint32_t)n);

    s->selector = (uint16_t)vmcs_read(GUEST_ES_SELECTOR + 2u * (uint32_t)n);
    s->base = vmcs_read(GUEST_ES_BASE + 2u * (uint32_t)n);
    s->limit = (uint32_t)vmcs_read(GUEST_ES_LIMIT + 2u * (uint32_t)n);
    s->unusable = (uint8_t)((ar >> 16) & 1u);
    s->type = (uint8_t)(ar & 15u);
    s->s = (uint8_t)((ar >> 4) & 1u);
    s->dpl = (uint8_t)((ar >> 5) & 3u);
    s->present = (uint8_t)!s->unusable;
    s->avl = (uint8_t)((ar >> 12) & 1u);
    s->l = (uint8_t)((ar >> 13) & 1u);
    s->db = (uint8_t)((ar >> 14) & 1u);
    s->g = (uint8_t)((ar >> 15) & 1u);
    s->padding = 0;
}

/** The segment registers of `v`'s record, by SEG_*. */
static struct kvm_segment *seg_of(struct kvm_vcpu *v, int n)
{
    switch (n) {
    case SEG_ES:
        return &v->sregs.es;
    case SEG_CS:
        return &v->sregs.cs;
    case SEG_SS:
        return &v->sregs.ss;
    case SEG_DS:
        return &v->sregs.ds;
    case SEG_FS:
        return &v->sregs.fs;
    case SEG_GS:
        return &v->sregs.gs;
    case SEG_LDTR:
        return &v->sregs.ldt;
    default:
        return &v->sregs.tr;
    }
}

/* The CR0 bits the guest changes without an exit: TS and WP, as Linux's
 * KVM_POSSIBLE_CR0_GUEST_BITS under EPT. Every CR4 bit exits. */
#define CR0_GUEST_OWNED (CR0_TS | CR0_WP)

/** CR0 as the processor runs the guest under it: never CD or NW, whose
 * effect EPT's memory types give, NE always, and the fixed bits but PE and
 * PG, which the unrestricted guest may clear, as Linux's vmx_set_cr0. */
static uint64_t hw_cr0(uint64_t cr0)
{
    uint64_t fixed0 = caps.cr0_fixed0 & ~(CR0_PE | CR0_PG);

    return ((cr0 & ~(CR0_CD | CR0_NW)) | CR0_NE | fixed0) & caps.cr0_fixed1;
}

/** CR4 as the processor runs the guest under it: VMXE and the fixed bits,
 * and, with paging off, neither SMEP, SMAP nor PKE, which act only on
 * paging, as Linux's vmx_set_cr4. */
static uint64_t hw_cr4(uint64_t cr4, uint64_t cr0)
{
    uint64_t c = (cr4 | caps.cr4_fixed0) & caps.cr4_fixed1;

    if (!(cr0 & CR0_PG)) {
        c &= ~(CR4_SMEP | CR4_SMAP | CR4_PKE);
    }
    return c;
}

/** Whether `v` pages with PAE's four PDPTEs: PG and PAE, not long mode. */
static int pae_paging(const struct kvm_vcpu *v)
{
    return (v->sregs.cr0 & CR0_PG) && (v->sregs.cr4 & CR4_PAE) && !(v->sregs.efer & GEFER_LMA);
}

/**
 * Read `v`'s four PDPTEs from guest memory at CR3, as the processor loads
 * them for PAE paging and Linux's load_pdptrs: 0, or -1 for a PDPTE with a
 * reserved bit or a table in no slot, which the guest's write takes as #GP.
 */
static int pdpte_load(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint64_t p[4];
    int i;

    if (kvm_guest_pdptes(v, v->sregs.cr3, p) != 0) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        x->pdpte[i] = p[i];
    }
    x->vdirty |= VX_PDPTE;
    return 0;
}

/* ---- VMX on and off ---- */

/* Whether each processor is in VMX operation, and its CR4 from before,
 * for the panic hook. A processor's flag is set after its VMXON succeeds
 * and cleared before its VMXOFF, so the hook never runs VMXOFF outside VMX
 * operation, which is #UD. */
static volatile int vmx_live[NR_CPUS];
static uint64_t vmx_cr4[NR_CPUS];
static uint64_t *vmx_vmcs[NR_CPUS]; /* the VMCS current there, or NULL */

/** Leave VMX operation on `cpu` and put CR4 back to `cr4`. */
static void vmx_leave(int cpu, uint64_t cr4)
{
    vmx_live[cpu] = 0;
    vmx_vmcs[cpu] = NULL;
    (void)vmx_op->vmxoff();
    vmx_op->write_cr4(cr4);
}

/** The panic hook: VMX off on the processor that panics, when a panic
 * comes inside a run's section, from an NMI or a machine check taken in
 * VMX root operation or from a fault there; the current VMCS is cleared
 * first, as Linux's vmx_emergency_disable_virtualization_cpu does. */
static void vmx_panic(void)
{
    int cpu = (int)arch_cpu_id();

    if (cpu < NR_CPUS && vmx_live[cpu]) {
        if (vmx_vmcs[cpu]) {
            (void)vmx_op->vmclear(vmx_vmcs[cpu]);
        }
        vmx_leave(cpu, vmx_cr4[cpu]);
    }
}

/** Whether `cr` has every bit `fixed0` sets and none `fixed1` clears. */
static int fixed_ok(uint64_t cr, uint64_t fixed0, uint64_t fixed1)
{
    return (cr & fixed0) == fixed0 && (cr & ~fixed1) == 0;
}

/**
 * Turn VMX on on this processor and make `x`'s VMCS current, with
 * interrupts off: CR4.VMXE, IA32_FEATURE_CONTROL locked with VMX on where
 * the firmware left it unlocked, VMXON, and VMPTRLD. 0, or -EIO when an
 * instruction fails, with VMX left off; `*cr4` is CR4 before.
 */
static int vmx_on(struct vmx_vcpu *x, int cpu, uint64_t *cr4)
{
    uint64_t fc;

    if (cpu >= NR_CPUS || !vmxon_region[cpu]) {
        return -EIO;
    }
    if (!fc_checked[cpu]) {
        fc = vmx_op->rdmsr(MSR_IA32_FEATURE_CONTROL);
        if (!(fc & FC_LOCKED)) {
            vmx_op->wrmsr(MSR_IA32_FEATURE_CONTROL, fc | FC_LOCKED | FC_VMX_OUTSIDE_SMX);
        } else if (!(fc & FC_VMX_OUTSIDE_SMX)) {
            return -EIO;
        }
        fc_checked[cpu] = 1;
    }
    *cr4 = vmx_op->read_cr(4);
    /* CR0 or CR4 outside the fixed bits makes VMXON #GP, which Linux's
     * kvm_cpu_vmxon catches from its exception table; refuse it before. */
    if (!fixed_ok(vmx_op->read_cr(0), caps.cr0_fixed0, caps.cr0_fixed1) ||
        !fixed_ok(*cr4 | CR4_VMXE, caps.cr4_fixed0, caps.cr4_fixed1)) {
        return -EIO;
    }
    vmx_cr4[cpu] = *cr4;
    if (!(*cr4 & CR4_VMXE)) {
        vmx_op->write_cr4(*cr4 | CR4_VMXE);
    }
    if (vmx_op->vmxon(&vmxon_pa[cpu]) != VMX_OK) {
        vmx_op->write_cr4(*cr4);
        return -EIO;
    }
    vmx_live[cpu] = 1;
    if ((!x->built && vmx_op->vmclear(&x->vmcs_pa) != VMX_OK) ||
        vmx_op->vmptrld(&x->vmcs_pa) != VMX_OK) {
        vmx_leave(cpu, *cr4);
        return -EIO;
    }
    vmx_vmcs[cpu] = &x->vmcs_pa;
    return 0;
}

/**
 * Write `x`'s VMCS back to memory and turn VMX off on `cpu`, with
 * interrupts off: VMCLEAR, so that the next entry, on whichever processor,
 * loads it anew, then VMXOFF and CR4 as it was. Every path out of
 * `vmx_on`'s section ends here, so VMX is never on outside it, and an INIT
 * after it finds the processor out of VMX operation.
 */
static void vmx_off(struct vmx_vcpu *x, int cpu, uint64_t cr4)
{
    vmx_vmcs[cpu] = NULL;
    (void)vmx_op->vmclear(&x->vmcs_pa);
    vmx_leave(cpu, cr4);
}

/* ---- the VMCS built ---- */

/** Whether the VMCS switches IA32_PERF_GLOBAL_CTRL, as Linux's
 * add_atomic_switch_msr does with the processor's controls; without them
 * the MSR lists do. */
static int perf_switched(void)
{
    return caps.perf_global && kvm_pmu_global();
}

/** Write the VMCS fields that last for the vCPU's life: the controls, the
 * MSR lists and bitmap, the host's constant state, and the guest's state
 * no part of the record holds. */
static void vmcs_build(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint32_t n = caps.rdtscp ? N_GUEST_MSRS : N_GUEST_MSRS - 1u;

    if (caps.proc2 & PROC2_VPID) {
        vmcs_write(VIRTUAL_PROCESSOR_ID, x->vpid);
    }
    vmcs_write(PIN_BASED_VM_EXEC_CONTROL, caps.pin);
    x->proc = caps.proc & ~(v->hlt_in_guest ? PROC_HLT : 0u);
    vmcs_write(CPU_BASED_VM_EXEC_CONTROL, x->proc);
    x->proc2 = caps.proc2 & ~(PROC2_RDTSCP | PROC2_INVPCID | PROC2_XSAVES);
    vmcs_write(SECONDARY_VM_EXEC_CONTROL, x->proc2);
    if (caps.proc2 & PROC2_XSAVES) {
        /* No component of IA32_XSS makes XSAVES or XRSTORS exit. */
        vmcs_write(XSS_EXIT_BITMAP, 0);
    }
    vmcs_write(VM_EXIT_CONTROLS, caps.exit | (perf_switched() ? EXIT_LOAD_PERF : 0u));
    x->entry = (caps.entry & ~ENTRY_IA32E) | (perf_switched() ? ENTRY_LOAD_PERF : 0u);
    vmcs_write(VM_ENTRY_CONTROLS, x->entry);
    x->exc_bitmap = EXC_ALWAYS;
    vmcs_write(EXCEPTION_BITMAP, x->exc_bitmap);
    vmcs_write(PAGE_FAULT_ERROR_CODE_MASK, 0);
    vmcs_write(PAGE_FAULT_ERROR_CODE_MATCH, 0);
    vmcs_write(CR3_TARGET_COUNT, 0);
    vmcs_write(MSR_BITMAP, x->msr_bitmap_pa);
    vmcs_write(VM_ENTRY_MSR_LOAD_ADDR, x->msrs_pa);
    vmcs_write(VM_ENTRY_MSR_LOAD_COUNT, n);
    vmcs_write(VM_EXIT_MSR_STORE_ADDR, x->msrs_pa);
    vmcs_write(VM_EXIT_MSR_STORE_COUNT, n);
    vmcs_write(VM_EXIT_MSR_LOAD_ADDR, x->msrs_pa + HOST_LIST * 16u);
    vmcs_write(VM_EXIT_MSR_LOAD_COUNT, caps.rdtscp ? N_HOST_MSRS : N_HOST_MSRS - 1u);
    vmcs_write(VMCS_LINK_POINTER, ~0ull);
    vmcs_write(CR0_GUEST_HOST_MASK, ~CR0_GUEST_OWNED);
    vmcs_write(CR4_GUEST_HOST_MASK, ~0ull);
    vmcs_write(HOST_CS_SELECTOR, HOST_CS);
    vmcs_write(HOST_SS_SELECTOR, HOST_SS);
    vmcs_write(HOST_DS_SELECTOR, 0);
    vmcs_write(HOST_ES_SELECTOR, 0);
    vmcs_write(HOST_FS_SELECTOR, 0);
    vmcs_write(HOST_GS_SELECTOR, 0);
    vmcs_write(HOST_TR_SELECTOR, HOST_TR);
    vmcs_write(HOST_RIP, vmx_op->exit_rip());
    vmcs_write(GUEST_ACTIVITY_STATE, 0);
    vmcs_write(GUEST_PENDING_DBG_EXCEPTIONS, 0);
    x->debugctl = kvm_pmu_vmcs_debugctl(v);
    vmcs_write(GUEST_IA32_DEBUGCTL, x->debugctl);
    x->built = 1;
    x->vdirty = VX_ALL;
}

/** Write the host's fields of the processor `cpu` the run is on: its
 * GS base, TSS and GDT, its CR0, its IDT, the SYSENTER MSRs, PAT and EFER,
 * and the MSRs the exit loads. */
static void host_cpu_fields(struct vmx_vcpu *x, int cpu, const struct vmx_host *h)
{
    uint64_t gs, tr, gdt;
    uint32_t i;

    host_cpu_bases(cpu, &gs, &tr, &gdt);
    vmcs_write(HOST_GS_BASE, gs);
    vmcs_write(HOST_TR_BASE, tr);
    vmcs_write(HOST_GDTR_BASE, gdt);
    vmcs_write(HOST_IDTR_BASE, h->idt_base);
    vmcs_write(HOST_CR0, vmx_op->read_cr(0));
    vmcs_write(HOST_IA32_SYSENTER_CS, vmx_op->rdmsr(MSR_IA32_SYSENTER_CS));
    vmcs_write(HOST_IA32_SYSENTER_ESP, vmx_op->rdmsr(MSR_IA32_SYSENTER_ESP));
    vmcs_write(HOST_IA32_SYSENTER_EIP, vmx_op->rdmsr(MSR_IA32_SYSENTER_EIP));
    vmcs_write(HOST_IA32_PAT, vmx_op->rdmsr(MSR_IA32_CR_PAT));
    vmcs_write(HOST_IA32_EFER, vmx_op->rdmsr(MSR_EFER));
    for (i = 0; i < N_HOST_MSRS; i++) {
        x->msrs[(HOST_LIST + i) * 2u] = host_msrs[i];
        if (host_msrs[i] != MSR_TSC_AUX || caps.rdtscp) {
            x->msrs[(HOST_LIST + i) * 2u + 1u] = vmx_op->rdmsr(host_msrs[i]);
        }
    }
    x->last_cpu = cpu;
}

/** The value of msr.c's MSR `index` of `v`, 0 when it keeps none. */
static uint64_t table_msr(struct kvm_vcpu *v, uint32_t index)
{
    uint64_t value = 0;

    if (kvm_msr_get(v, index, &value, 1) != 0) {
        return 0;
    }
    return value;
}

/** The secondary controls `v`'s CPUID allows: RDTSCP, INVPCID and
 * XSAVES only where it offers them, XSAVES with XSAVE, since without the
 * control each is #UD, as Linux's vmx_secondary_exec_control. */
static uint32_t proc2_for(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e, *d1;
    uint32_t p = caps.proc2 & ~(PROC2_RDTSCP | PROC2_INVPCID | PROC2_XSAVES);

    e = kvm_cpuid_find(v, 0x80000001u, 0);
    if (e && ((e->edx >> 27) & 1u)) {
        p |= caps.proc2 & PROC2_RDTSCP;
    }
    e = kvm_cpuid_find(v, 7, 0);
    if (e && ((e->ebx >> 10) & 1u)) {
        p |= caps.proc2 & PROC2_INVPCID;
    }
    e = kvm_cpuid_find(v, 1, 0);
    d1 = kvm_cpuid_find(v, 0xD, 1);
    if (e && ((e->ecx >> 26) & 1u) && d1 && ((d1->eax >> 3) & 1u)) {
        p |= caps.proc2 & PROC2_XSAVES;
    }
    return p;
}

/** Write `v`'s interrupt shadow and NMI blocking to the guest's
 * interruptibility state. */
static void load_intr(struct kvm_vcpu *v)
{
    uint32_t intr = 0;

    if (v->events.interrupt.shadow & SHADOW_STI) {
        intr |= INTR_STI;
    }
    if (v->events.interrupt.shadow & SHADOW_MOV_SS) {
        intr |= INTR_MOV_SS;
    }
    if (v->events.nmi.masked) {
        intr |= INTR_BLOCK_NMI;
    }
    vmcs_write(GUEST_INTERRUPTIBILITY_INFO, intr);
    /* An interrupt shadow under TF needs the single-step trap pending,
     * which the entry checks. */
    if ((intr & (INTR_STI | INTR_MOV_SS)) && (v->regs.rflags & RFLAGS_TF)) {
        vmcs_write(GUEST_PENDING_DBG_EXCEPTIONS, DR6_BS);
    }
}

/** Write the exception bitmap and DR7: the exceptions taken always, and
 * #BP while the caller's debugging sets software breakpoints, as Linux's
 * vmx_update_exception_bitmap; DR7 the caller's while it sets hardware
 * breakpoints, else the guest's. */
static void load_debug(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);

    x->exc_bitmap = EXC_ALWAYS;
    if (debugging(v, KVM_GUESTDBG_USE_SW_BP)) {
        x->exc_bitmap |= 1u << BP_VECTOR;
    }
    vmcs_write(EXCEPTION_BITMAP, x->exc_bitmap);
    vmcs_write(GUEST_DR7, dr7_of(v));
}

/** Write the parts of `v`'s state the record or this file changed since
 * the last entry, and clear their marks. */
static void load_dirty(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint32_t d = v->dirty, i;
    uint64_t pat;
    int n;

    if (d & KVM_PART_SREGS) {
        for (n = SEG_ES; n <= SEG_TR; n++) {
            seg_write(n, seg_of(v, n));
        }
        vmcs_write(GUEST_GDTR_BASE, v->sregs.gdt.base);
        vmcs_write(GUEST_GDTR_LIMIT, v->sregs.gdt.limit);
        vmcs_write(GUEST_IDTR_BASE, v->sregs.idt.base);
        vmcs_write(GUEST_IDTR_LIMIT, v->sregs.idt.limit);
        vmcs_write(GUEST_CR3, v->sregs.cr3);
        x->vdirty |= VX_CR0 | VX_CR4 | VX_EFER;
        x->fresh = 1;
        x->flush_vpid = 1;
    }
    if (x->vdirty & VX_CR0) {
        vmcs_write(CR0_READ_SHADOW, v->sregs.cr0);
        vmcs_write(GUEST_CR0, hw_cr0(v->sregs.cr0));
    }
    if (x->vdirty & (VX_CR0 | VX_CR4)) {
        vmcs_write(CR4_READ_SHADOW, v->sregs.cr4);
        vmcs_write(GUEST_CR4, hw_cr4(v->sregs.cr4, v->sregs.cr0));
    }
    if (x->vdirty & (VX_CR0 | VX_EFER)) {
        vmcs_write(GUEST_IA32_EFER, v->sregs.efer);
        x->entry =
            (v->sregs.efer & GEFER_LMA) ? (x->entry | ENTRY_IA32E) : (x->entry & ~ENTRY_IA32E);
        vmcs_write(VM_ENTRY_CONTROLS, x->entry);
    }
    if ((x->vdirty & VX_PDPTE) || ((d & KVM_PART_SREGS) && pae_paging(v))) {
        for (i = 0; i < 4u; i++) {
            vmcs_write(GUEST_PDPTR0 + 2u * i, x->pdpte[i]);
        }
    }
    if ((d & KVM_PART_EVENTS) || (x->vdirty & VX_INTR)) {
        load_intr(v);
    }
    if (d & KVM_PART_DEBUG) {
        load_debug(v);
    }
    if (d & KVM_PART_MSRS) {
        vmcs_write(GUEST_SYSENTER_CS, table_msr(v, MSR_IA32_SYSENTER_CS));
        vmcs_write(GUEST_SYSENTER_ESP, table_msr(v, MSR_IA32_SYSENTER_ESP));
        vmcs_write(GUEST_SYSENTER_EIP, table_msr(v, MSR_IA32_SYSENTER_EIP));
        pat = table_msr(v, MSR_IA32_CR_PAT);
        vmcs_write(GUEST_IA32_PAT, pat);
        for (i = 0; i < N_GUEST_MSRS; i++) {
            x->msrs[i * 2u] = guest_msrs[i];
            if (i != KGS_SLOT || x->kgs_table) {
                x->msrs[i * 2u + 1u] = table_msr(v, guest_msrs[i]);
            }
        }
    }
    if (d & KVM_PART_TSC) {
        vmcs_write(TSC_OFFSET, (uint64_t)v->tsc_offset);
        if (caps.proc2 & PROC2_TSC_SCALING) {
            vmcs_write(TSC_MULTIPLIER, v->tsc_ratio ? v->tsc_ratio : TSC_RATIO_ONE);
        }
    }
    if (d & KVM_PART_CPUID) {
        x->proc2 = proc2_for(v);
        vmcs_write(SECONDARY_VM_EXEC_CONTROL, x->proc2);
    }
    /* KVM_PART_FPU is the image and XCR0, loaded at every entry;
     * KVM_PART_CR8 the TPR, which every CR8 access exits for; KVM_PART_MP
     * the run state, which the run loop keeps. */
    v->dirty = 0;
    x->vdirty = 0;
}

/* ---- the entry ---- */

/** Read the exit of the run that just ended. */
static void read_exit(struct vmx_vcpu *x)
{
    struct vmx_exit *e = &x->ex;

    e->reason = (uint32_t)vmcs_read(VM_EXIT_REASON);
    e->qual = vmcs_read(EXIT_QUALIFICATION);
    e->intr_info = (uint32_t)vmcs_read(VM_EXIT_INTR_INFO);
    e->intr_error = (uint32_t)vmcs_read(VM_EXIT_INTR_ERROR_CODE);
    e->idt_info = (uint32_t)vmcs_read(IDT_VECTORING_INFO_FIELD);
    e->idt_error = (uint32_t)vmcs_read(IDT_VECTORING_ERROR_CODE);
    e->insn_len = (uint32_t)vmcs_read(VM_EXIT_INSTRUCTION_LEN);
    e->insn_info = (uint32_t)vmcs_read(VMX_INSTRUCTION_INFO);
    e->interruptibility = (uint32_t)vmcs_read(GUEST_INTERRUPTIBILITY_INFO);
    e->gpa =
        (e->reason & 0xFFFFu) == EXIT_EPT_VIOLATION || (e->reason & 0xFFFFu) == EXIT_EPT_MISCONFIG
            ? vmcs_read(GUEST_PHYSICAL_ADDRESS)
            : 0;
}

/** Read the guest's special registers into `v`'s record: the segments,
 * the tables, CR3, CR0's bits the guest owns, DR7 and the PDPTEs. */
static void read_sregs(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint64_t cr0;
    int n, i;

    for (n = SEG_ES; n <= SEG_TR; n++) {
        seg_read(n, seg_of(v, n));
    }
    v->sregs.gdt.base = vmcs_read(GUEST_GDTR_BASE);
    v->sregs.gdt.limit = (uint16_t)vmcs_read(GUEST_GDTR_LIMIT);
    v->sregs.idt.base = vmcs_read(GUEST_IDTR_BASE);
    v->sregs.idt.limit = (uint16_t)vmcs_read(GUEST_IDTR_LIMIT);
    v->sregs.cr3 = vmcs_read(GUEST_CR3);
    cr0 = vmcs_read(GUEST_CR0);
    v->sregs.cr0 = (v->sregs.cr0 & ~CR0_GUEST_OWNED) | (cr0 & CR0_GUEST_OWNED);
    if (!debugging(v, KVM_GUESTDBG_USE_HW_BP)) {
        v->debugregs.dr7 = vmcs_read(GUEST_DR7);
    }
    if (pae_paging(v)) {
        for (i = 0; i < 4; i++) {
            x->pdpte[i] = vmcs_read(GUEST_PDPTR0 + 2u * (uint32_t)i);
        }
    }
    x->fresh = 1;
}

/** Whether the exit `e` of `v` needs the special registers in the record
 * for its handling: an access to MMIO, string I/O, a control register's
 * write, a debug register, a hypercall's privilege, an exception, a task
 * switch, the APIC base's write, which vlapic.c marks as theirs, and any
 * exit under the caller's single step, whose report gives CS's base. */
static int needs_sregs(const struct kvm_vcpu *v, const struct vmx_exit *e)
{
    if (debugging(v, KVM_GUESTDBG_SINGLESTEP)) {
        return 1;
    }
    switch (e->reason) {
    case EXIT_EPT_VIOLATION:
    case EXIT_EPT_MISCONFIG:
    case EXIT_DR_ACCESS:
    case EXIT_VMCALL:
    case EXIT_TASK_SWITCH:
    case EXIT_EXCEPTION_NMI:
        return 1;
    case EXIT_IO:
        return (e->qual & 0x10u) != 0;
    case EXIT_CR_ACCESS:
        return (e->qual & 0xFu) != 8u;
    case EXIT_MSR_WRITE:
        return (uint32_t)v->regs.rcx == MSR_APIC_BASE;
    default:
        return 0;
    }
}

/**
 * Load DR0 to DR3 for the run when the DR7 it runs under enables a
 * breakpoint: the caller's breakpoints while it sets them, else the
 * guest's; the host's DR7, which the exit clears, comes back after. DR6
 * is not loaded: the guest reads and writes the record's, since every MOV
 * DR exits, and a #DB exits with its bits in the qualification and the
 * processor's DR6 as it was.
 */
static void dr_load(struct kvm_vcpu *v, uint64_t *host_dr7, uint64_t host_db[4])
{
    struct vmx_vcpu *x = vx(v);
    int i, hw = debugging(v, KVM_GUESTDBG_USE_HW_BP);

    *host_dr7 = vmx_op->read_dr(7);
    x->guest_dr = (dr7_of(v) & DR7_ENABLES) != 0;
    if (!x->guest_dr) {
        return;
    }
    for (i = 0; i < 4; i++) {
        host_db[i] = vmx_op->read_dr(i);
        vmx_op->write_dr(i, hw ? v->guest_debug.debugreg[i] : v->debugregs.db[i]);
    }
}

/** Put the host's debug registers back. */
static void dr_store(struct kvm_vcpu *v, uint64_t host_dr7, const uint64_t host_db[4])
{
    struct vmx_vcpu *x = vx(v);
    int i;

    if (x->guest_dr) {
        for (i = 0; i < 4; i++) {
            vmx_op->write_dr(i, host_db[i]);
        }
    }
    if (host_dr7 & DR7_ENABLES) {
        vmx_op->write_dr(7, host_dr7);
    }
}

/** Drop the processor's translations of `v`: the guest-physical ones of
 * its EPT tables and, with `linear`, the guest's own under its VPID. */
static void flush(struct kvm_vcpu *v, uint64_t eptp, int linear)
{
    struct vmx_vcpu *x = vx(v);
    uint64_t desc[2], type;

    desc[1] = 0;
    if (eptp) {
        desc[0] = eptp;
        type = (caps.ept_vpid & EPT_CAP_INVEPT_CTX) ? INV_SINGLE : INV_ALL;
        if (vmx_op->invept(type, desc) != VMX_OK) {
            insn_failed("invept", type);
        }
    }
    if (linear && (caps.proc2 & PROC2_VPID)) {
        desc[0] = x->vpid;
        type = (caps.ept_vpid & VPID_CAP_INVVPID_CTX) ? INV_SINGLE : INV_ALL;
        if (vmx_op->invvpid(type, desc) != VMX_OK) {
            insn_failed("invvpid", type);
        }
    }
}

/** The EPT pointer of `v`'s VM: four levels, write-back, with the
 * accessed and dirty bits where the processor has them. */
static uint64_t eptp_of(const struct kvm_vcpu *v)
{
    return kvm_tdp_root(v->vm) | (3u << 3) | 6u | (caps.ad ? 0x40u : 0u);
}

/** Write the controls and the guest state of the next entry: the window
 * the caller's run page asks for without the local APIC in the kernel,
 * while the guest cannot take an interrupt now; the event to inject; RSP,
 * RIP and RFLAGS; and the preemption timer, which bounds the run. */
static void entry_fields(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);

    if (v->vm->irqchip == KVM_IRQCHIP_NONE) {
        x->proc_want &= ~PROC_INTR_WINDOW;
        if (v->run->request_interrupt_window &&
            (x->inj_info || !(v->regs.rflags & RFLAGS_IF) || v->events.interrupt.shadow)) {
            x->proc_want |= PROC_INTR_WINDOW;
        }
    }
    if (x->proc_want != x->proc) {
        x->proc = x->proc_want;
        vmcs_write(CPU_BASED_VM_EXEC_CONTROL, x->proc);
    }
    vmcs_write(VM_ENTRY_INTR_INFO_FIELD, x->inj_info);
    /* A guest halted in HLT of its own wakes to the event, as Linux's
     * vmx_clear_hlt makes it: the HLT has completed, and RIP is past it. */
    if (x->inj_info && v->hlt_in_guest) {
        vmcs_write(GUEST_ACTIVITY_STATE, 0);
    }
    if (x->inj_info) {
        vmcs_write(VM_ENTRY_EXCEPTION_ERROR_CODE, x->inj_error);
        vmcs_write(VM_ENTRY_INSTRUCTION_LEN, x->inj_len);
    }
    if ((x->inj_info & INTR_VALID) && INTR_TYPE(x->inj_info) == TYPE_EXT_INTR) {
        v->irq_counts[INTR_VECTOR(x->inj_info)][KVM_VEC_INJECT]++;
    }
    vmcs_write(GUEST_RSP, v->regs.rsp);
    vmcs_write(GUEST_RIP, v->regs.rip);
    /* The caller's single step is the guest's TF, hidden from the guest's
     * RFLAGS after the exit, as Linux's __kvm_set_rflags sets it. */
    x->tf_set = debugging(v, KVM_GUESTDBG_SINGLESTEP) && !(v->regs.rflags & RFLAGS_TF);
    vmcs_write(GUEST_RFLAGS, v->regs.rflags | (x->tf_set ? RFLAGS_TF : 0));
    vmcs_write(VMX_PREEMPTION_TIMER_VALUE, VMX_RUN_BOUND_TSC >> caps.timer_shift);
}

/** Whether the exit `e` took the host's NMI from the run. */
static int exit_is_nmi(const struct vmx_exit *e)
{
    return e->reason == EXIT_EXCEPTION_NMI && (e->intr_info & INTR_VALID) &&
           INTR_TYPE(e->intr_info) == TYPE_NMI;
}

/** Whether the exit `e` took a machine check from the run, or the entry
 * met one. */
static int exit_is_mce(const struct vmx_exit *e)
{
    return (e->reason == EXIT_EXCEPTION_NMI && (e->intr_info & INTR_VALID) &&
            INTR_TYPE(e->intr_info) == TYPE_HW_EXC && INTR_VECTOR(e->intr_info) == MC_VECTOR) ||
           (e->reason & 0xFFFFu) == EXIT_MCE_DURING_ENTRY;
}

/** Read what the run that just ended left, while the VMCS is current: the
 * entry instruction's error, or the exit, RSP, RIP and RFLAGS, and the
 * special registers the exit's handling needs. */
static void after_run(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);

    x->fresh = 0;
    if (x->ex.failed != VMX_OK) {
        x->ex.insn_error =
            x->ex.failed == VMX_FAIL_VALID ? (uint32_t)vmcs_read(VM_INSTRUCTION_ERROR) : 0;
        return;
    }
    x->inj_info = 0;
    read_exit(x);
    v->regs.rsp = vmcs_read(GUEST_RSP);
    v->regs.rip = vmcs_read(GUEST_RIP);
    v->regs.rflags = vmcs_read(GUEST_RFLAGS);
    if (x->tf_set) {
        v->regs.rflags &= ~RFLAGS_TF;
    }
    x->kgs_table = 0;
    if (needs_sregs(v, &x->ex)) {
        read_sregs(v);
    } else {
        /* CS gives the mode in which the kernel steps RIP, and SS the
         * privilege, at every exit, as Linux's is_64_bit_mode reads CS. */
        seg_read(SEG_CS, &v->sregs.cs);
        seg_read(SEG_SS, &v->sregs.ss);
    }
}

/**
 * Run the guest until its next exit, with interrupts off and without the
 * big lock: VMX on, the VMCS current and brought up to date, the guest's
 * FPU, debug registers and MSRs in, VMLAUNCH; at the exit the host's state
 * back, the exit read, and VMX off. An NMI or a machine check the run took
 * from the host reaches the host's own handler after VMX is off, as
 * Linux's vmx_do_nmi_irqoff and kvm_machine_check pass them on; the host's
 * interrupt that ended the run waits until the run loop turns interrupts
 * on. Under the caller's single step the STI and MOV SS shadow ends before
 * the entry, as Linux's vmx_vcpu_run ends it: the step over STI or MOV SS
 * has been taken by the caller, and the shadow under TF would make the
 * entry fail for want of a pending single-step trap. While
 * `v->emul.resume` is set no guest runs: the next step of the instruction
 * under way is `exit`'s. 0, or -EIO when VMX cannot be turned on here.
 */
static int vmx_enter(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    struct vmx_host host;
    uint64_t cr4, eptp, host_dr7 = 0, host_db[4] = {0, 0, 0, 0}, global[2];
    uint32_t ng, nh, gbase = caps.rdtscp ? N_GUEST_MSRS : N_GUEST_MSRS - 1u;
    uint32_t hbase = caps.rdtscp ? N_HOST_MSRS : N_HOST_MSRS - 1u;
    int cpu = (int)arch_cpu_id(), r, tlb, nmi = 0, mce = 0;

    if (v->emul.resume) {
        return 0;
    }
    tlb = kvm_vcpu_test_clear(v, KVM_REQ_TLB_FLUSH);
    r = vmx_on(x, cpu, &cr4);
    if (r != 0) {
        return r;
    }
    if (!x->built) {
        vmcs_build(v);
    }
    vmx_op->save_host(&host);
    if (x->last_cpu != cpu) {
        host_cpu_fields(x, cpu, &host);
        tlb = 1;
        x->flush_vpid = 1;
    }
    vmcs_write(HOST_CR3, vmx_op->read_cr(3));
    vmcs_write(HOST_CR4, vmx_op->read_cr(4));
    vmcs_write(HOST_FS_BASE, host.fs_base);
    eptp = eptp_of(v);
    vmcs_write(EPT_POINTER, eptp);
    if (debugging(v, KVM_GUESTDBG_SINGLESTEP) && v->events.interrupt.shadow) {
        /* Under the step's TF the entry would ask for a pending step. */
        v->events.interrupt.shadow = 0;
        x->vdirty |= VX_INTR;
    }
    load_dirty(v);
    if (tlb || x->flush_vpid) {
        flush(v, tlb ? eptp : 0, x->flush_vpid);
        x->flush_vpid = 0;
    }
    entry_fields(v);
    kvm_pmu_vmx_enter(v, (struct kvm_pmu_msr *)x->msrs + gbase,
                      (struct kvm_pmu_msr *)x->msrs + HOST_LIST + hbase, &ng, &nh,
                      perf_switched() ? global : NULL);
    if (perf_switched()) {
        vmcs_write(GUEST_IA32_PERF_GLOBAL_CTRL, global[0]);
        vmcs_write(HOST_IA32_PERF_GLOBAL_CTRL, global[1]);
    }
    if (x->debugctl != (kvm_pmu_vmcs_debugctl(v))) {
        x->debugctl = kvm_pmu_vmcs_debugctl(v);
        vmcs_write(GUEST_IA32_DEBUGCTL, x->debugctl);
    }
    vmcs_write(VM_ENTRY_MSR_LOAD_COUNT, gbase + ng);
    vmcs_write(VM_EXIT_MSR_STORE_COUNT, gbase + ng);
    vmcs_write(VM_EXIT_MSR_LOAD_COUNT, hbase + nh);
    vmx_op->fpu_load(x->host_fpu, v->xsave, v->xcr0);
    dr_load(v, &host_dr7, host_db);
    x->ex.failed = vmx_op->run(&v->regs, &v->sregs.cr2, 0);
    dr_store(v, host_dr7, host_db);
    vmx_op->fpu_store(x->host_fpu, v->xsave, v->xcr0);
    vmx_op->restore_host(&host);
    after_run(v);
    kvm_pmu_vmx_exit(v, (const struct kvm_pmu_msr *)x->msrs + gbase,
                     x->ex.failed == VMX_OK && !(x->ex.reason & EXIT_FAILED_ENTRY));
    if (x->ex.failed == VMX_OK && !(x->ex.reason & EXIT_FAILED_ENTRY) &&
        (x->debugctl & DEBUGCTL_LBR)) {
        /* A #DB in the guest clears DEBUGCTL.LBR, and the exit saved it. */
        x->debugctl &= vmcs_read(GUEST_IA32_DEBUGCTL);
        v->pmu.debugctl = x->debugctl;
    }
    if (x->ex.failed == VMX_OK) {
        nmi = exit_is_nmi(&x->ex);
        mce = exit_is_mce(&x->ex);
    }
    vmx_off(x, cpu, cr4);
    if (nmi) {
        vmx_op->nmi();
    }
    if (mce) {
        vmx_op->machine_check();
    }
    return 0;
}

/* ---- the exits ---- */

/** Take the event the exit found under delivery back into `v`'s record,
 * to inject again at the next entry, and the interrupt shadow and NMI
 * blocking the exit left, as Linux's __vmx_complete_interrupts. */
static void complete_events(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint32_t info = x->ex.idt_info, type;
    uint32_t intr = x->ex.interruptibility;

    v->events.exception.injected = 0;
    v->events.exception.pending = 0;
    v->events.interrupt.injected = 0;
    v->events.nmi.injected = 0;
    v->events.interrupt.shadow = (uint8_t)(((intr & INTR_STI) ? SHADOW_STI : 0u) |
                                           ((intr & INTR_MOV_SS) ? SHADOW_MOV_SS : 0u));
    v->events.nmi.masked = (uint8_t)((intr & INTR_BLOCK_NMI) != 0);
    /* An NMI's IRET that faulted on EPT unblocked NMIs; the guest's IRET
     * runs again, so NMIs stay blocked until it does, as the SDM's 27.2.3
     * asks. */
    if (x->ex.reason == EXIT_EPT_VIOLATION && (x->ex.qual & INTR_NMI_UNBLOCK) &&
        !(info & INTR_VALID)) {
        v->events.nmi.masked = 1;
        x->vdirty |= VX_INTR;
    }
    if (!(info & INTR_VALID)) {
        return;
    }
    type = INTR_TYPE(info);
    switch (type) {
    case TYPE_NMI:
        v->events.nmi.injected = 1;
        v->events.nmi.masked = 0;
        x->vdirty |= VX_INTR;
        break;
    case TYPE_SOFT_EXC:
    case TYPE_PRIV_SOFT:
    case TYPE_HW_EXC:
        v->events.exception.injected = 1;
        v->events.exception.nr = (uint8_t)INTR_VECTOR(info);
        v->events.exception.has_error_code = (uint8_t)((info & INTR_ERRCODE) != 0);
        v->events.exception.error_code = x->ex.idt_error;
        x->inj_len = type == TYPE_HW_EXC ? 0 : x->ex.insn_len;
        break;
    case TYPE_SOFT_INTR:
    case TYPE_EXT_INTR:
        v->events.interrupt.injected = 1;
        v->events.interrupt.nr = (uint8_t)INTR_VECTOR(info);
        v->events.interrupt.soft = (uint8_t)(type == TYPE_SOFT_INTR);
        x->inj_len = type == TYPE_SOFT_INTR ? x->ex.insn_len : 0;
        if (type == TYPE_EXT_INTR) {
            v->irq_counts[INTR_VECTOR(info)][KVM_VEC_SAVE]++;
        }
        break;
    default:
        break;
    }
    kvm_vcpu_kick(v, KVM_REQ_EVENT);
}

/** An exit the backend does not handle: the internal error Linux gives
 * for an unexpected exit reason, with the reason and the processor. */
static void unexpected(struct kvm_vcpu *v)
{
    struct kvm_exit *ex = &v->exit;

    ex->kind = KVM_X_INTERNAL;
    k_memset(&ex->internal, 0, sizeof(ex->internal));
    ex->internal.suberror = KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
    ex->internal.ndata = 2;
    ex->internal.data[0] = vx(v)->ex.reason;
    ex->internal.data[1] = (uint64_t)(int64_t)v->cpu;
}

/** The register `n`, by the processor's number, as an instruction of `v`
 * reads it: its low half outside 64-bit mode. */
static uint64_t gpr_read(struct kvm_vcpu *v, uint32_t n)
{
    static const uint8_t slot[16] = {0, 2, 3, 1, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
    uint64_t x = ((uint64_t *)&v->regs)[slot[n & 15u]];

    return kvm_emul_mode(v) == 64 ? x : (x & 0xFFFFFFFFull);
}

/** Write the register `n` of `v`: a 32-bit write outside 64-bit mode. */
static void gpr_write(struct kvm_vcpu *v, uint32_t n, uint64_t value)
{
    static const uint8_t slot[16] = {0, 2, 3, 1, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};

    ((uint64_t *)&v->regs)[slot[n & 15u]] =
        kvm_emul_mode(v) == 64 ? value : (value & 0xFFFFFFFFull);
}

/** Queue #GP(0) for `v`'s instruction, which does not complete. */
static void gp(struct kvm_vcpu *v)
{
    kvm_queue_exception(v, GP_VECTOR, 1, 0);
}

/**
 * The guest's write of `cr0` to CR0, with Linux's checks of kvm_set_cr0:
 * #GP for high bits, NW without CD, PG without PE, long mode's PG without
 * PAE or with a 64-bit CS, and PAE's PDPTEs that do not load. Long mode
 * begins and ends with PG while EFER.LME is set. 0, or -1 with #GP queued.
 */
static int set_cr0(struct kvm_vcpu *v, uint64_t cr0)
{
    struct vmx_vcpu *x = vx(v);
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
    if ((cr0 & CR0_PG) && (v->sregs.cr4 & CR4_PAE) && !(efer & GEFER_LMA) &&
        ((old ^ cr0) & (CR0_PG | CR0_PE | CR0_CD | CR0_NW)) && pdpte_load(v) != 0) {
        v->sregs.cr0 = old;
        v->sregs.efer = old_efer;
        gp(v);
        return -1;
    }
    if ((old ^ cr0) & CR0_PG) {
        x->flush_vpid = 1;
    }
    x->vdirty |= VX_CR0 | VX_EFER;
    return 0;
}

/**
 * The guest's write of `cr4` to CR4, with Linux's checks of kvm_set_cr4:
 * #GP for a bit the guest's CPUID reserves, PAE cleared in long mode, PCIDE
 * set outside long mode or with CR3's PCID nonzero, and PDPTEs that do not
 * load. A change of PGE, PCIDE, PAE, PSE or SMEP drops the guest's linear
 * translations, as the processor does. 0, or -1 with #GP queued.
 */
static int set_cr4(struct kvm_vcpu *v, uint64_t cr4)
{
    struct vmx_vcpu *x = vx(v);
    uint64_t old = v->sregs.cr4;
    int lma = (v->sregs.efer & GEFER_LMA) != 0;

    if ((cr4 & kvm_cr4_reserved(v)) || (lma && !(cr4 & CR4_PAE)) ||
        ((cr4 & CR4_PCIDE) && !(old & CR4_PCIDE) && (!lma || (v->sregs.cr3 & 0xFFFu)))) {
        gp(v);
        return -1;
    }
    v->sregs.cr4 = cr4;
    if (pae_paging(v) && ((old ^ cr4) & (CR4_PGE | CR4_PSE | CR4_PAE | CR4_SMEP)) &&
        pdpte_load(v) != 0) {
        v->sregs.cr4 = old;
        gp(v);
        return -1;
    }
    if ((old ^ cr4) & (CR4_PGE | CR4_PCIDE | CR4_PAE | CR4_PSE | CR4_SMEP)) {
        x->flush_vpid = 1;
    }
    x->vdirty |= VX_CR4;
    return 0;
}

/** CR8 of `v`: the in-kernel APIC's TPR, or the record's without one. */
static uint64_t get_cr8(struct kvm_vcpu *v)
{
    return v->vm->irqchip != KVM_IRQCHIP_NONE ? kvm_lapic_cr8(v) : v->sregs.cr8;
}

/** A control register's access (exit 28), whose qualification names the
 * register, the kind and the general register: MOV to CR0, CR4 and CR8,
 * MOV from CR8, and LMSW. CR3 and CLTS do not exit. */
static int exit_cr(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint64_t q = x->ex.qual, val;
    uint32_t cr = (uint32_t)(q & 0xFu), kind = (uint32_t)((q >> 4) & 3u);
    uint32_t reg = (uint32_t)((q >> 8) & 0xFu);
    int r = 0;

    if (kind == 0) {
        val = gpr_read(v, reg);
        switch (cr) {
        case 0:
            r = set_cr0(v, val);
            break;
        case 4:
            r = set_cr4(v, val);
            break;
        case 8:
            if (val & ~0xFull) {
                gp(v);
                r = -1;
                break;
            }
            if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
                kvm_lapic_set_cr8(v, val);
            } else {
                v->sregs.cr8 = val;
            }
            /* A lower priority may let an interrupt through. */
            kvm_vcpu_kick(v, KVM_REQ_EVENT);
            break;
        default:
            unexpected(v);
            return 0;
        }
    } else if (kind == 1 && cr == 8) {
        gpr_write(v, reg, get_cr8(v));
    } else if (kind == 3) {
        /* LMSW: the low four bits, PE set but never cleared. */
        val = (q >> 16) & 0xFu;
        r = set_cr0(v, (v->sregs.cr0 & ~0xEull) | val | (v->sregs.cr0 & CR0_PE));
    } else {
        unexpected(v);
        return 0;
    }
    if (r == 0) {
        kvm_emul_skip(v, x->ex.insn_len);
    }
    v->exit.kind = KVM_X_AGAIN;
    return 0;
}

/** A debug register's access (exit 29), every one of which exits, as
 * Linux's handle_dr emulates them: DR4 and DR5 alias DR6 and DR7 unless
 * CR4.DE makes them #UD, DR7.GD raises #DB, and the upper halves of DR6 and
 * DR7 are #GP. */
static int exit_dr(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint32_t dr = (uint32_t)(x->ex.qual & 7u), reg = (uint32_t)((x->ex.qual >> 8) & 0xFu);
    int to_dr = !((x->ex.qual >> 4) & 1u);
    uint64_t val;

    v->exit.kind = KVM_X_AGAIN;
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
    if (to_dr) {
        val = gpr_read(v, reg);
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
        gpr_write(v, reg, val);
    }
    kvm_emul_skip(v, x->ex.insn_len);
    return 0;
}

/** Port I/O (exit 30): the qualification's size, direction, string, REP
 * and port; for INS and OUTS, the instruction information's address size
 * and OUTS's segment, or the mode's address size and DS without it. */
static int exit_io(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint64_t q = x->ex.qual;
    struct kvm_pio p;
    int mode;

    k_memset(&p, 0, sizeof(p));
    p.size = (uint32_t)(q & 7u) + 1u;
    p.in = (int)((q >> 3) & 1u);
    p.string = (int)((q >> 4) & 1u);
    p.rep = (int)((q >> 5) & 1u);
    p.port = (uint16_t)(q >> 16);
    p.len = x->ex.insn_len;
    if (p.string) {
        if (caps.ins_outs) {
            p.addr_size = 2u << ((x->ex.insn_info >> 7) & 7u);
            p.seg = (x->ex.insn_info >> 15) & 7u;
        } else {
            mode = kvm_emul_mode(v);
            p.addr_size = mode == 64 ? 8u : mode == 32 ? 4u : 2u;
            p.seg = KVM_SEG_DS;
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
    if ((efer & ~ok) || ((v->sregs.cr0 & CR0_PG) && ((efer ^ v->sregs.efer) & GEFER_LME))) {
        return -1;
    }
    v->sregs.efer = (efer & ~GEFER_LMA) | (v->sregs.efer & GEFER_LMA);
    vx(v)->vdirty |= VX_EFER;
    return 0;
}

/** Make msr.c's KERNEL_GS_BASE the guest's, which the last exit stored,
 * before anything there changes, since the next entry loads the list from
 * msr.c. */
static void kgs_sync(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);

    if (!x->kgs_table) {
        (void)kvm_msr_set(v, MSR_KERNEL_GS_BASE, x->msrs[KGS_SLOT * 2u + 1u], 1);
        x->kgs_table = 1;
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
        return kvm_pmu_debugctl(v, write, value, 0) ? KVM_MSR_INVAL : 0;
    default:
        return kvm_emul_msr(v, index, write, value);
    }
}

/** RDMSR and WRMSR (exits 31 and 32), as Linux's kvm_emulate_rdmsr and
 * kvm_emulate_wrmsr: an access KVM_X86_SET_MSR_FILTER denies fails at
 * once, any other is `msr_access`'s. A failure goes to the caller when
 * KVM_CAP_X86_USER_SPACE_MSR asked for its reason, and is #GP otherwise,
 * as Linux's default without ignore_msrs. */
static int exit_msr(struct kvm_vcpu *v, int write)
{
    struct vmx_vcpu *x = vx(v);
    uint32_t index = (uint32_t)v->regs.rcx;
    uint64_t value = (v->regs.rax & 0xFFFFFFFFull) | (v->regs.rdx << 32);
    int r;

    v->exit.kind = KVM_X_AGAIN;
    if (index - MSR_FS_BASE < 3u && kvm_msr_allowed(v, index, write)) {
        /* The bitmap predates the filter, and KVM_REQ_MSR_FILTER brings it
         * up to date before the instruction runs again. */
        return 0;
    }
    if (write) {
        kgs_sync(v);
    }
    r = kvm_msr_allowed(v, index, write) ? msr_access(v, index, write, &value) : KVM_MSR_FILTERED;
    if (r != 0) {
        if (!kvm_msr_user_exit(v, index, write, value, r, x->ex.insn_len)) {
            gp(v);
        }
        return 0;
    }
    if (!write) {
        v->regs.rax = value & 0xFFFFFFFFull;
        v->regs.rdx = value >> 32;
    }
    kvm_emul_skip(v, x->ex.insn_len);
    return 0;
}

/** VMCALL (exit 18): a Hyper-V hypercall when the guest has Hyper-V
 * (hyperv.c), else a KVM hypercall, whose answer goes in RAX. */
static int exit_vmcall(struct kvm_vcpu *v)
{
    uint64_t ret;

    if (kvm_hv_hypercall_enabled(v)) {
        return kvm_hv_hypercall(v, vx(v)->ex.insn_len);
    }
    ret = kvm_emul_hypercall(v);

    v->regs.rax = kvm_emul_mode(v) == 64 ? ret : (ret & 0xFFFFFFFFull);
    kvm_emul_skip(v, vx(v)->ex.insn_len);
    v->exit.kind = KVM_X_AGAIN;
    return 0;
}

/** The exit's numbers for an internal error's report, as Linux's
 * vmx_get_exit_info gives them: the reason, the qualification, the IDT
 * vectoring information, and the exit's interruption information with its
 * error code, 0 unless that is an exception with one. */
static void exit_info(const struct kvm_vcpu *v, uint64_t info[5])
{
    const struct vmx_exit *e = &vx(v)->ex;
    uint32_t coded = INTR_VALID | INTR_ERRCODE;

    info[0] = e->reason;
    info[1] = e->qual;
    info[2] = e->idt_info;
    info[3] = e->intr_info;
    info[4] = (e->intr_info & coded) == coded ? e->intr_error : 0;
}

/** An EPT violation (exit 48): a fault on the guest-physical tables,
 * which `kvm_tdp_fault` maps, or an access to a frame no slot holds, MMIO,
 * which emulate.c carries out. */
static int exit_ept(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint64_t info[5];
    int r;

    r = kvm_tdp_fault(v->vm, x->ex.gpa >> 12, (x->ex.qual & 2u) != 0);
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

/** The failed entry's exit: KVM_EXIT_FAIL_ENTRY with the instruction's
 * error, or the exit reason for an entry the processor refused after the
 * instruction, as Linux reports both. */
static void fail_entry(struct kvm_vcpu *v, uint64_t reason)
{
    v->exit.kind = KVM_X_FAIL_ENTRY;
    v->exit.hw_reason = reason;
}

/**
 * The guest's own #DB, as Linux's handle_exception_nmi gives it back: its
 * bits, which the qualification holds, folded into the guest's DR6, and
 * the exception queued. INT1 exits on the instruction, so RIP steps past
 * it, without the single-step trap it outranks, and the interrupt shadow
 * ends. For any other under a shadow with TF, the entry needs the
 * single-step trap pending, which `load_intr` writes with the event.
 */
static void guest_db(struct kvm_vcpu *v)
{
    struct vmx_exit *e = &vx(v)->ex;
    uint64_t rip;

    if (INTR_TYPE(e->intr_info) == TYPE_PRIV_SOFT) {
        rip = v->regs.rip + e->insn_len;
        v->regs.rip = kvm_emul_mode(v) == 64 ? rip : (rip & 0xFFFFFFFFull);
        v->events.interrupt.shadow = 0;
    }
    kvm_queue_db(v, e->qual);
}

/**
 * An exception's exit (0): the host's NMI or machine check, which `enter`
 * passed on; #UD, whose instruction emulate.c runs when it is one Linux's
 * emulator runs after a #UD, as Linux's handle_ud; #AC, given back to the
 * guest; the guest's own #DB, given back; and the #DB or #BP the caller's
 * debugging takes, which goes to the caller as KVM_EXIT_DEBUG with the
 * address, DR6 and DR7, as Linux's handle_exception_nmi gives it. The
 * bitmap takes no other.
 */
static int exit_exception(struct kvm_vcpu *v)
{
    struct vmx_exit *e = &vx(v)->ex;
    uint32_t vec = INTR_VECTOR(e->intr_info);
    struct kvm_run_debug *d = &v->exit.debug;
    uint64_t info[5];

    if (INTR_TYPE(e->intr_info) == TYPE_NMI || vec == MC_VECTOR) {
        return 0;
    }
    switch (vec) {
    case UD_VECTOR:
        exit_info(v, info);
        return kvm_emul_ud(v, info);
    case AC_VECTOR:
        kvm_queue_exception(v, AC_VECTOR, 1, e->intr_error);
        return 0;
    case DB_VECTOR:
        if (!debugging(v, KVM_GUESTDBG_SINGLESTEP | KVM_GUESTDBG_USE_HW_BP)) {
            guest_db(v);
            return 0;
        }
        break;
    case BP_VECTOR:
        if (debugging(v, KVM_GUESTDBG_USE_SW_BP)) {
            break;
        }
        unexpected(v);
        return 0;
    default:
        unexpected(v);
        return 0;
    }
    v->exit.kind = KVM_X_DEBUG;
    k_memset(d, 0, sizeof(*d));
    d->exception = vec;
    d->pc = v->sregs.cs.base + v->regs.rip;
    d->dr6 = vec == DB_VECTOR ? ((e->qual & 0x600Full) | DR6_FIXED) : 0;
    d->dr7 = dr7_of(v);
    return 0;
}

/**
 * A task switch (exit 9), which the kernel carries out, as Linux's
 * handle_task_switch: the TSS selector and the source from the
 * qualification; through a task gate, the event the exit found under
 * delivery taken off the queue, an NMI leaving NMIs blocked and an
 * exception giving its error code to push; an instruction that caused it
 * stepped past in the EIP the old task saves. A TSS or table in no slot
 * is the internal error Linux reports.
 */
static int exit_task_switch(struct kvm_vcpu *v)
{
    struct vmx_exit *e = &vx(v)->ex;
    uint32_t info = e->idt_info, type = INTR_TYPE(info);
    int reason = (int)((e->qual >> 30) & 3u), valid = (info & INTR_VALID) != 0;
    int has_error = 0, r;
    uint64_t next = v->regs.rip;

    if (reason == KVM_TS_GATE && valid && type == TYPE_NMI) {
        v->events.nmi.injected = 0;
        v->events.nmi.masked = 1;
    } else if (reason == KVM_TS_GATE && valid &&
               (type == TYPE_EXT_INTR || type == TYPE_SOFT_INTR)) {
        v->events.interrupt.injected = 0;
    } else if (reason == KVM_TS_GATE && valid) {
        has_error = type == TYPE_HW_EXC && (info & INTR_ERRCODE);
        v->events.exception.injected = 0;
        v->events.exception.pending = 0;
    }
    if (!valid || (type != TYPE_HW_EXC && type != TYPE_EXT_INTR && type != TYPE_NMI)) {
        next = (next + e->insn_len) & 0xFFFFFFFFull;
    }
    r = kvm_task_switch(v, (uint16_t)e->qual, reason,
                        valid && type == TYPE_SOFT_INTR ? (int)INTR_VECTOR(info) : -1, next,
                        has_error ? (int64_t)e->idt_error : -1);
    if (r > 0) {
        v->exit.kind = KVM_X_INTERNAL;
        k_memset(&v->exit.internal, 0, sizeof(v->exit.internal));
        v->exit.internal.suberror = KVM_INTERNAL_ERROR_EMULATION;
    }
    return r < 0 && r != -1 ? r : 0;
}

/**
 * The exits of instructions the kernel answers alone, each stepped past
 * or faulted: CPUID, HLT, INVD, MONITOR, MWAIT, WBINVD, RDPMC, XSETBV,
 * VMCALL, GETSEC and the VMX instructions of a nested guest, which is not
 * offered. Whether `reason` is one of them, with `v->exit` set.
 */
static int exit_insn(struct kvm_vcpu *v, uint32_t reason)
{
    uint32_t len = vx(v)->ex.insn_len;

    switch (reason) {
    case EXIT_CPUID:
        if (kvm_emul_cpuid(v) == 0) {
            kvm_emul_skip(v, len);
        }
        return 1;
    case EXIT_HLT:
        kvm_emul_skip(v, len);
        v->exit.kind = KVM_X_HLT;
        return 1;
    case EXIT_MWAIT:
    case EXIT_MONITOR:
        /* MONITOR and MWAIT as no-ops, as Linux's kvm_emulate_monitor_mwait:
         * #UD only with KVM_X86_QUIRK_MWAIT_NEVER_UD_FAULTS turned off, for a
         * guest whose CPUID lacks MWAIT. */
        if (!KVM_QUIRK(v->vm, KVM_X86_QUIRK_MWAIT_NEVER_UD_FAULTS) && !kvm_msr_guest_mwait(v)) {
            kvm_queue_exception(v, UD_VECTOR, 0, 0);
            return 1;
        }
        kvm_emul_skip(v, len);
        return 1;
    case EXIT_INVD:
        /* A no-op, as Linux's kvm_emulate_invd. */
        kvm_emul_skip(v, len);
        return 1;
    case EXIT_WBINVD:
        /* A cache flush matters only to a device that does not snoop, as
         * Linux's kvm_emulate_wbinvd judges. */
        if (v->vm->noncoherent) {
            vmx_op->wbinvd();
        }
        kvm_emul_skip(v, len);
        return 1;
    case EXIT_RDPMC:
        if (kvm_pmu_rdpmc(v) == 0) {
            kvm_emul_skip(v, len);
        }
        return 1;
    case EXIT_XSETBV:
        if (kvm_emul_xsetbv(v) == 0) {
            kvm_emul_skip(v, len);
        }
        return 1;
    case EXIT_VMCALL:
        (void)exit_vmcall(v);
        return 1;
    case EXIT_GETSEC:
    case EXIT_INVEPT:
    case EXIT_INVVPID:
        kvm_queue_exception(v, UD_VECTOR, 0, 0);
        return 1;
    default:
        break;
    }
    if (reason >= EXIT_VMCLEAR && reason <= EXIT_VMXON) {
        kvm_queue_exception(v, UD_VECTOR, 0, 0);
        return 1;
    }
    return 0;
}

/**
 * Name the exit `enter` read: handle it in the kernel, KVM_X_AGAIN, or
 * give it to the run loop. Each exit the processor gives with these
 * controls has its case. 0, or -errno to end KVM_RUN.
 */
static int exit_reason(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    struct vmx_exit *e = &x->ex;
    uint32_t reason;

    if (v->emul.resume) {
        return kvm_emul_resume(v);
    }
    if (e->failed != VMX_OK) {
        fail_entry(v, e->insn_error);
        return 0;
    }
    reason = e->reason;
    if (reason & EXIT_FAILED_ENTRY) {
        fail_entry(v, reason);
        return 0;
    }
    complete_events(v);
    v->if_flag = (v->regs.rflags & RFLAGS_IF) != 0;
    v->irq_allowed = v->if_flag && !(e->interruptibility & (INTR_STI | INTR_MOV_SS));
    v->exit.kind = KVM_X_AGAIN;
    reason &= 0xFFFFu;
    if (exit_insn(v, reason)) {
        return 0;
    }
    switch (reason) {
    case EXIT_EXCEPTION_NMI:
        return exit_exception(v);
    case EXIT_EXTERNAL_INTR:
        v->at_boundary = 1;
        return 0;
    case EXIT_PREEMPTION_TIMER:
    case EXIT_INIT_SIGNAL:
    case EXIT_SIPI:
        return 0;
    case EXIT_TRIPLE_FAULT:
        v->exit.kind = KVM_X_SHUTDOWN;
        return 0;
    case EXIT_INTR_WINDOW:
        x->proc_want &= ~PROC_INTR_WINDOW;
        v->exit.kind = KVM_X_IRQ_WINDOW;
        return 0;
    case EXIT_NMI_WINDOW:
        x->proc_want &= ~PROC_NMI_WINDOW;
        kvm_vcpu_kick(v, KVM_REQ_EVENT);
        return 0;
    case EXIT_CR_ACCESS:
        return exit_cr(v);
    case EXIT_DR_ACCESS:
        return exit_dr(v);
    case EXIT_IO:
        return exit_io(v);
    case EXIT_MSR_READ:
        return exit_msr(v, 0);
    case EXIT_MSR_WRITE:
        return exit_msr(v, 1);
    case EXIT_EPT_VIOLATION:
        return exit_ept(v);
    case EXIT_TASK_SWITCH:
        return exit_task_switch(v);
    default:
        break;
    }
    unexpected(v);
    return 0;
}

/**
 * The backend's `exit`, under the big lock with interrupts on: the first
 * failed VMWRITE, INVEPT or INVVPID reported, once, the exit named, and an
 * instruction the kernel stepped under the caller's single step ended as
 * KVM_EXIT_DEBUG with DR6.BS, as Linux's kvm_vcpu_do_singlestep. 0, or
 * -errno to end KVM_RUN.
 */
static int vmx_exit(struct kvm_vcpu *v)
{
    struct kvm_run_debug *d = &v->exit.debug;
    int r;

    if (failed_insn && !failed_told) {
        failed_told = 1;
        klog(KLOG_WARN, "kvm", "vmx: %s failed: 0x%lx error %u", failed_insn,
             (unsigned long)failed_arg, failed_error);
    }
    v->emul.step_trap = 0;
    r = exit_reason(v);
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
static void vmx_complete(struct kvm_vcpu *v)
{
    (void)kvm_emul_complete(v);
}

/** Write back the parts `parts` of the guest's state: the special
 * registers and DR7 from the VMCS, read in a section of their own when the
 * last exit did not, and KERNEL_GS_BASE from the MSR list. The rest the
 * record holds already. */
static void vmx_save(struct kvm_vcpu *v, uint32_t parts)
{
    struct vmx_vcpu *x = vx(v);
    uint64_t cr4;
    uint32_t flags;
    int cpu;

    if ((parts & (KVM_PART_SREGS | KVM_PART_DEBUG)) && !x->fresh && x->built) {
        flags = kirq_save();
        cpu = (int)arch_cpu_id();
        if (vmx_on(x, cpu, &cr4) == 0) {
            read_sregs(v);
            vmx_off(x, cpu, cr4);
        }
        kirq_restore(flags);
    }
    if (parts & KVM_PART_MSRS) {
        kgs_sync(v);
    }
}

/** Whether `v` can take an NMI at the next entry: no NMI blocking, and no
 * STI or MOV SS shadow, as Linux's vmx_nmi_blocked. */
static int nmi_open(const struct kvm_vcpu *v)
{
    return !v->events.nmi.masked && !v->events.interrupt.shadow;
}

/** Whether `v` can take an interrupt at the next entry: RFLAGS.IF and no
 * shadow, as Linux's vmx_interrupt_blocked. */
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

/** The interruption information of the exception `v` has injected or
 * pending, for the next entry, with its error code and, for INT3 and INTO,
 * its length: `len` when the run gave the instruction's, else 1. */
static uint32_t inject_exception(struct kvm_vcpu *v, uint32_t len)
{
    struct vmx_vcpu *x = vx(v);
    uint32_t nr = v->events.exception.nr, info = nr | INTR_VALID;

    if (nr == BP_VECTOR || nr == OF_VECTOR) {
        info |= TYPE_SOFT_EXC << 8;
        x->inj_len = len ? len : 1u;
    } else {
        info |= TYPE_HW_EXC << 8;
    }
    kvm_exception_rf(v);
    /* No error code in real mode, which the entry refuses, as Linux's
     * kvm_inject_exception drops it outside protected mode. */
    if ((v->events.exception.has_error_code || v->events.exception.pending) && has_error_code(nr) &&
        (v->sregs.cr0 & CR0_PE)) {
        info |= INTR_ERRCODE;
        x->inj_error = v->events.exception.error_code;
    }
    v->events.exception.injected = 1;
    v->events.exception.pending = 0;
    return info;
}

/** With the local APIC in the kernel and no other event to inject, the
 * interruption information of the vector the APIC gives when `v` can take
 * it, 0 otherwise, with the window exit armed while one waits. */
static uint32_t inject_lapic(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    int vec = kvm_lapic_pending(v);

    if (vec < 0) {
        return 0;
    }
    if (!irq_open(v)) {
        x->proc_want |= PROC_INTR_WINDOW;
        return 0;
    }
    vec = kvm_lapic_ack(v);
    if (vec < 0) {
        return 0;
    }
    v->events.interrupt.injected = 1;
    v->events.interrupt.nr = (uint8_t)vec;
    v->events.interrupt.soft = 0;
    return (uint32_t)vec | INTR_VALID;
}

/**
 * Choose the event the next entry injects, as Linux's
 * kvm_check_and_inject_events orders them: an exception, then an NMI, then
 * an interrupt, the one the run was delivering first; with the local APIC
 * in the kernel the vector it gives when the guest can take it. An NMI or
 * an interrupt the guest cannot take now arms the processor's window exit.
 */
static void vmx_inject(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);
    uint32_t info = 0, len = x->inj_len;
    int split = v->vm->irqchip != KVM_IRQCHIP_NONE;
    /* No new NMI or interrupt while the caller's KVM_GUESTDBG_BLOCKIRQ
     * holds them, as Linux's kvm_check_and_inject_events. */
    int block = debugging(v, KVM_GUESTDBG_BLOCKIRQ);

    x->proc_want &= ~(PROC_INTR_WINDOW | PROC_NMI_WINDOW);
    x->inj_info = 0;
    x->inj_error = 0;
    x->inj_len = 0;
    if ((v->dirty & KVM_PART_SREGS) && pae_paging(v)) {
        /* KVM_SET_SREGS's PAE state, whose PDPTEs the entry loads from the
         * VMCS: read them now, as Linux's __set_sregs does. */
        (void)pdpte_load(v);
    }
    if (v->events.exception.injected || v->events.exception.pending) {
        info = inject_exception(v, len);
    } else if (v->events.nmi.injected) {
        info = NMI_VECTOR | (TYPE_NMI << 8) | INTR_VALID;
    } else if (v->nmi_queued && nmi_open(v) && !block) {
        /* The delivery blocks NMIs, which the exit's interruptibility
         * then shows; blocking set before the entry would fail it. */
        v->nmi_queued--;
        v->events.nmi.injected = 1;
        info = NMI_VECTOR | (TYPE_NMI << 8) | INTR_VALID;
    } else if (v->events.interrupt.injected) {
        info = v->events.interrupt.nr | INTR_VALID;
        if (v->events.interrupt.soft) {
            info |= TYPE_SOFT_INTR << 8;
            x->inj_len = len ? len : 2u;
        }
    }
    if (v->nmi_queued && (info || !nmi_open(v))) {
        x->proc_want |= PROC_NMI_WINDOW;
    }
    if (split && info) {
        if (kvm_lapic_pending(v) >= 0) {
            x->proc_want |= PROC_INTR_WINDOW;
        }
    } else if (split && !block) {
        info = inject_lapic(v);
    }
    x->inj_info = info;
    v->dirty |= KVM_PART_EVENTS;
}

/** Single stepping and event injection are handled by the backend. */
static int32_t vmx_check_extension(long cap)
{
    return cap == KVM_CAP_X86_ROBUST_SINGLESTEP ? 1 : 0;
}

/** Make the VPID of a new vCPU: 1 to 65535, or 0, the host's, which
 * leaves the guest's translations to be dropped at every entry and exit,
 * when all are taken. */
static uint16_t vpid_alloc(void)
{
    uint32_t i;

    if (!(caps.proc2 & PROC2_VPID)) {
        return 0;
    }
    for (i = 1; i < 65536u; i++) {
        if (!(vpid_used[i / 8u] & (1u << (i % 8u)))) {
            vpid_used[i / 8u] |= (uint8_t)(1u << (i % 8u));
            return (uint16_t)i;
        }
    }
    return 0;
}

/** Give a VPID back. */
static void vpid_free(uint16_t vpid)
{
    if (vpid) {
        vpid_used[vpid / 8u] &= (uint8_t)~(1u << (vpid % 8u));
    }
}

/** The backend's share of a new VM: the VMXON regions of the processors,
 * made at the first VM and kept. 0 or -ENOMEM. */
static int vmx_vm_init(struct kvm_vm *vm)
{
    uintptr_t pa;
    uint32_t i, ncpu = (uint32_t)smp_cpu_count();

    vm->backend = NULL;
    for (i = 0; i < ncpu && i < NR_CPUS; i++) {
        if (vmxon_region[i]) {
            continue;
        }
        vmxon_region[i] = kalloc_pages(1, &pa);
        if (!vmxon_region[i]) {
            return -ENOMEM;
        }
        k_memset(vmxon_region[i], 0, 4096);
        *(uint32_t *)vmxon_region[i] = caps.revision;
        vmxon_pa[i] = pa;
    }
    return 0;
}

/**
 * Have the guest of `v` reach FS_BASE, GS_BASE and KERNEL_GS_BASE
 * directly but for the accesses KVM_X86_SET_MSR_FILTER denies, which exit,
 * as Linux's vmx_msr_filter_changed. The high MSRs' read bitmap is at byte
 * 1024 and their write bitmap at 3072, where the three are bits 0x100 to
 * 0x102.
 */
static void vmx_msr_filter_changed(struct kvm_vcpu *v)
{
    uint8_t *b = vx(v)->msr_bitmap;
    uint8_t rd = 0, wr = 0;
    uint32_t i;

    for (i = 0; i < 3u; i++) {
        rd |= (uint8_t)((kvm_msr_allowed(v, MSR_FS_BASE + i, 0) ? 0u : 1u) << i);
        wr |= (uint8_t)((kvm_msr_allowed(v, MSR_FS_BASE + i, 1) ? 0u : 1u) << i);
    }
    b[1024 + 0x100 / 8] = (uint8_t)((b[1024 + 0x100 / 8] & ~0x07u) | rd);
    b[3072 + 0x100 / 8] = (uint8_t)((b[3072 + 0x100 / 8] & ~0x07u) | wr);
}

/** Make the backend's share of `v`: the VMCS, the MSR area and bitmap,
 * the host's FPU area and the VPID. 0 or -ENOMEM. */
static int vmx_vcpu_init(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = kzalloc(sizeof(*x));
    uintptr_t pa;

    if (!x) {
        return -ENOMEM;
    }
    v->backend = x;
    x->vmcs = kalloc_pages(1, &pa);
    x->vmcs_pa = pa;
    x->msrs = kalloc_pages(1, &x->msrs_pa);
    x->msr_bitmap = kalloc_pages(1, &x->msr_bitmap_pa);
    x->host_fpu = fpu_area_alloc();
    if (!x->vmcs || !x->msrs || !x->msr_bitmap || !x->host_fpu) {
        return -ENOMEM;
    }
    k_memset(x->vmcs, 0, 4096);
    *(uint32_t *)x->vmcs = caps.revision;
    k_memset(x->msrs, 0, 4096);
    k_memset(x->msr_bitmap, 0xFF, 4096);
    vmx_msr_filter_changed(v);
    x->vpid = vpid_alloc();
    x->last_cpu = -1;
    x->kgs_table = 1;
    x->proc_want = caps.proc & ~(v->hlt_in_guest ? PROC_HLT : 0u);
    return 0;
}

/** Free the backend's share of `v`. */
static void vmx_vcpu_free(struct kvm_vcpu *v)
{
    struct vmx_vcpu *x = vx(v);

    if (!x) {
        return;
    }
    if (x->vmcs) {
        kfree_pages(x->vmcs, 1);
    }
    if (x->msrs) {
        kfree_pages(x->msrs, 1);
    }
    if (x->msr_bitmap) {
        kfree_pages(x->msr_bitmap, 1);
    }
    fpu_area_free(x->host_fpu);
    vpid_free(x->vpid);
    kfree(x);
    v->backend = NULL;
}

/** The control word for the capability MSR `msr`, or its true form when
 * the processor has those: `want` where allowed with the bits that must be
 * set; -1 when a bit of `need` is not allowed. */
static int controls(uint32_t msr, uint32_t true_msr, uint32_t want, uint32_t need, uint32_t *out)
{
    uint64_t cap = vmx_op->rdmsr((vmx_op->rdmsr(MSR_IA32_VMX_BASIC) & BASIC_TRUE) ? true_msr : msr);
    uint32_t c = (want | (uint32_t)cap) & (uint32_t)(cap >> 32);

    *out = c;
    return (c & need) == need ? 0 : -1;
}

/**
 * Whether the processor has VMX as this backend needs it, the firmware
 * leaves it usable, and the controls are chosen: VMX in CPUID,
 * IA32_FEATURE_CONTROL locked with VMX outside SMX on, or not locked, when
 * the backend locks it so; a VMCS of 4 KiB at most, write-back, in 64-bit
 * addresses; EPT of four levels, write-back, with INVEPT; the unrestricted
 * guest; the preemption timer; virtual NMIs; and the exit and entry
 * controls of EFER and PAT. VPID, RDTSCP, INVPCID, WBINVD exiting, XSAVES
 * and TSC scaling are taken where they are. 1 or 0.
 */
static int vmx_probe(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    uint64_t basic, fc, misc;
    uint32_t pin_need = PIN_EXT_INTR | PIN_NMI | PIN_VIRT_NMI | PIN_PREEMPTION;
    uint32_t proc_need = PROC_HLT | PROC_CR8_LOAD | PROC_CR8_STORE | PROC_MOV_DR | PROC_UNCOND_IO |
                         PROC_TSC_OFFSET | PROC_MWAIT | PROC_MONITOR | PROC_RDPMC |
                         PROC_MSR_BITMAP | PROC_SECONDARY;
    uint32_t proc2_need = PROC2_EPT | PROC2_UG;
    uint32_t exit_need = EXIT_HOST_64 | EXIT_SAVE_DEBUG | EXIT_LOAD_PAT | EXIT_LOAD_EFER;
    uint32_t entry_need = ENTRY_LOAD_DEBUG | ENTRY_LOAD_PAT | ENTRY_LOAD_EFER;
    uint32_t forbid, windows, perf_exit, perf_entry;

#ifndef RACH2_HOSTTEST
    vmx_op = &vmx_hw;
#endif
    kcpu_id(1, &a, &b, &c, &d);
    if (!(c & (1u << 5))) {
        return 0;
    }
    fc = vmx_op->rdmsr(MSR_IA32_FEATURE_CONTROL);
    if ((fc & FC_LOCKED) && !(fc & FC_VMX_OUTSIDE_SMX)) {
        return 0;
    }
    basic = vmx_op->rdmsr(MSR_IA32_VMX_BASIC);
    if (BASIC_SIZE(basic) > 4096u || ((basic >> 50) & 0xFu) != 6u || (basic & BASIC_32BIT)) {
        return 0;
    }
    k_memset(&caps, 0, sizeof(caps));
    caps.revision = (uint32_t)basic & 0x7FFFFFFFu;
    caps.ins_outs = (basic & BASIC_INS_OUTS) != 0;
    if (controls(MSR_IA32_VMX_PINBASED, MSR_IA32_VMX_TRUE_PIN, pin_need, pin_need, &caps.pin) ||
        controls(MSR_IA32_VMX_PROCBASED, MSR_IA32_VMX_TRUE_PROC, proc_need, proc_need,
                 &caps.proc)) {
        return 0;
    }
    /* With EPT, neither CR3 nor INVLPG exits, as Linux clears them; the
     * window exits are set only while an event waits, so they must be
     * allowed and not forced. */
    forbid = PROC_CR3_LOAD | PROC_CR3_STORE | PROC_INVLPG | PROC_INTR_WINDOW | PROC_NMI_WINDOW;
    if ((caps.proc & forbid) ||
        controls(MSR_IA32_VMX_PROCBASED, MSR_IA32_VMX_TRUE_PROC, PROC_INTR_WINDOW | PROC_NMI_WINDOW,
                 PROC_INTR_WINDOW | PROC_NMI_WINDOW, &windows)) {
        return 0;
    }
    caps.proc2 = (uint32_t)(vmx_op->rdmsr(MSR_IA32_VMX_PROCBASED2) >> 32) &
                 (PROC2_EPT | PROC2_UG | PROC2_VPID | PROC2_RDTSCP | PROC2_INVPCID | PROC2_WBINVD |
                  PROC2_XSAVES | PROC2_TSC_SCALING);
    if ((caps.proc2 & proc2_need) != proc2_need ||
        controls(MSR_IA32_VMX_EXIT, MSR_IA32_VMX_TRUE_EXIT, exit_need, exit_need, &caps.exit) ||
        controls(MSR_IA32_VMX_ENTRY, MSR_IA32_VMX_TRUE_ENTRY, entry_need, entry_need,
                 &caps.entry)) {
        return 0;
    }
    caps.perf_global = !controls(MSR_IA32_VMX_EXIT, MSR_IA32_VMX_TRUE_EXIT, EXIT_LOAD_PERF,
                                 EXIT_LOAD_PERF, &perf_exit) &&
                       !controls(MSR_IA32_VMX_ENTRY, MSR_IA32_VMX_TRUE_ENTRY, ENTRY_LOAD_PERF,
                                 ENTRY_LOAD_PERF, &perf_entry);
    caps.ept_vpid = vmx_op->rdmsr(MSR_IA32_VMX_EPT_VPID);
    if (!(caps.ept_vpid & EPT_CAP_WALK4) || !(caps.ept_vpid & EPT_CAP_WB) ||
        !(caps.ept_vpid & EPT_CAP_INVEPT) ||
        !(caps.ept_vpid & (EPT_CAP_INVEPT_CTX | EPT_CAP_INVEPT_ALL))) {
        return 0;
    }
    if (!(caps.ept_vpid & VPID_CAP_INVVPID) ||
        !(caps.ept_vpid & (VPID_CAP_INVVPID_CTX | VPID_CAP_INVVPID_ALL))) {
        caps.proc2 &= ~PROC2_VPID;
    }
    caps.ad = (caps.ept_vpid & EPT_CAP_AD) != 0;
    misc = vmx_op->rdmsr(MSR_IA32_VMX_MISC);
    caps.timer_shift = (uint32_t)(misc & 0x1Fu);
    caps.cr0_fixed0 = vmx_op->rdmsr(MSR_IA32_VMX_CR0_FIXED0);
    caps.cr0_fixed1 = vmx_op->rdmsr(MSR_IA32_VMX_CR0_FIXED1);
    caps.cr4_fixed0 = vmx_op->rdmsr(MSR_IA32_VMX_CR4_FIXED0);
    caps.cr4_fixed1 = vmx_op->rdmsr(MSR_IA32_VMX_CR4_FIXED1);
    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000001u) {
        kcpu_id(0x80000001u, &a, &b, &c, &d);
        caps.rdtscp = (d >> 27) & 1u;
    }
    k_panic_hook = vmx_panic;
    return 1;
}

/** The bits of a memory type in an EPT leaf: the type in bits 3 to 5, and
 * IPAT, bit 6, when the guest's PAT is ignored. */
static uint64_t vmx_cache(int type, int ignore_guest_pat)
{
    return ((uint64_t)type << 3) | (ignore_guest_pat ? 0x40u : 0u);
}

/** `kvm_backend`'s xsaves: whether the processor has the secondary
 * control that lets a guest run XSAVES and XRSTORS, as Linux clears
 * XSAVES from what it offers without it. */
static int vmx_xsaves(void)
{
    return (caps.proc2 & PROC2_XSAVES) != 0;
}

/** `kvm_backend`'s tsc_scaling: the TSC multiplier's largest value and
 * its 48 bits of fraction, or 0 on a processor without the control. */
static uint64_t vmx_tsc_scaling(uint32_t *frac_bits)
{
    *frac_bits = TSC_RATIO_BITS;
    return (caps.proc2 & PROC2_TSC_SCALING) ? ~0ull : 0;
}

/* EPT's format: read, write and execute, PS, and the accessed and dirty
 * bits of EPT's A/D. */
static const struct kvm_tdp_format vmx_tdp = {
    0x7u, 0x1u, 0x2u, 0x4u, 0, 0, 0x80u, 0x000FFFFFFFFFF000ull, 0x100u, 0x200u, vmx_cache};

const struct kvm_backend kvm_vmx = {"vmx",     vmx_probe,       vmx_vm_init,
                                    NULL,      vmx_vcpu_init,   vmx_vcpu_free,
                                    vmx_enter, vmx_exit,        vmx_complete,
                                    vmx_save,  vmx_inject,      vmx_check_extension,
                                    &vmx_tdp,  vmx_tsc_scaling, vmx_msr_filter_changed,
                                    vmx_xsaves};
