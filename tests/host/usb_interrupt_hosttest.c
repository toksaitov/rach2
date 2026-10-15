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

/* A modelled controller, in place of the driver's own structure. */
struct xhci_ctrl {
    int vector;       /* the message vector it was given, or -1 */
    int irq;          /* its PCI line's IRQ, or -1 for none */
    int messages;     /* whether enabling messages succeeds */
    int line_enabled; /* whether enabling the line succeeds */
    int events;       /* events waiting for the next drain */
};

static struct xhci_ctrl controllers[XHCI_MAX_CONTROLLERS];
static int controller_count;
/* Whether each vector is given out, as `isr_alloc_vector` and
 * `isr_free_vector` keep it. */
static int vector_taken[256];
/* Whether the local APIC takes messages, as `isr_msi_ready` reports. */
static int apic_ready;
/* The vector and the line reported pending, -1 for none, and the vector
 * `usb_core_step` makes pending in the middle of a pump. */
static int pending_vector = -1;
static int pending_irq = -1;
static int late_vector = -1;
/* How often each vector and each line was asked whether it is pending. */
static int vector_queries[256];
static int irq_queries[16];
/* The modelled EFLAGS; 0x200 (IF) is set while interrupts are enabled. */
static uint32_t irq_flags;
static int fails;

#define CHECK(what, condition)                                  \
    do {                                                        \
        if (!(condition)) {                                     \
            printf("usb-interrupt-hosttest: fail: %s\n", what); \
            fails++;                                            \
        }                                                       \
    } while (0)

/** Discard the message: no row checks the log. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Return 0: no row runs `usb_report_controllers`, the caller. */
uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0;
}

/** Find no device, as `pci_read_config32` reads nothing. */
bool pci_find_device_skip(uint8_t class_code, uint8_t subclass, pci_device_t *skip,
                          pci_device_t *out)
{
    (void)class_code;
    (void)subclass;
    (void)skip;
    (void)out;
    return false;
}

/** Return 0: the boot line names no option. */
int kcmdline_has(const char *key)
{
    (void)key;
    return 0;
}

/** Return 0: the boot line has no value for any key. */
int kcmdline_value(const char *key, char *out, unsigned int length)
{
    (void)key;
    (void)out;
    (void)length;
    return 0;
}

/** Return at once: no row waits. */
void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** Clear the modelled interrupt flag and return the flags it had. */
uint32_t kirq_save(void)
{
    uint32_t flags = irq_flags;

    irq_flags = 0;
    return flags;
}

/** Restore the flags that `kirq_save` returned. */
void kirq_restore(uint32_t flags)
{
    irq_flags = flags;
}

/** Return 0: no row reads the clock. */
uint32_t timer_now_us(void)
{
    return 0;
}

/** Do nothing: the latency record is not under test here. */
void usb_stamp_init(int enabled)
{
    (void)enabled;
}

/** Do nothing, as `usb_stamp_init` does. */
void usb_stamp_pump(uint32_t entry, uint32_t event, int msi, int took)
{
    (void)entry;
    (void)event;
    (void)msi;
    (void)took;
}

/** Return whether the row's local APIC takes messages. */
int isr_msi_ready(void)
{
    return apic_ready;
}

/** Count the question, and report `vector` pending when the row set it. */
int isr_vector_pending(int vector)
{
    vector_queries[vector]++;
    return vector == pending_vector;
}

/** Count the question, and report `line` pending when the row set it. */
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

/** Hand out the lowest free message vector, or -1 when the range is used up. */
int isr_alloc_vector(void)
{
    int vector;

    for (vector = ISR_MSI_FIRST; vector <= ISR_MSI_LAST; vector++) {
        if (!vector_taken[vector]) {
            vector_taken[vector] = 1;
            return vector;
        }
    }
    return -1;
}

/** Mark `vector` free for the next `isr_alloc_vector`. */
void isr_free_vector(int vector)
{
    vector_taken[vector] = 0;
}

/** Do nothing: no row raises an interrupt. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** Return the number of controllers the row set up. */
int xhci_controllers(void)
{
    return controller_count;
}

/** Return modelled controller `n`. */
struct xhci_ctrl *xhci_controller(int n)
{
    return &controllers[n];
}

