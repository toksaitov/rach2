/*
 * isr.c: the PC's interrupts and exceptions: the handler chains of
 * ../../irq.h, the routers the entry stubs call, the signals of the
 * processor's exceptions, and the routing of lines 0 to 15 through the 8259
 * pair or the I/O APIC.
 */

#include "ioapic.h"
#include "lapic.h"
#include "../../klog.h"
#include "isr.h"
#include "../../panic.h"
#include "../../serial.h"
#include "../../mm/pressure.h"

#include "../../lib/ksignal.h"
#include "../../lib/kstring.h"
#include "../../proc/process.h"
#include "../../proc/ptrace.h"
#include "../../proc/rseq.h"
#include "../../proc/sched.h"
#include "../../proc/smp.h"
#include "../../timer.h"
#include "../../watchdog.h"
#include "debugreg.h"
#if defined(__x86_64__)
#include "../x86_64/sysnr.h"
#else
#include "../i686/sysnr.h"
#include "../i686/tss.h"
#endif

/* ioctl's number in the table of the call in `p`: i386's 54 for a compat
 * call on x86-64, whose names ../x86_64/ia32_sysnr.h would redefine. */
#if defined(__x86_64__) && RACH2_IA32
#define CALL_IOCTL_NR(p) (SC_COMPAT(p) ? 54u : (uint32_t)SYS_IOCTL)
#else
#define CALL_IOCTL_NR(p) ((uint32_t)SYS_IOCTL)
#endif

#include "../../../kernel.h"

#if defined(__x86_64__)
/* The resumption of a faulting load of a selector, and the LDT loaded on a
 * processor that had not loaded it (arch/x86_64/context.c and ldt.c). */
int x86_64_fault_fixup(isr_params_t *p);
int ldt_fault(const isr_params_t *p);
#endif

/* The saved flags register of a frame: EFLAGS, or RFLAGS on x86-64. */
#if defined(__x86_64__)
#define FRAME_FLAGS(p) ((p)->rflags)
#else
#define FRAME_FLAGS(p) ((p)->eflags)
#endif

/* Every interrupt this kernel has routed through `_irq_router`, which is what
 * /proc/stat's `intr` line reports. */
static volatile uint32_t irq_count = 0;

/* One count per vector this kernel takes interrupts on, 32 to ISR_MSI_LAST,
 * which is what /proc/interrupts reports. */
static volatile uint32_t vector_count[ISR_MSI_LAST - 31];

/*
 * Each vector has a short chain of handlers, so that devices that share one
 * line can each install their own. PCI's level-triggered INTx lines are
 * often shared, since a chipset routes many devices' pins to a few lines.
 * On a shared line every chained handler runs and checks its own device's
 * status, so the device that raised the line is acknowledged; a handler
 * that never ran could not clear its device, the line would stay asserted,
 * and the interrupt would repeat without end.
 */
static isr_handler_t _isr_handlers[256][ISR_MAX_VECTOR_HANDLERS];

/* What each handler asked to be called, indexed as the chain is. A name is
 * the only thing /proc/interrupts can say about a line that a number cannot:
 * two devices on one line are two names on it. */
static const char *_isr_names[256][ISR_MAX_VECTOR_HANDLERS];

/*
 * How lines 0 to 15 reach the processor: through the 8259 pair, or through
 * the I/O APIC once `isr_lines_to_apic` has run. With the I/O APIC a line is
 * routed when a handler is registered on its vector, unless a local source
 * raises that vector itself (`lines_local`, the APIC timer on line 0's) or
 * the line is held masked (`lines_held`, the SCI until ACPICA takes it).
 * `lines_pci` are the lines a PCI device's interrupt line register gives and
 * the SCI's, which are level-triggered.
 */
static int lines_apic;
static uint16_t lines_local;
static uint16_t lines_held;
static uint16_t lines_pci;

/* The routers the entry stubs of i686/isr_handler.s and
 * x86_64/isr_handler.s call; x86-64's exceptions and interrupts reach the
 * first two through `x86_64_route` (x86_64/idt.c). */
void _isr_router(isr_params_t *params);
void isr_router_locked(isr_params_t *params);
int isr_call_ioctl(const isr_params_t *params);

void _irq_router(isr_params_t *params);

void isr_local_router(isr_params_t *params);

void isr_leave(isr_params_t *params);

/* The `cli` after the halt in `arch_idle_masked` (cpu.c). */
extern char arch_idle_resume[];

void isr_paranoid_router(isr_params_t *params);

/*
 * The architecturally-defined exception mnemonics, indexed by vector, for the
 * fault diagnostic. Vectors 22-31 are Intel-reserved.
 */
