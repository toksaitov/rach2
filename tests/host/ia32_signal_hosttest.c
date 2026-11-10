/**
 * Check the i386 signal frames, siginfo and ptrace registers of the x86-64
 * kernel, `arch/x86_64/sigframe32.c` and `arch/x86_64/ptrace.c`, against
 * Linux 6.12's layouts, over a model of user memory, the selectors and the
 * floating-point state, compiled with x86-64's `cpu.h`.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_X86_64 -DFPU_X86_64
 *
 * The offsets are those of Linux's `sigcontext_32`, `ucontext_ia32`,
 * `rt_sigframe_ia32`, `sigframe_ia32`, `compat_siginfo` and
 * `user_regs_struct32`, as arch/x86/include/asm/sigframe.h,
 * arch/x86/include/uapi/asm/sigcontext.h, include/linux/compat.h and
 * arch/x86/include/asm/user32.h give them.
 */
#include "arch/x86_64/sigframe32.c"
#include "arch/x86_64/ptrace.c"
#include "arch/x86_64/ia32_core.c"
#include "lib/kstring.h"

extern int printf(const char *format, ...);

#ifndef offsetof
#define offsetof(type, member) ((size_t)&((type *)0)->member)
#endif

struct percpu cpu_table[NR_CPUS];
static process_t proc, tracee;
static isr_params_t tracee_frame;
static int failures;

/* The model's user memory: USER_LEN bytes from USER_BASE, below 4 GiB. */
#define USER_BASE 0x40000000ul
#define USER_LEN  0x10000ul
static uint8_t user[USER_LEN];
static uintptr_t memory_base = USER_BASE;
static int ldt_loaded;

/* The model's selectors, MSRs and floating-point calls. */
static unsigned int fs_sel, gs_sel, ds_sel, es_sel;
static uint64_t fs_base, kernel_gs_base;
static int fpu_resets, fpu_loads, fpu_puts;
static uintptr_t vdso_rt, vdso_plain;
static int compat;

/* What the i386 frame's state costs in the model: an fxsave image. */
#define FP_BYTES 624u

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("ia32-signal-hosttest: fail: %s\n", row);
        failures++;
    }
}

/** The model's byte at user address `a` for `n` bytes, or NULL. */
static uint8_t *at(uintptr_t a, size_t n)
{
    if (a < memory_base || a + n > memory_base + USER_LEN || a + n < a) {
        return NULL;
    }
    return user + (a - memory_base);
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    uint8_t *p = at((uintptr_t)src, n);

    if (!p) {
        return -1;
    }
    k_memcpy(dst, p, n);
    return 0;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uint8_t *p = at((uintptr_t)dst, n);

    if (!p) {
        return -1;
    }
    k_memcpy(p, src, n);
    return 0;
}

/** The 32-bit word at user address `a`. */
static uint32_t word_at(uintptr_t a)
{
    uint32_t v = 0;
    uint8_t *p = at(a, 4);

    if (p) {
        k_memcpy(&v, p, 4);
    }
    return v;
}

/** Store `v` at user address `a`. */
static void word_put(uintptr_t a, uint32_t v)
{
    uint8_t *p = at(a, 4);

    if (p) {
        k_memcpy(p, &v, 4);
    }
}

uint64_t msr_read(uint32_t msr)
{
    return msr == MSR_FS_BASE ? fs_base : kernel_gs_base;
}

void msr_write(uint32_t msr, uint64_t value)
{
    if (msr == MSR_FS_BASE) {
        fs_base = value;
    } else {
        kernel_gs_base = value;
    }
}

unsigned int x86_64_fs_read(void)
{
    return fs_sel;
}

unsigned int x86_64_gs_read(void)
{
    return gs_sel;
}

unsigned int x86_64_ds_read(void)
{
    return ds_sel;
}

unsigned int x86_64_es_read(void)
{
    return es_sel;
}

void x86_64_fs_load(unsigned int sel)
{
    fs_sel = sel;
    fs_base = 0xBAD;
}

void x86_64_gs_load(unsigned int sel)
{
    gs_sel = sel;
    kernel_gs_base = 0xBAD;
}

void x86_64_ds_load(unsigned int sel)
{
    ds_sel = sel;
}

void x86_64_es_load(unsigned int sel)
{
    es_sel = sel;
}

/** `lar` in the model: the GDT's user code and data descriptors at DPL 3,
 * and nothing else. */
