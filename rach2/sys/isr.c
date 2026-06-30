#include "isr.h"
#include "panic.h"
#include "serial.h"

#include "lib/ksignal.h"

/*
 * Kill the faulting process with `signo` and schedule another; defined in the
 * process layer. Declared here, as in the page-fault handler, so the trap core
 * need not pull in the whole process header.
 */
extern void terminate_current_process(int32_t signo);

/* IRQ count, kept for debugger inspection; intentionally never read in code. */
static volatile uint32_t irq_count = 0;

/*
 * Each vector holds a short chain of handlers rather than a single slot, so
 * devices that share one PIC line can each install their own. Level-triggered
 * PCI INTx lines are routinely shared: under QEMU's i440fx the HD Audio
 * controller and the RTL8139 NIC both land on IRQ 11. On a shared line every
 * chained handler runs and checks its own device's status, so the device that
 * actually raised the line gets acknowledged; a handler that never runs cannot
 * clear its device, the line stays asserted, and the IRQ storms.
 */
#define MAX_VECTOR_HANDLERS 4
static isr_handler_t _isr_handlers[256][MAX_VECTOR_HANDLERS];

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
        serial_printf("isr: user exception: %s (vector %d, err 0x%x, "
                      "eip 0x%x): killing process\n",
                      name, params->vector, params->error_code, params->eip);
        terminate_current_process(_exception_signal(params->vector));
        return;
    }

    serial_printf("isr: kernel exception: %s (vector %d, err 0x%x, "
                  "eip 0x%x)\n",
                  name, params->vector, params->error_code, params->eip);
    k_panic("isr: unrecoverable CPU exception in kernel mode");
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
            register_isr_handler((uint8_t) vector, &_exception_handler);
        }
    }

    __asm__ __volatile__ ("sti"); /* Enable interrupt handling. */
}

/**
 * Append `handler` to a vector's chain.
 *
 * Re-registering an already-installed handler is a no-op, so an init path that
 * runs twice does not invoke it twice. A full chain is reported and the handler
 * is dropped.
 */
void register_isr_handler(uint8_t vector, isr_handler_t handler)
{
    int i;

    for (i = 0; i < MAX_VECTOR_HANDLERS; i++) {
        if (_isr_handlers[vector][i] == handler) {
            return;
        }
    }
    for (i = 0; i < MAX_VECTOR_HANDLERS; i++) {
        if (_isr_handlers[vector][i] == 0) {
            _isr_handlers[vector][i] = handler;
            return;
        }
    }
    serial_printf("register_isr_handler: vector %d chain full\n", vector);
}

/**
 * Replace a vector's entire chain with a single `handler`.
 *
 * For a deliberate override: swapping a boot self-test stub (the keyboard
 * probe) for the real driver handler, where the stub must not keep running
 * alongside it.
 */
void set_isr_handler(uint8_t vector, isr_handler_t handler)
{
    int i;

    _isr_handlers[vector][0] = handler;
    for (i = 1; i < MAX_VECTOR_HANDLERS; i++) {
        _isr_handlers[vector][i] = 0;
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

    for (i = 0; i < MAX_VECTOR_HANDLERS; i++) {
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
    k_printf("Kernel message: interrupt - 0x%X\n", params->vector);
#endif

    _dispatch(params);
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

#ifdef DEBUG
    k_printf("Kernel message: IRQ - 0x%X\n", params->vector);
#endif

    if (params->vector >= 40) {
        reset_slave_pic();
    }

    reset_master_pic();

    _dispatch(params);
}
