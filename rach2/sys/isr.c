#include "isr.h"
#include "serial.h"

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

/**
 * Clear the handler table and enable interrupts.
 */
void init_isr(void)
{
    k_memset(&_isr_handlers, 0, sizeof(_isr_handlers));

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
 * For a deliberate override: swapping a boot self-test stub (the PIT/keyboard
 * probes) for the real driver handler, where the stub must not keep running
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