static const char *const _exception_names[32] = {"divide error",
                                                 "debug",
                                                 "non-maskable interrupt",
                                                 "breakpoint",
                                                 "overflow",
                                                 "BOUND range exceeded",
                                                 "invalid opcode",
                                                 "device not available",
                                                 "double fault",
                                                 "coprocessor segment overrun",
                                                 "invalid TSS",
                                                 "segment not present",
                                                 "stack-segment fault",
                                                 "general protection",
                                                 "page fault",
                                                 "reserved",
                                                 "x87 FPU error",
                                                 "alignment check",
                                                 "machine check",
                                                 "SIMD FP exception",
                                                 "virtualization",
                                                 "control protection",
                                                 "reserved",
                                                 "reserved",
                                                 "reserved",
                                                 "reserved",
                                                 "reserved",
                                                 "reserved",
                                                 "reserved",
                                                 "reserved",
                                                 "reserved",
                                                 "reserved"};

/** Whether the processor pushes an error code for exception `vector`: the
 * double fault, the TSS, segment, stack, protection and page faults, the
 * alignment check and the control protection exception. */
static int _has_error_code(uint32_t vector)
{
    return vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 21;
}

/** The si_code of the floating-point exception an FPU status names, as
 * Linux reports it: invalid operation, then division by zero, overflow,
 * underflow (with a denormal operand) and precision, among the flags
 * `status` raised and `mask` does not mask; 0 for none. */
static int32_t _fpe_code(uint32_t status, uint32_t mask)
{
    uint32_t raised = status & ~mask;

    if (raised & 0x01u) {
        return FPE_FLTINV;
    }
    if (raised & 0x04u) {
        return FPE_FLTDIV;
    }
    if (raised & 0x08u) {
        return FPE_FLTOVF;
    }
    if (raised & 0x12u) {
        return FPE_FLTUND;
    }
    if (raised & 0x20u) {
        return FPE_FLTRES;
    }
    return 0;
}

/**
 * Map a CPU exception to the signal and siginfo a user process receives, as
 * Linux's traps do: the precise causes to their signal and code with the
 * faulting instruction's address, and the protection faults to SIGSEGV or
 * SIGBUS with SI_KERNEL. A breakpoint's SIGTRAP is SI_KERNEL too, and a
 * single step's is TRAP_TRACE. A hardware breakpoint the thread no longer
 * has, which a tracer's end left in the processor's DR7, returns 0.
 */
static int _exception_signal(const isr_params_t *params, struct k_siginfo *info)
{
    uint32_t status;
    uint32_t mask;

    k_memset(info, 0, sizeof(*info));
    info->code = SI_KERNEL;
    switch (params->vector) {
    case 0: /* divide error */
        info->code = FPE_INTDIV;
        info->addr = FRAME_PC(params);
        return SIGFPE;
    case 1: /* debug: DR6 says whether a step or a breakpoint */
        info->code = debugreg_status();
        if (info->code == DEBUGREG_STALE) {
            return 0;
        }
        if (info->code == 0) {
            info->code = TRAP_TRACE;
        }
        info->addr = FRAME_PC(params);
        return SIGTRAP;
    case 3: /* breakpoint */
        return SIGTRAP;
    case 6: /* invalid opcode */
        info->code = ILL_ILLOPN;
        info->addr = FRAME_PC(params);
        return SIGILL;
    case 11: /* segment not present */
    case 12: /* stack-segment fault */
        return SIGBUS;
    case 16: /* x87 FPU error */
        __asm__ __volatile__("fnstsw %%ax; movzwl %%ax, %0" : "=r"(status) : : "eax");
        __asm__ __volatile__("fnstcw (%0)" : : "r"(&mask) : "memory");
        info->code = _fpe_code(status, mask & 0x3Fu);
        info->addr = FRAME_PC(params);
        return SIGFPE;
    case 17: /* alignment check */
        info->code = BUS_ADRALN;
        return SIGBUS;
    case 19: /* SIMD FP exception */
        /* `stmxcsr (%eax)` as bytes: the staged TinyCC's assembler lacks it. */
        __asm__ __volatile__(".byte 0x0f, 0xae, 0x18" : : "a"(&status) : "memory");
        info->code = _fpe_code(status & 0x3Fu, (status >> 7) & 0x3Fu);
        info->addr = FRAME_PC(params);
        return SIGFPE;
    default:
        return SIGSEGV;
    }
}

/**
 * The handler of every processor exception without one of its own
 * (`init_isr`).
 *
 * Without a handler the router would return to the faulting instruction,
 * which would fault again without end. A fault taken in user mode (the
 * saved `CS` is at RPL 3) is that process's own, so it gets the signal its
 * cause maps to, forced as Linux forces one: a handler the process
 * installed runs, a tracer takes the stop, and otherwise the process dies
 * and the cause is logged. A fault in kernel mode has no safe recovery and
 * panics.
 *
 * A debug exception in kernel mode, which only i686 takes here, is no
 * fault of the kernel's: a tracer's watchpoint hit by a copy to or from user
 * memory, or by a `MOV SS` that delayed its exception into the system call
 * gate. It is dropped, as Linux drops it (`debugreg_drop`), and the
 * single-step flag is cleared in the saved flags.
 */
