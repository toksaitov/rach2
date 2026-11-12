#ifndef _LAPIC_H_
#define _LAPIC_H_

#include "../../lib/kint.h"

/*
 * lapic: the local APIC, enabled so that PCI devices can send messages, and
 * the processor's receiver of the I/O APIC's interrupts and the APIC
 * timer's.
 *
 * `lapic_init` enables it or refuses with the reason in the kernel log: a
 * processor without an APIC or without the model-specific registers, one
 * the firmware left in x2APIC mode with an identity xAPIC mode cannot
 * address where no remapping can come, a base address this driver cannot
 * map, or `rach2.nomsi` on the boot line. It runs the APIC in x2APIC mode
 * under a hypervisor that allows it, and in xAPIC mode elsewhere until
 * interrupt remapping moves it (`lapic_x2apic_enable`); a firmware's x2APIC
 * mode it keeps until remapping's format is known (`lapic_x2apic_settle`).
 * Every device driver asks `isr_msi_ready`, which is
 * `lapic_ready`, before it programs a message, and stays on its legacy line
 * when the answer is no.
 *
 * `lapic_eoi` ends an interrupt that arrived through the APIC: a message, an
 * I/O APIC input, or the timer. An interrupt the 8259 pair delivers, at
 * vectors 32 to 47, is acknowledged at the 8259 and reaches the processor
 * through `LINT0`, which `lapic_init` leaves an unmasked external-interrupt
 * entry; the platform masks `LINT0` when the I/O APIC takes the lines.
 */

/*
 * The registers, reached through this interface so that a host test can
 * model the page the way the xHCI driver's test models its controller.
 * Without a call to `lapic_set_io` the driver maps the page the processor
 * names and reads and writes it directly.
 */
struct lapic_io {
    uint32_t (*read)(uint32_t reg);
    void (*write)(uint32_t reg, uint32_t value);
};

/** Reach every register through `ops` in place of the mapped page, for a
 * host test; NULL reads and writes the page. */
void lapic_set_io(const struct lapic_io *ops);

/**
 * Enable the APIC: 0 when messages can be received, -1 with the reason
 * named in the log.
 *
 * Chooses the mode, maps the APIC's page in xAPIC mode, records the
 * processor's identity for `lapic_id`,
 * sets the spurious vector and the software enable with a task priority of
 * 0, and makes `LINT0` an unmasked external-interrupt entry if it is not
 * one. Called once, on the boot processor, with interrupts off, from
 * `platform_timer_start`.
 */
int lapic_init(void);

/** Whether the APIC is enabled in x2APIC mode, its registers MSRs. */
int lapic_x2apic(void);

/** Whether the APIC is enabled and may run in x2APIC mode: it does, or the
 * processor offers it and `nox2apic` did not take effect. Interrupt
 * remapping asks this before it chooses its x2APIC format. */
int lapic_x2apic_possible(void);

/** Whether `nox2apic` took effect: it is on the boot line, and the firmware
 * did not leave the APIC in an x2APIC mode it locked or with an identity
 * of 255 or more, where Linux's setup_nox2apic ignores it (apic.c, lines
 * 1733 to 1754). /proc/cpuinfo then leaves out `x2apic`. */
int lapic_nox2apic(void);

/**
 * Move the boot processor's APIC to x2APIC mode, and every processor
 * started after it: 1 when it moved, 0 when it was in x2APIC mode already,
 * and -1 when `lapic_x2apic_possible` says no.
 *
 * For interrupt remapping (iommu/intremap.c), once it is on in the x2APIC
 * format and refuses the compatibility format, with the 8259 and the I/O
 * APIC's lines masked, as Linux's enable_IR_x2apic moves it (apic.c, lines
 * 1913 to 1926). The move keeps every register, the request and
 * in-service registers among them; nothing moves the APIC back. Called on
 * the boot processor before the others start, with interrupts off.
 */
int lapic_x2apic_enable(void);

/**
 * Keep or leave the x2APIC mode a firmware left, which `lapic_init` kept
 * until now, once interrupt remapping has started in the x2APIC format
 * (`remapped`) or has not, as Linux's try_to_enable_x2apic does (apic.c,
 * lines 1820 to 1857): kept when remapped, left otherwise, and a panic
 * where it can be neither, with an identity of 255 or more. Nothing when
 * no such mode was kept. The I/O APIC's lines and the local timer are live
 * by then, so the leave masks the lines, the 8259 and the timer's and the
 * error's entries, lets the handlers take what was sent before, and sends
 * again what the request register held as it passed the disabled state.
 * Called once, on the boot processor before the others start and before
 * any driver enables a device's message, after the VT-d units, whose fault
 * events the caller holds (platform.c), with interrupts on.
 */
void lapic_x2apic_settle(int remapped);

/* The vector of the interprocessor interrupt (proc/smp.c): in the highest
 * priority class, so that a device's interrupt in service does not hold it
 * back. */
#define IPI_VECTOR 0xF0