unsigned int x86_64_lar(unsigned int sel)
{
    switch (sel) {
    case 15:
        return ldt_loaded ? 0x00C0F300u : 0;
    case GDT_USER_DATA:
        return 0x00C0F300u;
    case GDT_USER_CODE32:
        return 0x00CFFB00u;
    case GDT_USER_CODE:
        return 0x00AFFB00u;
    default:
        return 0;
    }
}

/** Publish the modeled LDT before a non-faulting selector check. */
void ldt_switch(const struct mm *mm)
{
    (void)mm;
    ldt_loaded = 1;
}

uintptr_t fpu_frame32_place(uintptr_t sp)
{
    return ((sp - 512u) & ~(uintptr_t)63) - 112u;
}

uintptr_t fpu_frame32_bytes(void)
{
    return FP_BYTES;
}

int fpu_frame32_put(uintptr_t fp, const void *area, uint32_t cs, uint32_t ds)
{
    (void)area;
    (void)cs;
    (void)ds;
    fpu_puts++;
    return at(fp, 112) ? 0 : -1;
}

int fpu_frame32_load(uintptr_t fp)
{
    fpu_loads++;
    return at(fp, 112) ? 0 : -1;
}

uint32_t fpu_frame_uc_flags(void)
{
    return 0;
}

void fpu_save(void *area)
{
    (void)area;
}

void fpu_reset(void)
{
    fpu_resets++;
}

void fpu_env32_get(const void *area, uint8_t *env, uint32_t cs, uint32_t ds)
{
    (void)area;
    k_memset(env, 0, FPU_ENV_SIZE);
    k_memcpy(env + 16, &cs, sizeof(cs));
    k_memcpy(env + 24, &ds, sizeof(ds));
}

void fpu_env32_set(void *area, const uint8_t *env)
{
    (void)area;
    (void)env;
}

void fpu_fx_get(const void *area, uint8_t *fx)
{
    (void)area;
    k_memset(fx, 0, FX_SIZE);
}

int fpu_fx_set(void *area, const uint8_t *fx)
{
    (void)area;
    (void)fx;
    return 0;
}

uint32_t fpu_xstate_size(void)
{
    return 0;
}

void fpu_xstate_view(uint8_t *copy)
{
    (void)copy;
}

int fpu_xstate_set(void *area, const uint8_t *in)
{
    (void)area;
    (void)in;
    return 0;
}

uintptr_t ia32_vdso_sigreturn(int rt)
{
    return rt ? vdso_rt : vdso_plain;
}

int sigframe_on_altstack(uintptr_t sp)
{
    return current_proc->altstack_on && sp > current_proc->altstack_sp &&
           sp <= current_proc->altstack_sp + current_proc->altstack_size;
}

int sigframe_ss_valid(unsigned int sel)
{
    return sel == GDT_USER_DATA;
}

/* The test writes no LDT, so no selector names a code segment of one. */
int ldt_code_valid(unsigned int sel)
{
    (void)sel;
    return 0;
}

int in_compat_syscall(void)
{
    return compat;
}

isr_params_t *proc_user_frame(const process_t *t)
{
    (void)t;
    return &tracee_frame;
}

process_t *ptrace_tracee(uint32_t pid)
{
    (void)pid;
    return &tracee;
}

/** Debug register `n` reads as 0xD0 + n in the model. */
unsigned long debugreg_peek(const process_t *t, unsigned n)
{
    (void)t;
    return 0xD0u + n;
}

int debugreg_poke(process_t *t, unsigned n, unsigned long v)
{
    (void)t;
    (void)n;
    (void)v;
    return 0;
}

uint32_t core_note_bytes(uint32_t descsz)
{
    return descsz;
}

void core_note_put(uint8_t **p, const char *name, uint32_t type, const void *desc, uint32_t descsz)
{
    (void)name;
    (void)type;
    k_memcpy(*p, desc, descsz);
    *p += descsz;
}

int tls_desc_valid(const struct user_desc *u)
{
    (void)u;
    return 1;
}

int tls_entry_get(const struct process *t, uint32_t entry, struct user_desc *u)
{
    (void)t;
    (void)entry;
    k_memset(u, 0, sizeof(*u));
    return 0;
}

int tls_entry_set(struct process *t, uint32_t entry, const struct user_desc *u)
{
    t->regs.tls[entry - GDT_TLS_MIN] = u->base_addr;
    return 0;
}

