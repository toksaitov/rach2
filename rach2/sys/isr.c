#include "lapic.h"
#include "klog.h"
#include "isr.h"
#include "panic.h"
#include "serial.h"
#include "mm/pressure.h"

#include "lib/ksignal.h"
#include "lib/kstring.h"
#include "proc/process.h"
#include "proc/ptrace.h"

/*
 * Kill the faulting process with `signo` and schedule another; defined in the
 * process layer. Declared here, as in the page-fault handler, so the trap core
 * need not pull in the whole process header.
 */
#include "../kernel.h"

/*
 * The scheduler, and whether this processor is inside a system call.
 * Declared here for the same reason as the kill above: a message vector that
 * woke a reader has to let that reader run, and the trap core need not pull
 * in the whole process header to ask which context it interrupted.
 */
extern void sched_preempt(void);

/* Every interrupt this kernel has routed through `_irq_router`, which is what
 * /proc/stat's `intr` line reports. */
static volatile uint32_t irq_count = 0;

/* One count per vector this kernel takes interrupts on, 32 to 63, which is
 * what /proc/interrupts reports. */
static volatile uint32_t vector_count[32];

/*
 * Each vector has a short chain of handlers, not a single slot, so
 * devices that share one PIC line can each install their own. Level-triggered
 * PCI INTx lines are routinely shared: under QEMU's i440fx the HD Audio
 * controller and the RTL8139 NIC both land on IRQ 11. On a shared line every
 * chained handler runs and checks its own device's status, so the device that
 * actually raised the line gets acknowledged; a handler that never runs cannot
 * clear its device, the line stays asserted, and the IRQ storms.
 */
static isr_handler_t _isr_handlers[256][ISR_MAX_VECTOR_HANDLERS];
static uint8_t _vector_taken[256]; /* given to a device by isr_alloc_vector */

/* What each handler asked to be called, indexed as the chain is. A name is
 * the only thing /proc/interrupts can say about a line that a number cannot:
 * two devices on one line are two names on it. */
static const char *_isr_names[256][ISR_MAX_VECTOR_HANDLERS];

void _isr_router(isr_params_t *params);

void _irq_router(isr_params_t *params);

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

/* The floating-point exception an FPU status names, as Linux reports it:
 * invalid operation, then division by zero, overflow, underflow and precision,
 * among the flags `status` raised and `mask` does not mask. */
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
 * single step's is TRAP_TRACE.
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
        info->addr = params->eip;
        return SIGFPE;
    case 1: /* debug: DR6 says whether a step or a breakpoint */
        info->code = ptrace_debug_status();
        if (info->code == 0) {
            info->code = TRAP_TRACE;
        }
        info->addr = params->eip;
        return SIGTRAP;
    case 3: /* breakpoint */
        return SIGTRAP;
    case 6: /* invalid opcode */
        info->code = ILL_ILLOPN;
        info->addr = params->eip;
        return SIGILL;
    case 11: /* segment not present */
    case 12: /* stack-segment fault */
        return SIGBUS;
    case 16: /* x87 FPU error */
        __asm__ __volatile__("fnstsw %%ax; movzwl %%ax, %0" : "=r"(status) : : "eax");
        __asm__ __volatile__("fnstcw (%0)" : : "r"(&mask) : "memory");
        info->code = _fpe_code(status, mask & 0x3Fu);
        info->addr = params->eip;
        return SIGFPE;
    case 17: /* alignment check */
        info->code = BUS_ADRALN;
        return SIGBUS;
    case 19: /* SIMD FP exception */
        __asm__ __volatile__("stmxcsr (%0)" : : "r"(&status) : "memory");
        info->code = _fpe_code(status & 0x3Fu, (status >> 7) & 0x3Fu);
        info->addr = params->eip;
        return SIGFPE;
    default:
        return SIGSEGV;
    }
}

/**
 * Default handler for every CPU exception without a dedicated one.
 *
 * The page-fault vector (14) installs its own handler; the rest of vectors
 * 0-31 share this one. Without it an unhandled exception returns into the
 * faulting instruction, which faults again to a triple fault and a silent
 * reset. A fault taken in user mode (the saved `CS` is at RPL 3) is that
 * process's own, so it gets the signal its cause maps to, forced as Linux
 * forces one: a handler the process installed runs, and otherwise the
 * process dies and the cause is printed. A fault in kernel mode has no safe
 * recovery and panics.
 */