static void _exception_handler(isr_params_t *params)
{
    const char *name = params->vector < 32 ? _exception_names[params->vector] : "unknown";

    if (params->vector == 1 && (params->cs & 3) != 3 && current_proc) {
        FRAME_FLAGS(params) &= ~0x100u;
        debugreg_drop();
        return;
    }

    if ((params->cs & 3) == 3) {
        struct k_siginfo info;
        int signo;

#if defined(__x86_64__)
        if (ldt_fault(params)) {
            return;
        }
#endif
        signo = _exception_signal(params, &info);

        if (signo == 0) {
            return;
        }
        current_proc->trap_nr = params->vector;
        current_proc->trap_err = params->error_code;
        current_proc->trap_addr = 0;
        signal_force(current_proc, signo, &info);
        if (current_proc->sigacts[signo].handler == KSIG_DFL &&
            !(current_proc->ptrace & PT_TRACED)) {
#if defined(__x86_64__)
            klog(KLOG_WARN, "isr", "user exception: %s (vector %d, err 0x%x, rip 0x%lx): signal %d",
                 name, (uint32_t)params->vector, (uint32_t)params->error_code,
                 (unsigned long)params->rip, signo);
#else
            klog(KLOG_WARN, "isr", "user exception: %s (vector %d, err 0x%x, eip 0x%x): signal %d",
                 name, params->vector, params->error_code, params->eip, signo);
#endif
        }
        return;
    }

#if defined(__x86_64__)
    if (x86_64_fault_fixup(params)) {
        return;
    }
    klog(KLOG_CRIT, "isr", "kernel exception: %s (vector %d, err 0x%x, rip 0x%lx)", name,
         (uint32_t)params->vector, (uint32_t)params->error_code, (unsigned long)params->rip);
#else
    klog(KLOG_CRIT, "isr", "kernel exception: %s (vector %d, err 0x%x, eip 0x%x)", name,
         params->vector, params->error_code, params->eip);
#endif
    k_panic_note_fault(name, (uintptr_t)FRAME_PC(params),
                       _has_error_code(params->vector) ? (long)params->error_code : PANIC_NO_ERROR);
    k_panic("unrecoverable CPU exception in kernel mode");
}

/**
 * The NMI, vector 2, or the machine check, vector 18, which report a fault
 * of the machine and not of what was running: print it and stop, in user
 * mode as in kernel mode. An NMI the watchdog sent or raised, a backtrace
 * request or its counter's overflow, is the watchdog's (`watchdog_nmi`), and
 * returns; so is the NMI of another processor's stop, which this processor
 * takes there, leaving VMX or SVM operation and halting (`smp_stop_nmi`), as
 * Linux's `smp_stop_nmi_callback` does. The NMI is counted on the
 * processor while the watchdog takes it, so that klog keeps its records
 * for later (`smp_serialized`). The paranoid router holds no lock,
 * so no signal may be posted here, and the panic begins before the record
 * is written, so that the console's lock, which the interrupted code may
 * hold, is not taken (`k_panic_begin`).
 */
static void _hardware_fault_handler(isr_params_t *params)
{
    int ours;

    if (params->vector == 2) {
        this_cpu()->in_nmi++;
        ours = watchdog_nmi(params);
        this_cpu()->in_nmi--;
        if (ours) {
            return;
        }
    }
    k_panic_begin();
    klog_deliver_directly();
    klog(KLOG_CRIT, "isr", "hardware fault: %s (vector %d, pc 0x%lx)",
         _exception_names[params->vector], (int)params->vector, (unsigned long)FRAME_PC(params));
    k_panic_note_fault(_exception_names[params->vector], (uintptr_t)FRAME_PC(params),
                       PANIC_NO_ERROR);
    k_panic("the machine reported a fault");
}

#if defined(__x86_64__)
/**
 * The double fault, vector 8, on the interrupt stack its gate names
 * (arch/x86_64/idt.c), with the frame of what was running. It never
 * returns.
 *
 * A double fault is an exception raised while the processor delivers
 * another. The one this kernel meets is a page fault whose frame could not
 * be pushed, since the stack in use ran into the unmapped guard page below
 * it; the interrupt stack is why this handler can run at all. CR2 has the
 * address the push faulted at; one in the guard below the running thread's
 * kernel stack (`kstack_overrun_pid`) says that stack ran out, and the
 * diagnostic names the thread. Either way the kernel cannot go on and the
 * handler panics; its record goes out directly, even when the fault came
 * while another record was being written.
 */
