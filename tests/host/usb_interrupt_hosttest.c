/**
 * Test interrupt-source registration and fallback counts across controllers.
 *
 * The real setup and pump run over controller and processor stubs. Each
 * controller can use MSI, a PCI line, or polling, and each source can be
 * pending independently. The host IRQ functions model the interrupt flag.
 *
 * host-deps:
 */

#include "../../rach2/sys/usb/usb.c"
#include "../../rach2/sys/arch/x86/isr.h"

extern int printf(const char *format, ...);

struct xhci_ctrl {
    int vector;
    int irq;
    int messages;
    int line_enabled;
    int events;
};

static struct xhci_ctrl controllers[XHCI_MAX_CONTROLLERS];
static int controller_count;
static int next_vector;
static int apic_ready;
static int pending_vector = -1;
static int pending_irq = -1;
static int late_vector = -1;
static int vector_queries[256];
static int irq_queries[16];
static uint32_t irq_flags;
static int fails;

#define CHECK(what, condition)                                  \
    do {                                                        \
        if (!(condition)) {                                     \
            printf("usb-interrupt-hosttest: fail: %s\n", what); \
            fails++;                                            \
        }                                                       \
    } while (0)

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0;
}

bool pci_find_device_skip(uint8_t class_code, uint8_t subclass, pci_device_t *skip,
                          pci_device_t *out)
{
    (void)class_code;
    (void)subclass;
    (void)skip;
    (void)out;
    return false;
}

int kcmdline_has(const char *key)
{
    (void)key;
    return 0;
}

int kcmdline_value(const char *key, char *out, unsigned int length)
{
    (void)key;
    (void)out;
    (void)length;
    return 0;
}

void k_udelay(uint32_t usec)
{
    (void)usec;
}

uint32_t kirq_save(void)
{
    uint32_t flags = irq_flags;

    irq_flags = 0;
    return flags;
}

void kirq_restore(uint32_t flags)
{
    irq_flags = flags;
}

uint32_t timer_now_us(void)
{
    return 0;
}

void usb_stamp_init(int enabled)
{
    (void)enabled;
}

void usb_stamp_pump(uint32_t entry, uint32_t event, int msi, int took)
{
    (void)entry;
    (void)event;
    (void)msi;
    (void)took;
}

int isr_msi_ready(void)
{
    return apic_ready;
}

int isr_vector_pending(int vector)
{
    vector_queries[vector]++;
    return vector == pending_vector;
}

int isr_line_pending(int line)
{
    irq_queries[line]++;
    return line == pending_irq;
}

/** The interrupt flag, bit 9 of the modelled flags. */
int kirq_enabled(void)
{
    return (irq_flags & 0x200u) != 0;
}

int isr_alloc_vector(void)
{
    return next_vector <= ISR_MSI_LAST ? next_vector++ : -1;
}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

int xhci_controllers(void)
{
    return controller_count;
}

struct xhci_ctrl *xhci_controller(int n)
{
    return &controllers[n];
}

int xhci_interrupts_enable(struct xhci_ctrl *hc, uint8_t vector)
{
    if (!hc->messages) {
        return -1;
    }
    hc->vector = vector;
    return 0;
}

int xhci_line_irq(struct xhci_ctrl *hc)
{
    return hc->irq;
}

int xhci_interrupts_enable_line(struct xhci_ctrl *hc)
{
    return hc->line_enabled ? hc->irq : -1;
}

void xhci_interrupts_disable(struct xhci_ctrl *hc)
{
    (void)hc;
}

void xhci_stop(struct xhci_ctrl *hc)
{
    (void)hc;
}

void xhci_advance(struct xhci_ctrl *hc, uint32_t elapsed_ms)
{
    (void)hc;
    (void)elapsed_ms;
}

int xhci_events(struct xhci_ctrl *hc)
{
    int events = hc->events;

    hc->events = 0;
    return events;
}

int xhci_interrupt_take(struct xhci_ctrl *hc)
{
    return xhci_events(hc);
}

void usb_core_step(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    if (late_vector >= 0) {
        pending_vector = late_vector;
    }
}

void usb_core_reset(void)
{}

int usb_core_idle(void)
{
    return 1;
}

int usb_core_count(void)
{
    return 0;
}