static void _exception_handler(isr_params_t *params)
{
    const char *name = params->vector < 32 ? _exception_names[params->vector] : "unknown";

    /* A data breakpoint a tracer set on a user address fires when the
     * kernel touches it too, in a copy to or from the process: the thread
     * gets its SIGTRAP on the way back to user mode, and the access goes
     * on, since a data breakpoint traps after it. */
    if (params->vector == 1 && (params->cs & 3) != 3 && current_proc) {
        struct k_siginfo info;

        k_memset(&info, 0, sizeof(info));
        info.code = ptrace_debug_status();
        params->eflags &= ~0x100u;
        if (info.code == TRAP_HWBKPT) {
            signal_post_thread(current_proc, SIGTRAP, &info);
        }
        return;
    }

    if ((params->cs & 3) == 3) {
        struct k_siginfo info;
        int signo = _exception_signal(params, &info);

        current_proc->trap_nr = params->vector;
        current_proc->trap_err = params->error_code;
        current_proc->trap_addr = 0;
        signal_force(current_proc, signo, &info);
        if (current_proc->sigacts[signo].handler == KSIG_DFL) {
            klog(KLOG_WARN, "isr", "user exception: %s (vector %d, err 0x%x, eip 0x%x): signal %d",
                 name, params->vector, params->error_code, params->eip, signo);
        }
        return;
    }

    klog(KLOG_CRIT, "isr",
         "kernel exception: %s (vector %d, err 0x%x, "
         "eip 0x%x)\n",
         name, params->vector, params->error_code, params->eip);
    k_panic("unrecoverable CPU exception in kernel mode");
}

/**
 * Clear the handler table, install the default exception handler, and enable
 * interrupts.
 *
 * Every CPU exception except the page fault (vector 14, handled in
 * `init_paging`) gets the shared default so an unhandled fault yields a
 * diagnostic and a contained kill or panic, not a silent triple-fault reset.
 */
void init_isr(void)
{
    int vector;

    k_memset(&_isr_handlers, 0, sizeof(_isr_handlers));

    for (vector = 0; vector < 32; vector++) {
        if (vector != 14) {
            register_isr_handler((uint8_t)vector, &_exception_handler, "exception");
        }
    }

    __asm__ __volatile__("sti"); /* Enable interrupt handling. */
}

/**
 * The lowest message vector not yet given to a device, marked as given;
 * -1 with a record when every one is taken.
 */
int isr_alloc_vector(void)
{
    int v;

    for (v = ISR_MSI_FIRST; v <= ISR_MSI_LAST; v++) {
        if (!_vector_taken[v]) {
            _vector_taken[v] = 1;
            return v;
        }
    }
    klog(KLOG_ERR, "isr", "vector: none free for a device");
    return -1;
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

/**
 * Dispatch a CPU exception/software interrupt to its handler chain.
 *
 * `params` points at the saved register frame on the interrupt stack, so a
 * handler can edit the saved registers (for example, a syscall return value).
 */
void _isr_router(isr_params_t *params)
{
#ifdef DEBUG
    klog(KLOG_DEBUG, "isr", "interrupt: vector 0x%X", params->vector);
#endif

    _dispatch(params);
}

/*
 * The hardware interrupt's dispatch, with the depth `in_interrupt` reads
 * kept around it: an allocation made by a handler is the kernel's, whoever
 * was running.
 */
static void irq_dispatch(isr_params_t *params)
{
    irq_enter();
    _dispatch(params);
    irq_leave();
}

/**
 * Dispatch a hardware IRQ and send the PIC end-of-interrupt.
 *
 * Vectors at or above 40 come from the slave PIC and need both PICs acked; the
 * master is always acked. The EOI is sent before the handler chain runs so a
 * fast device can re-raise its line without being lost.
 */
void _irq_router(isr_params_t *params)
{
    irq_count++;
    if (params->vector >= 32 && params->vector < 64) {
        vector_count[params->vector - 32]++;
    }

#ifdef DEBUG
    klog(KLOG_DEBUG, "isr", "irq: vector 0x%X", params->vector);
#endif

    if (params->vector >= 48) {
        /*
         * A message from a PCI device: the 8259 has no line to acknowledge,
         * and the APIC's end-of-interrupt comes after the handler chain.
         * Nothing is lost either way, since a second message sets the
         * processor's request bit whether or not this one is still in
         * service and is taken when this returns. Ending it after the chain
         * keeps the two halves of one interrupt in one place, and costs the
         * vectors below it the time the chain runs, which is bounded by the
         * handlers this kernel installs.
         */
        if (params->vector == 255) {
            lapic_spurious(); /* the APIC's own, which needs no end */
            return;
        }
        irq_dispatch(params);
        lapic_eoi();
        sched_preempt(); /* a woken reader runs now, or at the call's exit */
        return;
    }

    if (params->vector >= 40) {
        reset_slave_pic();
    }

    reset_master_pic();

    irq_dispatch(params);
    /* A process a device woke runs now, or at its call's exit, as on the
     * APIC path above; woken only at the next tick, a disk's caller that
     * slept for its command would get one command a scheduler round. The
     * timer's own handler takes its switch itself. */
    if (params->vector != 32) {
        sched_preempt();
    }
}

/**
 * The count of interrupts taken on `vector`, for /proc/interrupts.
 *
 * Vectors below 32 are exceptions and above 63 are the syscall gate and the
 * APIC's spurious vector, none of which this counts.
 */
uint32_t isr_vector_count(int vector)
{
    if (vector < 32 || vector >= 64) {
        return 0;
    }
    return vector_count[vector - 32];
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
    return irq_count;
}