/** Enable the APIC of a processor the boot one started, in the boot
 * processor's mode: the spurious vector and the software enable, and the
 * task priority; nothing when `lapic_init` refused. Its `LINT0` and
 * `LINT1` are the MADT's (`apic_lint_init`). */
void lapic_init_ap(void);

/**
 * Send `vector` to the processor whose APIC has the identity `target`.
 *
 * This and the sends below do nothing while the APIC is not enabled. In
 * xAPIC mode each writes the destination and then the command with
 * interrupts off and waits, boundedly, until the APIC has sent the message;
 * in x2APIC mode each is one write, after a fence that orders the stores
 * made before it.
 */
void lapic_send_ipi(uint32_t target, uint8_t vector);

/** Send that processor a non-maskable interrupt, which no masking of its
 * own holds back: the watchdog's backtrace request and the panic's stop of
 * a processor that keeps interrupts off. */
void lapic_send_nmi(uint32_t target);

/** Send that processor INIT, which holds it waiting for a startup message. */
void lapic_send_init(uint32_t target);

/** Send that processor a startup message: it starts in real mode at the
 * physical address `page` * 4096, below 1 MiB. */
void lapic_send_startup(uint32_t target, uint32_t page);

/** Whether the APIC is enabled and messages can be received. */
int lapic_ready(void);

/**
 * Write `LINT0` as an unmasked external interrupt again; nothing while the
 * APIC is not enabled.
 *
 * For `platform_timer_start`, when the 8259's tick stops arriving after the
 * APIC was enabled: that is what the timer shows of a firmware or an
 * emulator that masked the entry after `lapic_init` wrote it.
 */
void lapic_repair_lint0(void);

/**
 * Whether `vector` is waiting at this processor or is being handled; 0
 * while the APIC is not enabled.
 *
 * A message the device has sent is latched in the request register until
 * the processor takes it, and is in the in-service register while its
 * handler runs. Either says the interrupt was announced, which is what the
 * USB stack asks before it counts an event as one no message was coming
 * for: a processor inside another interrupt has its interrupt flag clear,
 * and a message that arrived a moment earlier is waiting, not lost.
 */
int lapic_pending(uint8_t vector);

/** The APIC identity of the boot processor, read when it enabled the APIC
 * or changed its mode: the destination of a device's message and of an I/O
 * APIC input. */
uint32_t lapic_id(void);

/** The boot processor's whole x2APIC identity, in either mode: the one it
 * answers to once in x2APIC mode, whose low eight bits are its xAPIC
 * identity (SDM, CPUID leaf 0BH), and which a remapping entry in the
 * x2APIC format names it by. */
uint32_t lapic_x2apic_id(void);

/** The APIC identity of the processor running this; 0 while the APIC is
 * not enabled. */
uint32_t lapic_this_id(void);

/** End an interrupt that arrived through the APIC; nothing while the APIC
 * is not enabled. */
void lapic_eoi(void);

/** Whether `vector` is in this processor's request register: raised and
 * not yet taken, as the 8259's request register says of a line. Unlike
 * `lapic_pending`, a vector whose handler is running is not counted. */
int lapic_requested(uint8_t vector);

/** Write local-vector entry `LINT0` (`lint` 0) or `LINT1` (`lint` 1). */
void lapic_set_lint(int lint, uint32_t entry);

/** Write this processor's performance-counter entry, which an overflowing
 * counter raises: `LAPIC_LVT_NMI` for the hard-lockup detector
 * (arch/x86/pmu.c), `LAPIC_LVT_MASKED` to quiet it. Intel's processors
 * set the entry's mask when they deliver the interrupt and AMD's do not;
 * the handler writes it again on both. Nothing while the APIC is not
 * enabled. */
void lapic_set_lvtpc(uint32_t entry);

/** Raise `vector` on this processor by an interprocessor interrupt to
 * itself, as a device's message would: for an edge an I/O APIC input could
 * not see because it came while the input was masked. */
void lapic_self_ipi(uint8_t vector);

/*
 * The timer, this processor's: programmed with `vector`, periodic or
 * one-shot, masked or not, from `initial` counts at the bus clock divided
 * by sixteen; started again from `initial` counts as last programmed;
 * stopped, with its entry masked; and read. Each does nothing, or reads 0,
 * while the APIC is not enabled.
 */
void lapic_timer_set(uint8_t vector, int periodic, int masked, uint32_t initial);
void lapic_timer_count(uint32_t initial);
void lapic_timer_stop(void);
uint32_t lapic_timer_current(void);
uint32_t lapic_timer_initial(void);

/* Local-vector entry values: masked, and a non-maskable interrupt, active
 * low when `LAPIC_LVT_ACTIVE_LOW` is added. */
#define LAPIC_LVT_MASKED     (1u << 16)
#define LAPIC_LVT_NMI        (4u << 8)
#define LAPIC_LVT_ACTIVE_LOW (1u << 13)

/** The spurious vector's handler, which counts the vector on the processor
 * that took it, and the sum of those counts. */
void lapic_spurious(void);
uint32_t lapic_spurious_count(void);

#endif /* _LAPIC_H_ */