/** Take `vector` and return 0 when the controller's messages work, else -1. */
int xhci_interrupts_enable(struct xhci_ctrl *hc, uint8_t vector)
{
    if (!hc->messages) {
        return -1;
    }
    hc->vector = vector;
    return 0;
}

/** Return the controller's line IRQ, or -1. */
int xhci_line_irq(struct xhci_ctrl *hc)
{
    return hc->irq;
}

/** Return the line IRQ when enabling the line works, else -1. */
int xhci_interrupts_enable_line(struct xhci_ctrl *hc)
{
    return hc->line_enabled ? hc->irq : -1;
}

/** Do nothing: no row silences a controller or stops the stack. */
void xhci_interrupts_disable(struct xhci_ctrl *hc)
{
    (void)hc;
}

/** Do nothing, as `xhci_interrupts_disable` does. */
void xhci_stop(struct xhci_ctrl *hc)
{
    (void)hc;
}

/** Do nothing: the model has no clock. */
void xhci_advance(struct xhci_ctrl *hc, uint32_t elapsed_ms)
{
    (void)hc;
    (void)elapsed_ms;
}

/** Take and return the controller's waiting events. */
int xhci_events(struct xhci_ctrl *hc)
{
    int events = hc->events;

    hc->events = 0;
    return events;
}

/** Take the events, as `xhci_events` does; there is nothing to acknowledge. */
int xhci_interrupt_take(struct xhci_ctrl *hc)
{
    return xhci_events(hc);
}

/** Make `late_vector` pending, as a message that arrives during the step. */
void usb_core_step(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    if (late_vector >= 0) {
        pending_vector = late_vector;
    }
}

/** Do nothing: there are no devices. */
void usb_core_reset(void)
{}

/** Return 1: there are no devices to wait for. */
int usb_core_idle(void)
{
    return 1;
}

/** Return 0 devices. */
int usb_core_count(void)
{
    return 0;
}

int xhci_dma32;

/** Discover no controller: the rows set up the controllers themselves. */
int xhci_discover(void)
{
    return 0;
}

/** Return an empty error text. */
const char *xhci_error(struct xhci_ctrl *hc)
{
    (void)hc;
    return "";
}

/** Return 0. The bring-up stubs from here to `xhci_port_status` are not
 * reached by the rows; they link `usb_init`. */
int xhci_read_caps(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

/** Return 0. */
int xhci_own(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

/** Return 0. */
int xhci_halt_reset(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

/** Return 0. */
int xhci_scratchpads(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

/** Return 0. */
int xhci_start(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

/** Do nothing. */
void xhci_release(struct xhci_ctrl *hc)
{
    (void)hc;
}

/** Do nothing. */
void xhci_forget(struct xhci_ctrl *hc)
{
    (void)hc;
}

/** Return a zeroed set of capabilities. */
const struct xhci_caps *xhci_capabilities(struct xhci_ctrl *hc)
{
    static struct xhci_caps caps;

    (void)hc;
    return &caps;
}

/** Return 0 ports. */
int xhci_ports(struct xhci_ctrl *hc)
{
    (void)hc;
    return 0;
}

/** Return -1: there is no port. */
int xhci_port_status(struct xhci_ctrl *hc, int port, struct usb_port_status *out)
{
    (void)hc;
    (void)port;
    (void)out;
    return -1;
}

/**
 * Reset the controller, processor and USB state for `count` controllers, each
 * with no working interrupt source until the row gives it one.
 */
static void reset(int count)
{
    int n;

    controller_count = count;
    apic_ready = 1;
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
        vector_taken[n] = 0;
    }
    for (n = 0; n < 16; n++) {
        irq_queries[n] = 0;
    }
}

/**
 * Start the interrupts, then drain each controller with a working source
 * while that source alone is pending: none of those drains may count as a
 * fallback. Then drain events with no source pending, which counts once, and
 * drain nothing, which does not count.
 */
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

/**
 * Run the configurations: every controller on messages, both on lines with
 * no local APIC, messages and a line mixed in both orders, and one source
 * whose enable failed, which must not be asked about and whose vector is
 * given back; then a message that becomes pending only during the pump's step,
 * and a controller polled with no interrupt configured. Return 1 when a row
 * failed.
 */
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
    CHECK("a working MSI enable keeps its vector", vector_taken[48]);
    CHECK("a failed MSI enable gives its vector back", !vector_taken[49]);

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