/** No active TLS note in this model. */
uint32_t ia32_tls_get(const process_t *t, struct user_desc *u)
{
    (void)t;
    (void)u;
    return 0;
}

/* The model's LDT: entry N of LDT_MODEL_ENTRIES has the base
 * LDT_MODEL_BASE + N, and a selector past them none. */
#define LDT_MODEL_ENTRIES 16u
#define LDT_MODEL_BASE    0x1D70000u

/** A TLS entry's base as `tls_entry_set` stored it, or the model LDT's. */
uintptr_t tls_selector_base(const struct process *t, unsigned int sel)
{
    unsigned int entry = sel >> 3;

    if ((sel & 4u) != 0) {
        return entry < LDT_MODEL_ENTRIES ? LDT_MODEL_BASE + entry : 0;
    }
    return entry >= GDT_TLS_MIN && entry <= GDT_TLS_MAX
               ? (uintptr_t)t->regs.tls[entry - GDT_TLS_MIN]
               : 0;
}

/* A size or offset and Linux's value of it. */
struct layout_row {
    size_t got, want;
    const char *what;
};

#define ROW(got, want) {(size_t)(got), (size_t)(want), #got}

static const struct layout_row layout[] = {
    ROW(sizeof(struct sigcontext32), 88),
    ROW(offsetof(struct sigcontext32, gs), 0),
    ROW(offsetof(struct sigcontext32, ds), 12),
    ROW(offsetof(struct sigcontext32, edi), 16),
    ROW(offsetof(struct sigcontext32, eax), 44),
    ROW(offsetof(struct sigcontext32, trapno), 48),
    ROW(offsetof(struct sigcontext32, eip), 56),
    ROW(offsetof(struct sigcontext32, cs), 60),
    ROW(offsetof(struct sigcontext32, eflags), 64),
    ROW(offsetof(struct sigcontext32, esp_at_signal), 68),
    ROW(offsetof(struct sigcontext32, ss), 72),
    ROW(offsetof(struct sigcontext32, fpstate), 76),
    ROW(offsetof(struct sigcontext32, oldmask), 80),
    ROW(offsetof(struct sigcontext32, cr2), 84),
    ROW(sizeof(struct ucontext32), 116),
    ROW(offsetof(struct ucontext32, ss_sp), 8),
    ROW(offsetof(struct ucontext32, ss_flags), 12),
    ROW(offsetof(struct ucontext32, ss_size), 16),
    ROW(offsetof(struct ucontext32, mc), 20),
    ROW(offsetof(struct ucontext32, sigmask), 108),
    ROW(sizeof(struct rt_sigframe32), 268),
    ROW(offsetof(struct rt_sigframe32, sig), 4),
    ROW(offsetof(struct rt_sigframe32, pinfo), 8),
    ROW(offsetof(struct rt_sigframe32, puc), 12),
    ROW(offsetof(struct rt_sigframe32, info), 16),
    ROW(offsetof(struct rt_sigframe32, uc), 144),
    ROW(offsetof(struct rt_sigframe32, retcode), 260),
    ROW(sizeof(struct sigframe32), 732),
    ROW(offsetof(struct sigframe32, sc), 8),
    ROW(offsetof(struct sigframe32, fpstate_unused), 96),
    ROW(offsetof(struct sigframe32, extramask), 720),
    ROW(offsetof(struct sigframe32, retcode), 724),
    ROW(R32_EBX * 4, 0),
    ROW(R32_ECX * 4, 4),
    ROW(R32_EDX * 4, 8),
    ROW(R32_ESI * 4, 12),
    ROW(R32_EDI * 4, 16),
    ROW(R32_EBP * 4, 20),
    ROW(R32_EAX * 4, 24),
    ROW(R32_DS * 4, 28),
    ROW(R32_ES * 4, 32),
    ROW(R32_FS * 4, 36),
    ROW(R32_GS * 4, 40),
    ROW(R32_ORIG_EAX * 4, 44),
    ROW(R32_EIP * 4, 48),
    ROW(R32_CS * 4, 52),
    ROW(R32_EFLAGS * 4, 56),
    ROW(R32_ESP * 4, 60),
    ROW(R32_SS * 4, 64),
    ROW(R32_COUNT * 4, 68),
    ROW(USER32_DEBUGREG, 252),
    ROW(USER32_SIZE, 284),
};