static void _double_fault_handler(isr_params_t *params)
{
    uintptr_t cr2 = arch_fault_address();
    int pid;

    k_panic_begin();
    klog_deliver_directly();
    k_panic_note_fault("double fault", (uintptr_t)params->rip, PANIC_NO_ERROR);
    k_panic_note_address(cr2);
    pid = kstack_overrun_pid(cr2);
    if (pid >= 0) {
        klog(KLOG_CRIT, "isr",
             "kernel exception: double fault (vector 8, rip 0x%lx, rsp 0x%lx): "
             "pid %d ran past the end of its kernel stack at 0x%lx",
             (unsigned long)params->rip, (unsigned long)params->rsp, pid, cr2);
        k_panic("double fault: a kernel stack ran out");
    }
    klog(KLOG_CRIT, "isr",
         "kernel exception: double fault (vector 8, rip 0x%lx, rsp 0x%lx, cr2 0x%lx)",
         (unsigned long)params->rip, (unsigned long)params->rsp, cr2);
    k_panic("unrecoverable CPU exception in kernel mode");
}
#else
/**
 * The double fault's task, which the processor enters through the task gate of
 * vector 8 and not by a call: it saves what was running into the processor's
 * `kernel_tss`, loads its `double_fault_tss` and starts here on that TSS's own
 * stack, with its error code, always 0, where a return address would be. It
 * never returns.
 *
 * A double fault is an exception raised while the processor delivers
 * another. The one this kernel meets is a page fault whose frame could not
 * be pushed, since the stack in use ran into the unmapped guard page below
 * it, and a gate that ran on that stack would fault a third time and reset
 * the machine. CR2 has the address the push faulted at; one in the guard
 * below the running thread's kernel stack (`kstack_overrun_pid`) says that
 * stack ran out, and the diagnostic names the thread. The instruction and
 * stack pointers are those the processor saved in `kernel_tss`. Either way
 * the kernel cannot go on and the handler panics; its record goes out
 * directly, even when the fault came while another record was being
 * written.
 */
void isr_double_fault(void)
{
    uint32_t cr2;
    int pid;

    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    k_panic_begin();
    klog_deliver_directly();
    k_panic_note_fault("double fault", (uintptr_t)kernel_tss[arch_cpu_id()].eip, PANIC_NO_ERROR);
    k_panic_note_address(cr2);
    pid = kstack_overrun_pid(cr2);
    if (pid >= 0) {
        klog(KLOG_CRIT, "isr",
             "kernel exception: double fault (vector 8, eip 0x%x, esp 0x%x): "
             "pid %d ran past the end of its kernel stack at 0x%x",
             kernel_tss[arch_cpu_id()].eip, kernel_tss[arch_cpu_id()].esp, pid, cr2);
        k_panic("double fault: a kernel stack ran out");
    }
    klog(KLOG_CRIT, "isr",
         "kernel exception: double fault (vector 8, eip 0x%x, esp 0x%x, cr2 0x%x)",
         kernel_tss[arch_cpu_id()].eip, kernel_tss[arch_cpu_id()].esp, cr2);
    k_panic("unrecoverable CPU exception in kernel mode");
}
#endif

/**
 * Every exception has a handler from here or from `init_paging`, so that a
 * fault yields a log record and a contained kill or panic, not a fault
 * repeated without end. The double fault is taken onto a stack of its own,
 * by a task gate on i686 and by an interrupt stack on x86-64, since a
 * kernel stack that runs into its guard page raises one: the page fault's
 * frame cannot be pushed there. The NMI and the machine check report a
 * fault of the machine and stop it, whatever was running.
 */
void init_isr(void)
{
    int vector;

    k_memset(&_isr_handlers, 0, sizeof(_isr_handlers));

    for (vector = 0; vector < 32; vector++) {
        if (vector != 2 && vector != 8 && vector != 14 && vector != 18) {
            register_isr_handler((uint8_t)vector, &_exception_handler, "exception");
        }
    }
    register_isr_handler(2, &_hardware_fault_handler, "nmi");
    register_isr_handler(18, &_hardware_fault_handler, "machine check");
#if defined(__x86_64__)
    register_isr_handler(8, &_double_fault_handler, "double fault");
#endif

    __asm__ __volatile__("sti");
}

/** Route line `line` through the I/O APIC to this processor, when it is
 * the I/O APIC's to route. */
static void line_route(int line)
{
    uint16_t bit = (uint16_t)(1u << line);

    if (!lines_apic || ((lines_local | lines_held) & bit) != 0) {
        return;
    }
    (void)ioapic_route_line(line, (lines_pci & bit) != 0, (uint8_t)IRQ_VECTOR(line),
                            (uint8_t)lapic_id());
}

/** A handler went on `vector`: when it is a line's, route the line. */
static void vector_registered(uint8_t vector)
{
    if (vector >= IRQ_VECTOR(0) && vector < IRQ_VECTOR(16)) {
        line_route(vector - IRQ_VECTOR(0));
    }
}

void isr_lines_to_apic(uint16_t pci, uint16_t local)
{
    int line;

    lines_apic = 1;
    lines_pci = pci;
    lines_local = local;
    for (line = 0; line < 16; line++) {
        if (_isr_handlers[IRQ_VECTOR(line)][0] != 0) {
            line_route(line);
        }
    }
}

int isr_lines_apic(void)
{
    return lines_apic;
}

void isr_line_hold(int line)
{
    if (line < 0 || line > 15) {
        return;
    }
    lines_held |= (uint16_t)(1u << line);
    ioapic_mask_line(line);
}

