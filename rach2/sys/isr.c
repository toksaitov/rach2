#include "lapic.h"
#include "klog.h"
#include "isr.h"
#include "panic.h"
#include "serial.h"
#include "mm/pressure.h"

#include "lib/ksignal.h"

/*
 * Kill the faulting process with `signo` and schedule another; defined in the
 * process layer. Declared here, as in the page-fault handler, so the trap core
 * need not pull in the whole process header.
 */
extern void terminate_current_process(int32_t signo);

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
static const char *const _exception_names[32] = {
    "divide error", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "BOUND range exceeded", "invalid opcode",
    "device not available", "double fault", "coprocessor segment overrun",
    "invalid TSS", "segment not present", "stack-segment fault",
    "general protection", "page fault", "reserved", "x87 FPU error",
    "alignment check", "machine check", "SIMD FP exception", "virtualization",
    "control protection", "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved"
};

/**
 * Map a CPU exception vector to the signal a faulting user process dies with.
 *
 * The few faults that name a precise cause map to it; everything else is, from
 * the process's point of view, a memory violation, so it gets `SIGSEGV`.
 */
static int _exception_signal(uint32_t vector)
{
    switch (vector) {
    case 0:  return SIGFPE;  /* divide error */
    case 1:  return SIGTRAP; /* debug */
    case 3:  return SIGTRAP; /* breakpoint */
    case 6:  return SIGILL;  /* invalid opcode */
    case 11: return SIGBUS;  /* segment not present */
    case 12: return SIGBUS;  /* stack-segment fault */
    case 16: return SIGFPE;  /* x87 FPU error */
    case 17: return SIGBUS;  /* alignment check */
    case 19: return SIGFPE;  /* SIMD FP exception */
    default: return SIGSEGV;
    }
}

/**
 * Default handler for every CPU exception without a dedicated one.
 *
 * The page-fault vector (14) installs its own handler; the rest of vectors
 * 0-31 share this one. Without it an unhandled exception returns into the
 * faulting instruction, which faults again to a triple fault and a silent
 * reset. A fault taken in user mode (the saved `CS` is at RPL 3) is that
 * process's own bug, so it is killed with the signal its cause maps to and the
 * kernel survives. A fault in kernel mode has no safe recovery and panics.
 * Either way the cause is printed: the mnemonic, vector, error code, and EIP.
 */
static void _exception_handler(isr_params_t *params)
{
    const char *name = params->vector < 32
                           ? _exception_names[params->vector]
                           : "unknown";

    if ((params->cs & 3) == 3) {
        klog(KLOG_WARN, "isr", "user exception: %s (vector %d, err 0x%x, "
                      "eip 0x%x): killing process\n",
                      name, params->vector, params->error_code, params->eip);
        terminate_current_process(_exception_signal(params->vector));
        return;
    }

    klog(KLOG_CRIT, "isr", "kernel exception: %s (vector %d, err 0x%x, "
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
            register_isr_handler((uint8_t) vector, &_exception_handler, "exception");
        }
    }

    __asm__ __volatile__ ("sti"); /* Enable interrupt handling. */
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
