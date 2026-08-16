/*
 * power.c - stopping the machine: halt, power off, restart.
 *
 * The kernel knew one way to stop before this file: a single `cli; hlt` in the
 * debug shell's halt command, which an NMI or an SMI resumes straight into
 * whatever follows. A stop has to be a loop, and a machine that can be asked
 * to power off should be asked rather than parked.
 *
 * Neither the power-off nor the reset is synchronous. An `out` has no
 * completion signal and the platform acts in its own time, so control returning
 * from one is not proof that it was refused; it is only proof that the machine
 * has not stopped yet. Each path therefore waits before it says anything, and
 * then reports what is actually known: that it asked, that it waited, and that
 * it is parking the CPU instead.
 */

#include "power.h"
#include "board.h"
#include "kio.h"
#include "pci.h"
#include "serial.h"

/* The PIIX4 the QEMU `pc` machine carries, at 00:01.3, and the two registers
 * that say where its ACPI power management block lives and whether that block
 * is decoding. */
#define PIIX4_BUS           0
#define PIIX4_DEVICE        1
#define PIIX4_FUNCTION      3
#define PIIX4_VENDOR        0x8086
#define PIIX4_DEVICE_ID     0x7113
#define PIIX4_CFG_VENDOR    0x00
#define PIIX4_CFG_DEVICE    0x02
#define PIIX4_CFG_PMBA      0x40
#define PIIX4_CFG_PMREGMISC 0x80

/* PMREGMISC bit 0: the PM I/O space is decoding. */
#define PIIX4_PMIOSE 0x01

/* PM1a_CNT sits four bytes into the PM block, and SLP_EN with SLP_TYP 0 is
 * what QEMU's PIIX4 model takes as soft-off. */
#define PM1A_CNT_OFFSET 4
#define PM1A_CNT_SLP_EN 0x2000

/* The chipset reset control port. Bit 1 selects a hard reset and bit 2 is the
 * trigger, which acts on a transition from 0 to 1, so it takes two writes. */
#define RESET_CONTROL_PORT 0xCF9
#define RESET_CONTROL_ARM  0x02
#define RESET_CONTROL_FIRE 0x06

/* The APM control port. Writing it raises an SMI, and the firmware's handler
 * decides what the value means; 0xFB is the reset command on the Insyde
 * firmware this kernel is being carried to. On a machine with no SMM handler
 * for it, which includes QEMU as this kernel runs it, the write is inert. */
#define APM_CONTROL_PORT 0xB2
#define APM_CMD_RESET    0xFB

/* The keyboard controller's command port, and the pulse that drives the CPU's
 * reset line on a PC that still wires it that way. Bit 1 of the status is set
 * while the input buffer is full, and a command written then is lost. */
#define KBC_STATUS_PORT  0x64
#define KBC_COMMAND_PORT 0x64
#define KBC_INPUT_FULL   0x02
#define KBC_PULSE_RESET  0xFE

/* Between reset attempts, rather than after the last one. A reset that is
 * going to happen happens within milliseconds, so waiting the full settle
 * three times over would only delay the fallback. */
#define POWER_RESET_SPINS 2000000UL

/* How long to let an asynchronous request land before giving up on it. The
 * unit is an idle loop iteration and the number wants only to be large enough
 * that a machine which is going to stop has stopped. */
#define POWER_SETTLE_SPINS 20000000UL

/**
 * Park the CPU for good. Does not return.
 *
 * A loop and not one `cli; hlt`: an NMI or an SMI wakes a halted CPU, and
 * execution then falls into whatever the assembler put next.
 */