void isr_line_release(int line, int level)
{
    uint16_t bit;

    if (line < 0 || line > 15) {
        return;
    }
    bit = (uint16_t)(1u << line);
    lines_held &= (uint16_t)~bit;
    if (level) {
        lines_pci |= bit;
    }
    if (!lines_apic) {
        pic_unmask_irq((uint8_t)line);
    } else if (_isr_handlers[IRQ_VECTOR(line)][0] != 0) {
        line_route(line);
    }
}

/** Return the free message vectors under the big lock. */
int isr_free_vector_count(void)
{
    return vector_free_count();
}

/**
 * The lowest message vector not yet given to a device, marked as given;
 * -1 with a record when every one is taken.
 */
int isr_alloc_vector(void)
{
    return isr_alloc_vector_aligned(1);
}

/**
 * The lowest free message vector that is a multiple of `align`, marked as
 * given (`vector_take`); -1 with a record when none is free. An `align`
 * that is not a power of two from 1 to 32 is taken as 1.
 */
int isr_alloc_vector_aligned(int align)
{
    int v = vector_take(align);

    if (v < 0) {
        klog(KLOG_ERR, "isr", "vector: none free for a device, aligned to %d", align);
    }
    return v;
}

/**
 * Give a message vector back: empty its chain of handlers and mark it free.
 * A vector outside the pool, or one not given, is left alone.
 */
void isr_free_vector(int vector)
{
    int i;

    if (!vector_give(vector)) {
        return;
    }
    for (i = 0; i < ISR_MAX_VECTOR_HANDLERS; i++) {
        _isr_handlers[vector][i] = 0;
        _isr_names[vector][i] = 0;
    }
}

/**
 * Append `handler` to a vector's chain.
 *
 * `name` is what /proc/interrupts calls this handler; it is a literal that
 * outlives the call, since only the pointer is kept. Re-registering an
 * already-installed handler is a no-op, so an init path that runs twice does
 * not invoke it twice, and the name it registered under the first time
 * stands. A full chain is reported and the handler is dropped.
 */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    int i;

    for (i = 0; i < ISR_MAX_VECTOR_HANDLERS; i++) {
        if (_isr_handlers[vector][i] == handler) {
            return;
        }
    }
    for (i = 0; i < ISR_MAX_VECTOR_HANDLERS; i++) {
        if (_isr_handlers[vector][i] == 0) {
            _isr_handlers[vector][i] = handler;
            _isr_names[vector][i] = name;
            vector_registered(vector);
            return;
        }
    }
    klog(KLOG_ERR, "isr", "handler: vector %d chain full", vector);
}

/**
 * Replace a vector's entire chain with a single `handler`.
 *
 * For a deliberate sole owner of a vector, where no other handler must share
 * it: the keyboard IRQ installs its handler this way instead of chaining. The
 * name goes with it, and the chain's other names go with the handlers.
 */
void set_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    int i;

    _isr_handlers[vector][0] = handler;
    _isr_names[vector][0] = name;
    for (i = 1; i < ISR_MAX_VECTOR_HANDLERS; i++) {
        _isr_handlers[vector][i] = 0;
        _isr_names[vector][i] = 0;
    }
    vector_registered(vector);
}

/**
 * Run every handler chained on the interrupt's vector.
 *
 * Each shared-line handler inspects its own device first and returns quickly
 * when the interrupt is not its own.
 */
static void _dispatch(isr_params_t *params)
{
    int i;
    isr_handler_t handler;

    for (i = 0; i < ISR_MAX_VECTOR_HANDLERS; i++) {
        handler = _isr_handlers[params->vector][i];
        if (handler) {
            handler(params);
        }
    }
}

/* The vector of a system call's frame on both kernels, `int $0x80`'s, which
 * x86-64's `syscall` entry gives its frame too (SYSCALL_VECTOR of each
 * architecture's sysnr.h). */
#define SYSCALL_ENTRY 0x80u

/**
 * Mark an entry from user mode that is not a system call with -1 in the
 * interrupted thread's `orig_nr`, as Linux writes -1 in `orig_ax` on every
 * such entry, so that its tracer and its core file read -1 there and not
 * the number of the last call. Only the thread itself writes the field
 * while it runs, so no lock is needed.
 */
static void user_entry(const isr_params_t *params)
{
    if (FRAME_USER(params) && params->vector != SYSCALL_ENTRY && current_proc) {
        current_proc->orig_nr = -1;
    }
}

/**
 * Panic at once for an exception taken while the panic reads the running
 * process (`k_panic_reading`), before the router takes the big lock;
 * return otherwise. Interrupts are off through the panic, so the exception
 * is the read's own, and the panic that follows reports the first one
 * again (`k_panic`). The panic may run without the lock, from
 * `_hardware_fault_handler` or a double fault, and a processor it stopped
 * may hold the lock for good, so taking it here could spin for ever and
 * the second report, the countdown and the restart would never come.
 * Linux's oops path takes no lock a halted processor can hold: a fault in
 * an oops re-enters it under `die_lock`, which its owner passes through
 * (arch/x86/kernel/dumpstack.c:351-373), and a kernel fault with page
 * faults disabled takes no mmap lock (arch/x86/mm/fault.c:1270-1273).
 */