int xhci_dma32;

int xhci_discover(void)
{
    return 0;
}

const char *xhci_error(struct xhci_ctrl *hc)
{
    (void)hc;
    return "";
}

int xhci_read_caps(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

int xhci_own(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

int xhci_halt_reset(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

int xhci_scratchpads(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

int xhci_start(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

void xhci_release(struct xhci_ctrl *hc)
{
    (void)hc;
}

void xhci_forget(struct xhci_ctrl *hc)
{
    (void)hc;
}

const struct xhci_caps *xhci_capabilities(struct xhci_ctrl *hc)
{
    static struct xhci_caps caps;

    (void)hc;
    return &caps;
}

int xhci_ports(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

int xhci_port_status(struct xhci_ctrl *hc, int port, struct usb_port_status *out)
{
    (void)hc;
    (void)port;
    (void)out;
    return -1;
}

/** Reset controller, processor and USB state for one configuration. */
static void reset(int count)
{
    int n;

    controller_count = count;
    apic_ready = 1;
    next_vector = ISR_MSI_FIRST;
    pending_vector = -1;
    pending_irq = -1;
    late_vector = -1;
    irq_flags = 0x200u;
    usb_up = 1;
    interrupts_started = 0;
    usb_fallback = 0;
    pump_enabled_interrupts = 0;
    for (n = 0; n < XHCI_MAX_CONTROLLERS; n++) {
        controllers[n].vector = -1;
        controllers[n].irq = -1;
        controllers[n].messages = 0;
        controllers[n].line_enabled = 0;
        controllers[n].events = 0;
    }
    for (n = 0; n < 256; n++) {
        vector_queries[n] = 0;
    }
    for (n = 0; n < 16; n++) {
        irq_queries[n] = 0;
    }
}

/** Check each configured source with only that source pending. */
static void check_sources(void)
{
    int n;

    usb_interrupts_start();
    for (n = 0; n < controller_count; n++) {
        if (controllers[n].messages) {
            pending_vector = controllers[n].vector;
        } else if (controllers[n].line_enabled) {
            pending_irq = controllers[n].irq;
        } else {
            continue;
        }
        controllers[n].events = 1;
        usb_poll();
        CHECK("a configured pending source prevents fallback", usb_fallback_count() == 0);
        CHECK("the pump preserves the interrupt flag", irq_flags == 0x200u);
        pending_vector = -1;
        pending_irq = -1;
    }
    controllers[0].events = 1;
    usb_poll();
    CHECK("work without a pending source counts once", usb_fallback_count() == 1);
    usb_poll();
    CHECK("an empty drain does not count", usb_fallback_count() == 1);
}

int main(void)
{
    int n;

    reset(XHCI_MAX_CONTROLLERS);
    for (n = 0; n < controller_count; n++) {
        controllers[n].messages = 1;
    }
    check_sources();

    reset(2);
    apic_ready = 0;
    controllers[0].irq = 7;
    controllers[1].irq = 11;
    controllers[0].line_enabled = controllers[1].line_enabled = 1;
    check_sources();

    reset(2);
    controllers[0].messages = 1;
    controllers[1].irq = 11;
    controllers[1].line_enabled = 1;
    check_sources();

    reset(2);
    controllers[0].irq = 7;
    controllers[0].line_enabled = 1;
    controllers[1].messages = 1;
    check_sources();

    reset(2);
    controllers[0].messages = 1;
    controllers[1].irq = 11;
    check_sources();
    CHECK("a failed line enable is not inspected", irq_queries[11] == 0);
    CHECK("a failed MSI enable is not inspected", vector_queries[49] == 0);

    reset(2);
    controllers[0].messages = controllers[1].messages = 1;
    usb_interrupts_start();
    late_vector = controllers[0].vector;
    controllers[0].events = 1;
    usb_poll();
    CHECK("a message observed at the second check prevents fallback", usb_fallback_count() == 0);

    reset(1);
    usb_interrupts_start();
    controllers[0].events = 1;
    usb_poll();
    CHECK("polling without configured interrupts does not count", usb_fallback_count() == 0);

    if (fails) {
        return 1;
    }
    printf("usb-interrupt-hosttest: result: pass\n");
    return 0;
}