/** The structures' sizes and offsets are Linux's. */
static void run_layout(void)
{
    size_t i;

    for (i = 0; i < sizeof(layout) / sizeof(layout[0]); i++) {
        check(layout[i].got == layout[i].want, layout[i].what);
    }
}

/** Whether the `n` bytes at `a` and `b` are equal; `a` may be NULL. */
static int same(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i;

    if (!a) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/** The word at byte `off` of the siginfo `si`. */
static uint32_t si_word(const uint8_t *si, unsigned off)
{
    return get32(si + off);
}

/** compat_siginfo: signo, errno and code, then the union at byte 12. */
static void run_siginfo(void)
{
    uint8_t si[SIGINFO_SIZE];
    struct k_siginfo info, back;

    k_memset(&info, 0, sizeof(info));
    info.code = 0; /* SI_USER */
    info.pid = 41;
    info.uid = 1000;
    ia32_fill_siginfo(si, SIGUSR1, &info);
    check(si_word(si, 0) == SIGUSR1 && si_word(si, 8) == 0 && si_word(si, 12) == 41 &&
              si_word(si, 16) == 1000,
          "kill's pid at 12 and uid at 16");

    info.code = SI_QUEUE;
    info.value = 0x1234567890ul;
    ia32_fill_siginfo(si, 40, &info);
    check(si_word(si, 20) == 0x34567890u, "sigqueue's value at 20, its low half");
    ia32_take_siginfo(si, &back);
    check(back.code == SI_QUEUE && back.pid == 41 && back.uid == 1000 && back.value == 0x34567890u,
          "sigqueue's siginfo read back");

    k_memset(&info, 0, sizeof(info));
    info.code = CLD_EXITED;
    info.pid = 7;
    info.uid = 5;
    info.status = 3;
    ia32_fill_siginfo(si, SIGCHLD, &info);
    check(si_word(si, 12) == 7 && si_word(si, 16) == 5 && si_word(si, 20) == 3,
          "SIGCHLD's pid, uid and status at 12, 16 and 20");

    k_memset(&info, 0, sizeof(info));
    info.code = SEGV_MAPERR;
    info.addr = 0xFFFF0000DEADBEEFul;
    ia32_fill_siginfo(si, SIGSEGV, &info);
    check(si_word(si, 12) == 0xDEADBEEFu && si_word(si, 16) == 0,
          "a fault's address at 12, its low half alone");

    k_memset(&info, 0, sizeof(info));
    info.code = SIGSYS_SECCOMP;
    info.addr = 0x08048123u;
    info.uid = 20;
    info.status = (int32_t)0x40000003u;
    ia32_fill_siginfo(si, SIGSYS, &info);
    check(si_word(si, 12) == 0x08048123u && si_word(si, 16) == 20 && si_word(si, 20) == 0x40000003u,
          "seccomp SIGSYS's call address, number and architecture at 12, 16 and 20");
}

/** Give the running thread an i386 context: its frame `tf` interrupted at
 * EIP 0x08049100 with the stack at `sp`. */
static void setup(isr_params_t *tf, uintptr_t sp)
{
    memory_base = USER_BASE;
    ldt_loaded = 0;
    k_memset(&proc, 0, sizeof(proc));
    k_memset(tf, 0, sizeof(*tf));
    k_memset(user, 0, sizeof(user));
    cpu_table[0].current = &proc;
    tf->rax = 0xFFFFFFFF00000011ul;
    tf->rbx = 0x22;
    tf->rcx = 0x33;
    tf->rdx = 0x44;
    tf->rsi = 0x55;
    tf->rdi = 0x66;
    tf->rbp = 0x77;
    tf->r8 = 0x88;
    tf->rip = 0x08049100u;
    tf->rsp = sp;
    tf->cs = GDT_USER_CODE32;
    tf->ss = GDT_USER_DATA;
    tf->rflags = 0x202u | EFLAGS_DF | EFLAGS_TF;
    ds_sel = es_sel = GDT_USER_DATA;
    fs_sel = 0;
    gs_sel = 0;
    fpu_resets = fpu_loads = fpu_puts = 0;
    vdso_rt = vdso_plain = 0;
}

/** The rt frame: Linux's layout under an i386 stack pointer, its
 * arguments in registers, and the return through rt_sigreturn. */
static void run_rt_frame(void)
{
    isr_params_t tf;
    struct k_sigaction sa;
    struct k_siginfo info;
    uintptr_t sp = USER_BASE + 0x8000u, f;
    uint64_t mask = 0;

    setup(&tf, sp);
    k_memset(&sa, 0, sizeof(sa));
    sa.handler = 0x08049000u;
    sa.flags = SA_SIGINFO | SA_RESTORER;
    sa.restorer = 0x0804A000u;
    k_memset(&info, 0, sizeof(info));
    info.pid = 9;
    check(ia32_sigframe_setup(&tf, SIGUSR2, &sa, &info, 0x8000000100000004ull) == 0,
          "the rt frame is built");
    f = tf.rsp;
    check(((f + 4u) & 15u) == 0, "the rt frame's stack is i386's: 4 past a 16-byte boundary");
    check(f + sizeof(struct rt_sigframe32) <= fpu_frame32_place(sp) && f < sp,
          "the rt frame lies below its floating-point state");
    check(tf.rip == 0x08049000u && tf.rax == SIGUSR2 && tf.rdx == f + 16u && tf.rcx == f + 144u &&
              tf.cs == GDT_USER_CODE32 && tf.ss == GDT_USER_DATA,
          "the handler's EIP, EAX, EDX and ECX and its segments");
    check((tf.rflags & (EFLAGS_DF | EFLAGS_TF)) == 0, "DF and TF cleared for the handler");
    check(word_at(f) == 0x0804A000u && word_at(f + 4) == SIGUSR2 && word_at(f + 8) == f + 16u &&
              word_at(f + 12) == f + 144u,
          "pretcode is SA_RESTORER's, then sig, pinfo and puc");
    check(word_at(f + 16) == SIGUSR2 && word_at(f + 16 + 12) == 9, "the siginfo at byte 16");
    check(word_at(f + 144 + 20 + 44) == 0x11u && word_at(f + 144 + 20 + 32) == 0x22u &&
              word_at(f + 144 + 20 + 56) == 0x08049100u &&
              word_at(f + 144 + 20 + 60) == GDT_USER_CODE32 &&
              word_at(f + 144 + 20 + 76) == fpu_frame32_place(sp) &&
              word_at(f + 144 + 20 + 80) == 4u,
          "the sigcontext's EAX, EBX, EIP, CS, fpstate and oldmask");
    check(word_at(f + 144 + 108) == 4u && word_at(f + 144 + 112) == 0x80000001u,
          "the ucontext's two mask words");
    check(word_at(f + 144 + 12) == SS_DISABLE, "uc_stack's flags without an alternate stack");
    check(fpu_puts == 1 && fpu_resets == 1, "the state saved and the handler's reset");

    /* The handler changes EAX and returns through rt_sigreturn. */
    word_put(f + 144 + 20 + 44, 0x99u);
    tf.rsp = f + 4u;
    tf.rax = 0;
    tf.r8 = 0x1888;
    check(ia32_sigframe_restore(&tf, 1, &mask) == 0, "rt_sigreturn restores the frame");
    check(tf.rax == 0x99u && tf.rbx == 0x22u && tf.rip == 0x08049100u && tf.rsp == sp &&
              tf.cs == GDT_USER_CODE32 && tf.ss == GDT_USER_DATA && tf.r8 == 0x1888,
          "the registers the handler left, zero-extended, and R8 untouched");
    check(mask == 0x8000000100000004ull, "the mask the frame saved");
    check(fpu_loads == 1, "the state loaded from the frame");

    /* A code selector that is not a user one is refused. */
    tf.rsp = f + 4u;
    word_put(f + 144 + 20 + 60, 0x10u);
    check(ia32_sigframe_restore(&tf, 1, &mask) == -1, "a kernel CS in the frame is refused");
}

/** The plain frame: its retcode as the return without SA_RESTORER, the
 * vDSO's when there is one, and extramask. */
static void run_plain_frame(void)
{
    isr_params_t tf;
    struct k_sigaction sa;
    struct k_siginfo info;
    uintptr_t sp = USER_BASE + 0x8000u, f;
    uint64_t mask = 0;
    static const uint8_t retcode[8] = {0x58, 0xB8, 0x77, 0x00, 0x00, 0x00, 0xCD, 0x80};

    setup(&tf, sp);
    k_memset(&sa, 0, sizeof(sa));
    sa.handler = 0x08049000u;
    k_memset(&info, 0, sizeof(info));
    check(ia32_sigframe_setup(&tf, SIGINT, &sa, &info, 0x0000000700000002ull) == 0,
          "the plain frame is built");
    f = tf.rsp;
    check(((f + 4u) & 15u) == 0 && tf.rdx == 0 && tf.rcx == 0, "the plain frame's stack");
    check(word_at(f) == f + 724u && same(at(f + 724u, 8), retcode, 8),
          "without a restorer or a vDSO it returns to its own popl, movl, int $0x80");
    check(word_at(f + 8 + 80) == 2u && word_at(f + 720) == 7u,
          "oldmask and extramask hold the mask's halves");

    tf.rsp = f + 8u;
    check(ia32_sigframe_restore(&tf, 0, &mask) == 0 && mask == 0x0000000700000002ull &&
              tf.rsp == sp,
          "sigreturn takes the frame 8 below the stack pointer");

    setup(&tf, sp);
    vdso_plain = 0x0F7F0400u;
    check(ia32_sigframe_setup(&tf, SIGINT, &sa, &info, 0) == 0 && word_at(tf.rsp) == 0x0F7F0400u,
          "without a restorer it returns through the vDSO's");
}

/** SA_ONSTACK's frame goes on the alternate stack; one that would reach
 * 4 GiB is refused. */
static void run_stacks(void)
{
    isr_params_t tf;
    struct k_sigaction sa;
    struct k_siginfo info;
    uintptr_t sp = USER_BASE + 0x8000u;

    setup(&tf, sp);
    proc.altstack_sp = USER_BASE + 0xA000u;
    proc.altstack_size = 0x4000u;
    proc.altstack_on = 1;
    k_memset(&sa, 0, sizeof(sa));
    sa.handler = 0x08049000u;
    sa.flags = SA_SIGINFO | SA_ONSTACK | SA_RESTORER;
    k_memset(&info, 0, sizeof(info));
    check(ia32_sigframe_setup(&tf, SIGUSR1, &sa, &info, 0) == 0 && tf.rsp > USER_BASE + 0xA000u &&
              tf.rsp < USER_BASE + 0xE000u,
          "SA_ONSTACK's frame lies on the alternate stack");
    check(word_at(tf.rsp + 144 + 8) == USER_BASE + 0xA000u && word_at(tf.rsp + 144 + 12) == 0 &&
              word_at(tf.rsp + 144 + 16) == 0x4000u,
          "uc_stack as sigaltstack read it where the signal came");

    setup(&tf, LOW_4G + 0x1000u);
    memory_base = LOW_4G - USER_LEN / 2u;
    sa.flags = SA_SIGINFO | SA_RESTORER;
    check(ia32_sigframe_setup(&tf, SIGUSR1, &sa, &info, 0) == -1 && fpu_puts == 0,
          "a frame above 4 GiB is refused before the modeled memory accepts it");
    setup(&tf, LOW_4G + 512u + 64u);
    memory_base = LOW_4G - USER_LEN / 2u;
    check(fpu_frame32_place(tf.rsp) < LOW_4G &&
              fpu_frame32_place(tf.rsp) + fpu_frame32_bytes() > LOW_4G &&
              ia32_sigframe_setup(&tf, SIGUSR1, &sa, &info, 0) == -1 && fpu_puts == 0,
          "a frame whose floating-point state starts below 4 GiB and ends above it is refused");
    setup(&tf, 0xFFFFFF00u);
    memory_base = LOW_4G - USER_LEN / 2u;
    check(ia32_sigframe_setup(&tf, SIGUSR1, &sa, &info, 0) == 0 && tf.rsp < LOW_4G,
          "a frame ending below 4 GiB is accepted");
    memory_base = USER_BASE;
}

/** A non-flat stack uses the legacy restorer stack under Linux's conditions. */
static void run_legacy_stack(void)
{
    isr_params_t tf;
    struct k_sigaction sa;
    struct k_siginfo info;

    setup(&tf, 0x2000u);
    tf.ss = 15;
    proc.altstack_sp = USER_BASE + 0xA000u;
    proc.altstack_size = 0x4000u;
    proc.altstack_on = 1;
    k_memset(&sa, 0, sizeof(sa));
    k_memset(&info, 0, sizeof(info));
    sa.handler = 0x08049000u;
    sa.restorer = USER_BASE + 0xD000u;
    sa.flags = SA_SIGINFO;
    vdso_rt = 0x0F7F0400u;
    check(ia32_sigframe_setup(&tf, SIGUSR1, &sa, &info, 0) == 0 && tf.rsp > USER_BASE + 0xA000u &&
              tf.rsp < USER_BASE + 0xD000u && word_at(tf.rsp) == vdso_rt,
          "the LDT stack switches to sa_restorer, below the alternate stack's top, and returns "
          "through the vDSO");
    check(!ldt_loaded && seg_from_frame(15) == 15 && ldt_loaded,
          "the data selector check loads a newly published LDT before lar");
    tf.rsp = 0x2000u;
    tf.ss = 15;
    sa.restorer = USER_BASE + 0x9000u;
    check(ia32_sigframe_setup(&tf, SIGUSR1, &sa, &info, 0) == -1,
          "an sa_restorer stack below the alternate stack is refused");
    tf.rsp = 0x2000u;
    tf.ss = 15;
    sa.flags |= SA_RESTORER;
    check(ia32_sigframe_setup(&tf, SIGUSR1, &sa, &info, 0) == -1,
          "SA_RESTORER makes its pointer code rather than a legacy stack");
}

/** An i386 tracer's registers: user_regs_struct32 from PTRACE_GETREGS and
 * PEEKUSER, and the i386 register sets of a 32-bit tracee. */
static void run_ptrace(void)
{
    isr_params_t tf;
    uintptr_t buf = USER_BASE + 0x100u;
    uint32_t unit = 0;

    setup(&tf, USER_BASE + 0x8000u);
    k_memset(&tracee, 0, sizeof(tracee));
    k_memset(&tracee_frame, 0, sizeof(tracee_frame));
    tracee_frame.rbx = 0xAAAA00000000B0B0ul;
    tracee_frame.rax = 0xC0C0u;
    tracee_frame.rip = 0x08049200u;
    tracee_frame.cs = GDT_USER_CODE32;
    tracee_frame.ss = GDT_USER_DATA;
    tracee_frame.rsp = 0xFFFFD000u;
    tracee.orig_nr = 20;
    tracee.regs.ds_sel = GDT_USER_DATA;
    compat = 1;

    check(arch_ptrace(&tracee, 2, PTRACE_GETREGS, 0, buf) == 0, "an i386 tracer's GETREGS");
    check(word_at(buf) == 0xB0B0u && word_at(buf + 24) == 0xC0C0u &&
              word_at(buf + 28) == GDT_USER_DATA && word_at(buf + 44) == 20u &&
              word_at(buf + 48) == 0x08049200u && word_at(buf + 52) == GDT_USER_CODE32 &&
              word_at(buf + 60) == 0xFFFFD000u && word_at(buf + 64) == GDT_USER_DATA &&
              word_at(buf + 68) == 0,
          "user_regs_struct32's 68 bytes, low halves");
    check(arch_ptrace(&tracee, 2, PTRACE_PEEKUSER, 24, buf) == 0 && word_at(buf) == 0xC0C0u,
          "PEEKUSER of EAX at 24");
    check(arch_ptrace(&tracee, 2, PTRACE_PEEKUSER, 256, buf) == 0 && word_at(buf) == 0xD1u,
          "PEEKUSER of u_debugreg[1] at 256");
    check(arch_ptrace(&tracee, 2, PTRACE_PEEKUSER, 280, buf) == 0 && word_at(buf) == 0xD7u,
          "PEEKUSER of u_debugreg[7], struct user32's last word");
    check(arch_ptrace(&tracee, 2, PTRACE_PEEKUSER, 248, buf) == 0 && word_at(buf) == 0,
          "PEEKUSER of a field that is not a register reads 0");
    check(arch_ptrace(&tracee, 2, PTRACE_PEEKUSER, 284, buf) == -EIO &&
              arch_ptrace(&tracee, 2, PTRACE_PEEKUSER, 26, buf) == -EIO,
          "PEEKUSER past struct user32 or unaligned is EIO");
    check(arch_ptrace(&tracee, 2, PTRACE_ARCH_PRCTL, 0, buf) == -EIO,
          "ARCH_PRCTL is not an i386 tracer's");
    compat = 0;

    check(arch_regset_size(&tracee, NT_PRSTATUS, &unit) == 68 && unit == 4,
          "a 32-bit tracee's NT_PRSTATUS is user_regs_struct32");
    check(arch_regset_size(&tracee, NT_PRFPREG, &unit) == 108 &&
              arch_regset_size(&tracee, NT_PRXFPREG, &unit) == 512 &&
              arch_regset_size(&tracee, NT_386_TLS, &unit) == 48 && unit == 16,
          "a 32-bit tracee's NT_PRFPREG, NT_PRXFPREG and NT_386_TLS");
    tracee_frame.cs = GDT_USER_CODE;
    check(arch_regset_size(&tracee, NT_PRSTATUS, &unit) == 27 * 8 && unit == 8,
          "a 64-bit tracee's NT_PRSTATUS is user_regs_struct");
}

/** Ptrace writes SS, refreshes both bases when its TLS set or a TLS entry
 * changes, at each RPL of the selector, and takes a selector's low 16 bits. */
static void run_ptrace_writes(void)
{
    struct user_desc u;
    uint8_t notes[FPU_ENV_SIZE + FX_SIZE], *next = notes;
    uint32_t word;
    unsigned int rpl;

    check(regs32_set(&tracee, R32_SS, 15) == 0 && tracee_frame.ss == 15 &&
              regs32_set(&tracee, R32_SS, 0) == -EIO && tracee_frame.ss == 15,
          "ptrace saves an LDT stack selector and refuses a null one");
    check(regs32_set(&tracee, R32_COUNT, 0) == -EIO &&
              regset32_set(&tracee, NT_PRSTATUS, notes, R32_COUNT * 4u + 4u) == -EINVAL,
          "an oversized register set cannot index past the i386 register map");
    k_memset(&u, 0, sizeof(u));
    for (rpl = 0; rpl < 4; rpl++) {
        u.base_addr = 0x321000u + rpl;
        tracee.regs.fs_sel = tracee.regs.gs_sel = (uint16_t)((GDT_TLS_MIN << 3) | rpl);
        tracee.tls_base = tracee.regs.gs_base = 0xBADu;
        check(regset32_set(&tracee, NT_386_TLS, (const uint8_t *)&u, sizeof(u)) == 0 &&
                  tracee.tls_base == u.base_addr && tracee.regs.gs_base == u.base_addr,
              "NT_386_TLS updates the saved FS and GS bases of a TLS selector at each RPL");
        u.base_addr = 0x654000u + rpl;
        k_memcpy(at(USER_BASE + 0x200u, sizeof(u)), &u, sizeof(u));
        tracee.tls_base = tracee.regs.gs_base = 0xBADu;
        check(arch_ptrace(&tracee, 2, PTRACE_SET_THREAD_AREA, GDT_TLS_MIN, USER_BASE + 0x200u) ==
                      0 &&
                  tracee.tls_base == u.base_addr && tracee.regs.gs_base == u.base_addr,
              "PTRACE_SET_THREAD_AREA updates the saved FS and GS bases at each RPL");
    }
    tracee.tls_base = tracee.regs.gs_base = 0xBADu;
    check(regs32_set(&tracee, R32_FS, 0x0001000Fu) == 0 && tracee.regs.fs_sel == 0x000Fu &&
              tracee.tls_base == LDT_MODEL_BASE + 1u,
          "a FS write with upper bits stores 16 bits and takes LDT entry 1's base");
    check(regs32_set(&tracee, R32_GS, 0x00010063u) == 0 && tracee.regs.gs_sel == 0x0063u &&
              tracee.regs.gs_base == tracee.regs.tls[0],
          "a GS write with upper bits stores 16 bits and takes TLS entry 12's base");
    tracee_frame.cs = 15;
    tracee.regs.ds_sel = 23;
    ia32_core_fp_notes(&next, &tracee);
    k_memcpy(&word, notes + 16, sizeof(word));
    check(word == 15, "the i386 core x87 note uses the tracee's code selector");
    k_memcpy(&word, notes + 24, sizeof(word));
    check(word == 23 && next == notes + sizeof(notes),
          "the i386 core x87 note uses the tracee's data selector");
}

int main(void)
{
    run_layout();
    run_siginfo();
    run_rt_frame();
    run_plain_frame();
    run_stacks();
    run_legacy_stack();
    run_ptrace();
    run_ptrace_writes();
    if (failures) {
        printf("ia32-signal-hosttest: %d failures\n", failures);
        return failures > 255 ? 255 : failures;
    }
    printf("ia32-signal-hosttest: ok\n");
    return 0;
}