static void panic_if_reading(const isr_params_t *params)
{
    const char *name;

    if (!k_panic_reading()) {
        return;
    }
    name = params->vector < 32 ? _exception_names[params->vector] : "unknown";
    k_panic_note_fault(name, (uintptr_t)FRAME_PC(params),
                       _has_error_code(params->vector) ? (long)params->error_code : PANIC_NO_ERROR);
    if (params->vector == 14) {
        k_panic_note_address(arch_fault_address());
    }
    k_panic("exception in the panic's read of the process");
}

/**
 * Charge the hold under way to the ioctl(2) request of the system call in
 * `params`, of either ABI, for bkl/calls. Returns the request's slot plus
 * one, which `smp_call_end` takes, or 0 for another call or a request no
 * slot took. The caller holds the big lock; `ia32_fast_syscall` (vdso32.c)
 * calls it as `_isr_router` does.
 */
int isr_call_ioctl(const isr_params_t *params)
{
    if ((uint32_t)SC_NR(params) != CALL_IOCTL_NR(params)) {
        return 0;
    }
    return bkl_note_ioctl((uint32_t)SC_ARG(params, 1));
}

/**
 * Take the big lock and run the handler chain of a processor exception or
 * a system call; `isr_leave` gives the lock back. `params` is the saved
 * register frame, so that a handler can change the saved registers, a
 * system call's result for one. An exception during the panic's read of
 * the process panics before the lock is taken (`panic_if_reading`).
 */
void _isr_router(isr_params_t *params)
{
    uint64_t start = 0;
    uint32_t nr = 0;
    int ioctl = 0;

    user_entry(params);
    if (params->vector == SYSCALL_ENTRY) {
        start = arch_cycles();
        nr = (uint32_t)SC_NR(params);
    }
    panic_if_reading(params);
    bkl_enter();
    bkl_frame_push(1);
    if (params->vector == SYSCALL_ENTRY) {
        ioctl = isr_call_ioctl(params);
    }
    isr_router_locked(params);
    if (params->vector == SYSCALL_ENTRY) {
        smp_call_end(nr, ioctl, start);
    }
}

/** Dispatch a saved frame with the big lock held; isr_leave releases it. */
void isr_router_locked(isr_params_t *params)
{
    int32_t nr = params->vector == SYSCALL_ENTRY ? (int32_t)SC_NR(params) : -1;
    struct bkl_entry_saved saved;

    bkl_note_entry((uint32_t)params->vector, nr);
    bkl_entry_push(&saved, (uint32_t)params->vector, nr,
                   nr >= 0 && (uint32_t)nr == CALL_IOCTL_NR(params), (uint32_t)SC_ARG(params, 1));
#ifdef DEBUG
    klog(KLOG_DEBUG, "isr", "interrupt: vector 0x%X", params->vector);
#endif

    _dispatch(params);
    bkl_entry_pop(&saved);
}

/**
 * Dispatch the NMI, the double fault or the machine check, which arrive at
 * any instruction, without the big lock: their handlers report and stop,
 * and the processor they interrupted may hold the lock, spin for it, or be
 * between the exchange that takes it and the count of its depth. On x86-64
 * each arrives on an interrupt stack of its own. On i686 the NMI and the
 * machine check arrive on the stack in use, and the double fault starts a
 * task of its own (`isr_double_fault`), which does not come here.
 */
void isr_paranoid_router(isr_params_t *params)
{
    _dispatch(params);
}

/**
 * The return to the interrupted code, after its router: deliver a signal
 * pending for a return to user mode, update the thread's rseq area
 * (`rseq_return`), and give back the big lock the router
 * took; a tick the router took without the lock returns as it came.
 * Interrupts are off. A forked child's first return comes here too,
 * with the depth its fork gave it.
 */
void isr_leave(isr_params_t *params)
{
    struct percpu *cpu = this_cpu();

    if (cpu->tick_unlocked) {
        cpu->tick_unlocked = 0;
        return;
    }
    (void)bkl_frame_pop();
    deliver_signals(params);
    if (FRAME_USER(params)) {
        rseq_return(params);
        debugreg_resume();
        waketime_return();
    }
    bkl_exit();
}

/**
 * Take a local vector, without the big lock: the interprocessor interrupt,
 * whose request is in this processor's record (`smp_ipi`), and the APIC's
 * spurious vector, which needs no end-of-interrupt.
 *
 * An interprocessor interrupt that finds user mode with a requested
 * reschedule or its thread's rseq marked takes the big lock after its end
 * of interrupt. It takes the switch, then returns through `isr_leave`, so
 * that a move updates the rseq area and a barrier aborts its section before
 * user code resumes. An interrupt in kernel mode leaves the request for
 * the system call's next scheduling point.
 */
