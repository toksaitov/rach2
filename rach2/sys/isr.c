#include "isr.h"
#include "serial.h"

/* Debug counter for IRQs */
static volatile uint32_t irq_count = 0;

/*
 * Custom interrupt handlers
 */
isr_handler_t _isr_handlers[256];

/*
 * Calls registered interrupt handlers passing pointer to 'params' on stack.
 * Pointer allows handlers to modify saved registers (e.g., syscall return values).
 */
void _isr_router(isr_params_t *params);

/*
 * Calls registered interrupt request handlers passing pointer to 'params'.
 */
void _irq_router(isr_params_t *params);

void init_isr(void)
{
    k_memset(&_isr_handlers, 0, sizeof(_isr_handlers));

    __asm__ __volatile__ ("sti"); /* Enable interrupt handling. */
}

void register_isr_handler(uint8_t vector, isr_handler_t handler)
{
    _isr_handlers[vector] = handler;
}

void _isr_router(isr_params_t *params)
{
#ifdef DEBUG
    k_printf("Kernel message: interrupt - 0x%X\n", params->vector);
#endif

    if (_isr_handlers[params->vector]) {
        _isr_handlers[params->vector](params);
    }
}

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

    if (_isr_handlers[params->vector]) {
        _isr_handlers[params->vector](params);
    }
}