static void power_halt_forever(void)
{
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/**
 * Give an asynchronous request time to take effect. Returns if it did not.
 */
static void power_settle(void)
{
    volatile unsigned long spins = POWER_SETTLE_SPINS;

    while (spins > 0) {
        spins--;
    }
}

/** As power_settle, for the shorter wait between reset attempts. */
static void power_reset_settle(void)
{
    volatile unsigned long spins = POWER_RESET_SPINS;

    while (spins > 0) {
        spins--;
    }
}

/**
 * Ask a PIIX4 to remove power. Returns only if the machine is still running.
 *
 * This is QEMU's power-off and not a general one, and the difference is worth
 * stating rather than implying. The three checks below establish where the PM
 * block is; they do not establish that sleep type 0 means soft-off, which on
 * real hardware is a value firmware publishes in the ACPI tables this kernel
 * does not parse. On QEMU's PIIX4 model the write below powers the machine off;
 * on a board whose S5 is some other type it could ask for a different sleep
 * state instead of falling through to the halt, which is the one outcome here
 * that the fallback cannot catch. Reading S5 properly needs an AML interpreter,
 * which is a milestone of its own.
 *
 * The PM block's base is read out of configuration space rather than assumed,
 * because this kernel also boots real hardware from a GRUB disk and the port
 * that means power-off on a PIIX4 means something else, or nothing, elsewhere.
 * Three things are checked and each rules out a different way of being wrong:
 * the device must be the one this code understands, its PM I/O space must be
 * decoding, and the base must have been programmed. That last check is the one
 * worth naming: bit 0 of PMBA is hardwired to 1 as the I/O-space indicator, so
 * it says nothing about whether firmware ever set a base, and a base left at
 * its reset value would send the sleep word to port 4, which is a live DMA
 * controller register on any machine that has one.
 */
static void power_acpi_off(void)
{
    /* The identified chipset first. `board.c` derived this port from the
     * controller's own base register and refused unless four checks passed, so
     * a nonzero port here is one this kernel is entitled to write. The PIIX4
     * path below stays for a machine no profile matched, which is every
     * machine this kernel ran on before board.c existed. */
    uint16_t port = board_pm1_cnt_port();

    if (port != 0) {
        const board_profile_t *p = board_current();

        /* The token stays exactly what it was: tools/shutdown_test.sh watches
         * for it, so the board's name goes on its own line rather than into
         * the middle of a string another program reads. */
        serial_printf("poweroff: board %s\n", board_name());
        serial_printf("poweroff: powering off\n");
        outw(port, board_sleep_word(p));
        power_settle();
        serial_printf("poweroff: the platform did not power off; halting instead\n");
        return;
    }
    {
        uint16_t vendor =
            pci_read_config16(PIIX4_BUS, PIIX4_DEVICE, PIIX4_FUNCTION, PIIX4_CFG_VENDOR);
        uint16_t device =
            pci_read_config16(PIIX4_BUS, PIIX4_DEVICE, PIIX4_FUNCTION, PIIX4_CFG_DEVICE);
        uint32_t pmba;
        uint8_t misc;
        uint16_t base;

        if (vendor != PIIX4_VENDOR || device != PIIX4_DEVICE_ID) {
            serial_printf("poweroff: no PIIX4 power management; halting instead\n");
            return;
        }
        misc = pci_read_config8(PIIX4_BUS, PIIX4_DEVICE, PIIX4_FUNCTION, PIIX4_CFG_PMREGMISC);
        pmba = pci_read_config32(PIIX4_BUS, PIIX4_DEVICE, PIIX4_FUNCTION, PIIX4_CFG_PMBA);
        base = (uint16_t)(pmba & 0xFFC0u);
        if ((misc & PIIX4_PMIOSE) == 0 || base == 0) {
            serial_printf("poweroff: no PIIX4 power management; halting instead\n");
            return;
        }

        serial_printf("poweroff: powering off\n");
        outw((uint16_t)(base + PM1A_CNT_OFFSET), PM1A_CNT_SLP_EN);
        power_settle();
        serial_printf("poweroff: the platform did not power off; halting instead\n");
    }
}

/** Wait, boundedly, for the keyboard controller to accept a command. */
static int kbc_ready(void)
{
    int spins;

    for (spins = 0; spins < 100000; spins++) {
        if ((inb(KBC_STATUS_PORT) & KBC_INPUT_FULL) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Ask the machine to reset, three ways. Returns only if it is still running.
 *
 * All three ports are fixed rather than discovered, and the asymmetry with the
 * power-off above is deliberate: that one derives an address, and a wrong
 * address would put a sleep word into a live device, so it refuses unless it
 * knows the address is right. Each of these writes a byte to a port that is a
 * reset mechanism on the machines that have it and unclaimed on those that do
 * not, where the write is inert. Discovering them would need a mechanism PCI
 * configuration space does not offer for these registers.
 *
 * The order is by how much of the machine each one goes through, most first.
 * The firmware's own SMI handler resets the way the firmware intends,
 * including whatever it does about the devices it owns; the chipset register
 * resets the chipset; the keyboard controller pulse is the oldest and the
 * least likely to be wired on a recent laptop. Trying them in the other order
 * would work on QEMU, where the chipset register is the one that acts, and
 * leave the target machine parked at a halt, which over SSH is
 * indistinguishable from a kernel that died.
 */
static void power_reset(void)
{
    serial_printf("reboot: restarting\n");

    outb(APM_CONTROL_PORT, APM_CMD_RESET);
    power_reset_settle();

    outb(RESET_CONTROL_PORT, RESET_CONTROL_ARM);
    outb(RESET_CONTROL_PORT, RESET_CONTROL_FIRE);
    power_reset_settle();

    if (kbc_ready()) {
        outb(KBC_COMMAND_PORT, KBC_PULSE_RESET);
    }
    power_settle();
    serial_printf("reboot: the platform did not reset; halting instead\n");
}

/**
 * Stop the machine the way `how` asks. Does not return.
 */
void power_stop(int how)
{
    if (how == POWER_OFF) {
        power_acpi_off();
    } else if (how == POWER_RESTART) {
        power_reset();
    }
    serial_printf("halt: system halted\n");
    power_halt_forever();
}