void isr_local_router(isr_params_t *params)
{
    user_entry(params);
    if (params->vector == IPI_VECTOR) {
        smp_ipi();
        lapic_eoi();
        if (FRAME_USER(params) && current_proc &&
            (current_proc->need_resched || current_proc->rseq_notify)) {
            bkl_enter();
            bkl_frame_push(1);
            sched_take_preempt();
            isr_leave(params);
        }
        return;
    }
    lapic_spurious();
}

/* The boot processor's ticks in a row that found the big lock held and
 * ended without it, and the count at which its tick waits for the lock all
 * the same, so that its periodic work (`timer_handler`, kernel.c) runs at
 * most this many ticks late. */
static uint32_t boot_ticks_skipped;
#define BOOT_TICK_SKIPS 4u

/**
 * Whether a tick that found the big lock held by another processor must
 * wait for it: when `sched_tick_waits` says a process queued here has
 * waited long enough, and on the boot processor also when no reference
 * keeps the clock, since its tick alone counts the clock then, or when its
 * last BOOT_TICK_SKIPS - 1 ticks ended without the lock. Interrupts are
 * off.
 */
static int tick_waits(void)
{
    int waits = sched_tick_waits();

    if (arch_cpu_id() != 0) {
        return waits;
    }
    return waits || !timer_tick_shared() || ++boot_ticks_skipped >= BOOT_TICK_SKIPS;
}

/**
 * Run the chain of a device's interrupt with the depth `in_interrupt` reads
 * kept around it: an allocation made by a handler is the kernel's, whoever
 * was running.
 */
static void irq_dispatch(isr_params_t *params)
{
    struct bkl_entry_saved saved;

    irq_enter();
    bkl_entry_push(&saved, (uint32_t)params->vector, -1, 0, 0);
    _dispatch(params);
    bkl_entry_pop(&saved);
    irq_leave();
}

/**
 * Take the big lock, count a device's interrupt, run its vector's chain and
 * end it at the controller it came through; `isr_leave` gives the lock
 * back.
 *
 * A message from a PCI device, vector 48 and up, has no 8259 line to
 * acknowledge, and the local APIC's end-of-interrupt comes after the chain.
 * Nothing is lost either way, since a second message sets the processor's
 * request bit whether or not this one is still in service. Ending it after
 * the chain keeps the two halves of one interrupt in one place, and costs
 * the vectors below it the time the chain runs, which the kernel's own
 * handlers bound.
 *
 * Through the 8259, vectors at or above 40 come from the slave and need
 * both controllers acknowledged; the master is always acknowledged. The end
 * is sent before the chain runs, so that a fast device can raise its line
 * again without being lost.
 *
 * Through the I/O APIC, the local APIC's end-of-interrupt comes after the
 * chain, as a message's does: a level-triggered line is still asserted until
 * its handler has quieted the device, and an end sent before would have the
 * I/O APIC send it again at once. The tick's vector is the exception. Its
 * handler may switch to another process before it returns, and while its
 * vector is in service the local APIC blocks every vector of its
 * priority and below, the lines' included; it is an edge, and is ended
 * first, as through the 8259.
 *
 * After any chain but the tick's, a process a device woke runs now, or at
 * its system call's exit (`sched_preempt`): woken only at the next tick, a
 * disk's caller that slept for its command would get one command a
 * scheduler round. The tick's own handler makes its switch itself.
 *
 * A tick that finds another processor holding the big lock does nothing
 * else: it is ended and counted on this processor's record and returns, and
 * the next tick that takes the lock charges the ticks since
 * (`sched_account_tick`) and, on the boot processor, counts them from the
 * reference (`timer_tick_clock`). Waiting for the lock would take from this
 * processor's user code all the time the holder keeps it, such as the sound
 * mix's, which converts a channel's rate in the device's interrupt; and
 * once the waits pass a tick, the next tick is pending at the return to user
 * mode and comes before the user code's next instruction, which then makes
 * no progress at all while other processors keep the lock busy. A tick
 * waits for the lock only when `tick_waits` says so: a process on this
 * processor's queue has waited long enough, since with the lock always held
 * no tick would end the running process's slice, and a busy processor does
 * not steal; or the boot processor's periodic work has waited enough
 * ticks, or only its tick keeps the clock. Meanwhile the timer pass that
 * wakes the sleepers is made by whichever processor's tick has the lock
 * (`timer_pass_late`, kernel.c).
 *
 * Every tick runs the lockup detectors' part first (`watchdog_tick`), with
 * or without the lock, since a processor locked up with interrupts off may
 * hold it for good.
 *
 * An interrupt that ended a halt, which it knows by the frame's address,
 * resumes with interrupts off. Otherwise a tick already pending at the
 * `iret`, which a wait for the big lock longer than a tick makes likely,
 * arrives before the `cli` that follows the halt, and the processor stays in
 * this router, still marked idle, with ready work on its queue.
 */
void _irq_router(isr_params_t *params)
{
    struct percpu *cpu = this_cpu();
    uint32_t wake_irq = params->vector == IRQ_VECTOR(0) ? waketime_clock() : 0;

    user_entry(params);
    if (FRAME_PC(params) == (unsigned long)arch_idle_resume) {
        FRAME_IRQ_OFF(params);
    }
    if (params->vector == IRQ_VECTOR(0)) {
        watchdog_tick(params);
    }
    if (params->vector != IRQ_VECTOR(0)) {
        bkl_enter();
    } else if (!bkl_try_enter()) {
        if (!tick_waits()) {
            if (lines_apic || arch_cpu_id() != 0) {
                lapic_eoi();
            } else {
                reset_master_pic();
            }
            cpu->ticks_unlocked++;
            cpu->tick_unlocked = 1;
            return;
        }
        bkl_enter();
    }
    bkl_frame_push(1);
    if (params->vector == IRQ_VECTOR(0)) {
        if (arch_cpu_id() == 0) {
            boot_ticks_skipped = 0;
        }
        cpu->wake_irq_start = wake_irq;
        cpu->wake_irq_locked = waketime_clock();
    }
    bkl_note_entry((uint32_t)params->vector, -1);
#if defined(__x86_64__)
    bkl_hold_sample((uintptr_t)FRAME_PC(params), (uintptr_t)params->rbp);
    if (params->vector == IRQ_VECTOR(0)) {
        bkl_profile_sample((uintptr_t)FRAME_PC(params), (uintptr_t)params->rbp);
    }
#else
    bkl_hold_sample((uintptr_t)FRAME_PC(params), (uintptr_t)params->ebp);
    if (params->vector == IRQ_VECTOR(0)) {
        bkl_profile_sample((uintptr_t)FRAME_PC(params), (uintptr_t)params->ebp);
    }
#endif
    irq_count++;
    if (params->vector >= 32 && params->vector <= ISR_MSI_LAST) {
        vector_count[params->vector - 32]++;
    }

#ifdef DEBUG
    klog(KLOG_DEBUG, "isr", "irq: vector 0x%X", params->vector);
#endif

    if (params->vector >= 48) {
        irq_dispatch(params);
        lapic_eoi();
        sched_preempt(); /* a woken reader runs now, or at the call's exit */
        return;
    }

    if (lines_apic) {
        if (params->vector == IRQ_VECTOR(0)) {
            lapic_eoi();
            irq_dispatch(params);
            return;
        }
        irq_dispatch(params);
        lapic_eoi();
        sched_preempt();
        return;
    }

    if (params->vector >= 40) {
        reset_slave_pic();
    }

    reset_master_pic();

    irq_dispatch(params);
    if (params->vector != 32) {
        sched_preempt();
    }
}

/** The ticks of every processor that found the big lock held, which
 * `_irq_router` counts on their records instead of `irq_count`. */
static uint32_t isr_ticks_unlocked(void)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        sum += cpu_table[i].ticks_unlocked;
    }
    return sum;
}

/**
 * The count of interrupts taken on `vector`, for /proc/interrupts.
 *
 * Vectors below 32 are exceptions, and those above ISR_MSI_LAST are the
 * interprocessor interrupt and the APIC's spurious vector, none of which
 * this counts, any more than the system call gate, 128; their count is 0.
 */
uint32_t isr_vector_count(int vector)
{
    if (vector < 32 || vector > ISR_MSI_LAST) {
        return 0;
    }
    return vector_count[vector - 32] + (vector == IRQ_VECTOR(0) ? isr_ticks_unlocked() : 0u);
}

/**
 * The name the handler in `slot` of `vector` registered under, or null.
 *
 * The slots are walked by /proc/interrupts, which prints every name a vector
 * has: a shared PIC line is one vector and two devices.
 */
const char *isr_vector_name(int vector, int slot)
{
    if (vector < 0 || vector >= 256 || slot < 0 || slot >= ISR_MAX_VECTOR_HANDLERS) {
        return 0;
    }
    return _isr_names[vector][slot];
}

/** Every interrupt routed since the boot, for /proc/stat's `intr` line. */
uint32_t isr_interrupt_total(void)
{
    return irq_count + isr_ticks_unlocked();
}

const char *isr_vector_source(int vector)
{
    if (vector >= ISR_MSI_FIRST && vector <= ISR_MSI_LAST) {
        return "MSI";
    }
    if (!lines_apic) {
        return "PIC";
    }
    if (vector >= IRQ_VECTOR(0) && vector < IRQ_VECTOR(16) &&
        (lines_local & (1u << (vector - IRQ_VECTOR(0)))) != 0) {
        return "LAPIC";
    }
    return "IO-APIC";
}

int isr_msi_ready(void)
{
    return lapic_ready();
}

int isr_line_pending(int line)
{
    if (lines_apic) {
        return lapic_requested((uint8_t)IRQ_VECTOR(line)) || ioapic_line_pending(line);
    }
    return pic_irq_pending((uint8_t)line);
}

int isr_vector_pending(int vector)
{
    return lapic_pending((uint8_t)vector);
}

uint32_t isr_spurious_count(void)
{
    return lapic_spurious_count();
}
