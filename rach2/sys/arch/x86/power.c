/*
 * power.c: stopping the PC: halt, power off, restart, as power.h describes.
 *
 * Neither the power off nor the reset is synchronous. An `out` has no
 * completion signal and the platform acts in its own time, so control
 * returning from one does not show that it was refused, only that the
 * machine has not stopped yet. Each path therefore waits before it reports,
 * and then reports what is known: that it asked, that it waited, and that it
 * is halting the processor instead.
 */

#include "acpi.h"
#include "portio.h"
#include "../../klog.h"
#include "../../power.h"
#include "../../proc/smp.h"

#include "smbios.h"
#include "board.h"
#include "../../cmdline.h"
#include "../../kio.h"
#include "../../pci.h"
#include "../../serial.h"

/* The PIIX4 the QEMU `pc` machine has, at 00:01.3, and the two registers
 * that give the base of its ACPI power management block and say whether that
 * block is decoding. */
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

/* PM1a_CNT is four bytes into the PM block, and SLP_EN with SLP_TYP 0 is
 * what QEMU's PIIX4 model takes as soft off. */
#define PM1A_CNT_OFFSET 4
#define PM1A_CNT_SLP_EN 0x2000

/* The chipset reset control port. Bit 1 selects a hard reset and bit 2 is the
 * trigger, which acts on a transition from 0 to 1, so it takes two writes. */
#define RESET_CONTROL_PORT 0xCF9
#define RESET_CONTROL_ARM  0x02
#define RESET_CONTROL_FIRE 0x06

/* The APM control port. Writing it raises an SMI, and the firmware's handler
 * decides what the value means; 0xFB is the reset command of Insyde's
 * firmware, and `power_reset` sends it to no other. */
#define APM_CONTROL_PORT 0xB2
#define APM_CMD_RESET    0xFB

/* The keyboard controller's command port, and the pulse that drives the CPU's
 * reset line on a PC that still wires it that way. Bit 1 of the status is set
 * while the input buffer is full, and a command written then is lost. */
#define KBC_STATUS_PORT  0x64
#define KBC_COMMAND_PORT 0x64
#define KBC_INPUT_FULL   0x02
#define KBC_PULSE_RESET  0xFE

/* The wait after each reset mechanism before the keyboard controller's
 * pulse, which the full settle follows, in idle loop iterations. A reset that
 * is going to happen happens within milliseconds, so a full settle after each
 * would only delay the next. */
#define POWER_RESET_SPINS 2000000UL

/* How long an asynchronous request is given to act, in idle loop iterations:
 * long enough that a machine that is going to stop has stopped. */
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
 * Ask for the soft-off state the way ACPI defines it. Returns only if the
 * machine is still running, or the tables do not say how.
 *
 * The sleep types are \_S5's, which `acpi.c` found in the DSDT. Each PM1
 * control register is read, its sleep type field set and its enable bit
 * cleared, and written; then each is written again with SLP_EN, as the
 * specification orders the two writes (ACPI 6.5, 16.1.7). A machine still
 * in legacy mode, SCI_EN clear, is asked into ACPI mode first through the
 * FADT's SMI command, which some chipsets want before they honour SLP_EN;
 * the wait for it is bounded, since a firmware that does not answer must not
 * stop the power off, and interrupts are off (`power_stop`), so the SCI the
 * change may raise is not taken. A hardware-reduced machine has no PM1
 * registers and takes the sleep type and its enable in its sleep control
 * register.
 */
static void power_acpi_s5(void)
{
    const struct acpi_info *a = acpi_get();
    uint32_t pm1a = 0, pm1b = 0;
    int spin, has_b;

    if (!a->fadt || !a->s5) {
        return;
    }
    if (a->fadt_flags & ACPI_FADT_HW_REDUCED) {
        if (a->sleep_control.address == 0) {
            return;
        }
        klog(KLOG_INFO, "poweroff", "acpi: sleep type %d to the sleep control register",
             (int)a->slp_typa);
        klog(KLOG_INFO, "poweroff", "powering off");
        (void)acpi_gas_write(&a->sleep_control, ((uint32_t)(a->slp_typa & 7u) << 2) | 0x20u);
        power_settle();
        klog(KLOG_ERR, "poweroff", "acpi: the platform did not power off");
        return;
    }
    if (acpi_gas_read(&a->pm1a_cnt, &pm1a) != 0) {
        return;
    }
    has_b = acpi_gas_read(&a->pm1b_cnt, &pm1b) == 0;
    if ((pm1a & ACPI_PM1_SCI_EN) == 0 && a->smi_cmd != 0 && a->smi_cmd <= 0xFFFFu &&
        a->acpi_enable != 0) {
        outb((uint16_t)a->smi_cmd, a->acpi_enable);
        for (spin = 0; spin < 100000 && (pm1a & ACPI_PM1_SCI_EN) == 0; spin++) {
            (void)acpi_gas_read(&a->pm1a_cnt, &pm1a);
        }
    }
    klog(KLOG_INFO, "poweroff", "acpi: sleep types %d and %d", (int)a->slp_typa, (int)a->slp_typb);
    klog(KLOG_INFO, "poweroff", "powering off");
    pm1a = (pm1a & ~(ACPI_PM1_SLP_TYP | ACPI_PM1_SLP_EN)) |
           ((uint32_t)a->slp_typa << ACPI_PM1_SLP_TYP_POS);
    pm1b = (pm1b & ~(ACPI_PM1_SLP_TYP | ACPI_PM1_SLP_EN)) |
           ((uint32_t)a->slp_typb << ACPI_PM1_SLP_TYP_POS);
    (void)acpi_gas_write(&a->pm1a_cnt, pm1a);
    if (has_b) {
        (void)acpi_gas_write(&a->pm1b_cnt, pm1b);
    }
    (void)acpi_gas_write(&a->pm1a_cnt, pm1a | ACPI_PM1_SLP_EN);
    if (has_b) {
        (void)acpi_gas_write(&a->pm1b_cnt, pm1b | ACPI_PM1_SLP_EN);
    }
    power_settle();
    klog(KLOG_ERR, "poweroff", "acpi: the platform did not power off; trying the board table");
}

/**
 * Ask the chipset to remove power, with the board profile's sleep word or a
 * PIIX4's. Returns only if the machine is still running.
 *
 * A nonzero `board_pm1_cnt_port` is the PM1 control register `board_init`
 * derived from the chipset's own base register after its four checks. The
 * board's name goes on a line of its own, apart from "powering off", which
 * the shutdown suite reads.
 *
 * Without a profile in force, the PIIX4 path writes QEMU's soft off, sleep
 * type 0, to a PIIX4 at 00:01.3. Three checks establish where its PM block
 * is: the device must be a PIIX4, its PM I/O space must be decoding, and the
 * base must have been programmed. Bit 0 of PMBA is hardwired to 1 as the
 * I/O-space indicator, so it does not show that firmware set a base, and a
 * base left at its reset value would send the sleep word to port 4, a DMA
 * controller register. The checks do not establish that sleep type 0 means
 * soft off, which on hardware is the value \_S5 gives: on a board whose S5
 * is another type the write could ask for another sleep state instead of
 * falling through to the halt, the one outcome here the fallback cannot
 * catch.
 */
static void power_acpi_off(void)
{
    uint16_t port = board_pm1_cnt_port();

    if (port != 0) {
        const board_profile_t *p = board_current();

        klog(KLOG_INFO, "poweroff", "board %s", board_name());
        klog(KLOG_INFO, "poweroff", "powering off");
        outw(port, board_sleep_word(p));
        power_settle();
        klog(KLOG_ERR, "poweroff", "the platform did not power off; halting instead");
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
            klog(KLOG_ERR, "poweroff", "no PIIX4 power management; halting instead");
            return;
        }
        misc = pci_read_config8(PIIX4_BUS, PIIX4_DEVICE, PIIX4_FUNCTION, PIIX4_CFG_PMREGMISC);
        pmba = pci_read_config32(PIIX4_BUS, PIIX4_DEVICE, PIIX4_FUNCTION, PIIX4_CFG_PMBA);
        base = (uint16_t)(pmba & 0xFFC0u);
        if ((misc & PIIX4_PMIOSE) == 0 || base == 0) {
            klog(KLOG_ERR, "poweroff", "no PIIX4 power management; halting instead");
            return;
        }

        klog(KLOG_INFO, "poweroff", "powering off");
        outw((uint16_t)(base + PM1A_CNT_OFFSET), PM1A_CNT_SLP_EN);
        power_settle();
        klog(KLOG_ERR, "poweroff", "the platform did not power off; halting instead");
    }
}

/** Wait, boundedly, for the keyboard controller to accept a command: 1 when
 * it will, 0 when its input buffer stays full. */
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

/*
 * The reset's port writes, its wait between mechanisms, and the firmware's
 * vendor and ACPI tables it decides from, through one table. `power_mmio`,
 * the kernel's own binding, is the only one, and nothing replaces it;
 * tests/host/power_hosttest.c tests the firmware rule on its own copy,
 * `smbios_vendor_is("Insyde")`, without this file.
 */
struct power_io {
    void (*outb)(uint16_t port, uint8_t value);
    int (*kbc_ready)(void);
    void (*settle)(void);
    const char *(*vendor)(void);
    const struct acpi_info *(*acpi)(void);
};

/** The table's port write, `outb`. */
static void power_outb(uint16_t port, uint8_t value)
{
    outb(port, value);
}

/** The table's wait after a reset mechanism, `power_reset_settle`. */
static void power_io_settle(void)
{
    power_reset_settle();
}

static const struct power_io power_mmio = {power_outb, kbc_ready, power_io_settle,
                                           smbios_bios_vendor, acpi_get};
static const struct power_io *power_io = &power_mmio;

/**
 * Whether the firmware's own reset command is this firmware's: whether the
 * BIOS vendor begins with "Insyde"; an empty vendor does not.
 *
 * `0xFB` on port `0xB2` is a request to an Insyde firmware's
 * system-management handler. Sent to a machine running another firmware it
 * reaches a handler written for something else, which may do anything at all,
 * so it goes only where the firmware says it belongs.
 */
static int firmware_takes_smi_reset(void)
{
    const char *v = power_io->vendor();
    const char *want = "Insyde";
    int i;

    for (i = 0; want[i] != '\0'; i++) {
        if (v[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * Ask the machine to reset, up to four ways. Returns only if it is still
 * running.
 *
 * The first is the FADT's reset register, where the FADT says it has one:
 * the way ACPI defines, the register and the value the firmware publishes,
 * written in whatever space it gives. The other three ports are fixed, not
 * discovered, and the asymmetry with the power-off above is deliberate: that
 * one derives an address, and a wrong address would put a sleep word into a
 * live device, so it refuses unless it knows the address is right. Each of
 * these writes a byte to a port that is a reset mechanism on the machines
 * that have it and unclaimed on those that do not, where the write is inert.
 *
 * The order of the three is by how much of the machine each one goes
 * through, most first. The firmware's own SMI handler resets the way the
 * firmware intends, including whatever it does about the devices it owns;
 * the chipset register resets the chipset; the keyboard controller pulse is
 * the oldest and the least likely to be wired on a recent laptop. Trying
 * them in the other order works on QEMU, where the chipset register is the
 * one that acts, and leaves the Lenovo halted, which over SSH looks the same
 * as a kernel that died.
 *
 * `rach2.resetskip=N`, a single digit, leaves out the first N mechanisms
 * before the keyboard controller's pulse: the FADT's register, where there
 * is one, then the firmware's command and the chipset register. It lets a
 * test show that each later mechanism resets the machine on its own, which
 * is what makes the sequence a chain of fallbacks; `tests/run reset` boots
 * each value. It also leaves out an earlier mechanism that misbehaves on a
 * machine.
 */
static void power_reset(void)
{
    const struct acpi_info *a = power_io->acpi();
    int skip = 0, method = 0;
    char value[8];

    if (kcmdline_value("rach2.resetskip", value, (unsigned int)sizeof(value))) {
        if (value[0] >= '0' && value[0] <= '9' && value[1] == '\0') {
            skip = value[0] - '0';
        }
    }
    klog(KLOG_INFO, "reboot", "restarting (skipping %d)", skip);

    if (a->reset_supported && method++ >= skip) {
        klog(KLOG_INFO, "reboot", "acpi: writing 0x%x to the reset register",
             (unsigned)a->reset_value);
        if (a->reset_reg.space == ACPI_SPACE_IO && a->reset_reg.address <= 0xFFFFu) {
            power_io->outb((uint16_t)a->reset_reg.address, a->reset_value);
        } else {
            (void)acpi_gas_write(&a->reset_reg, a->reset_value);
        }
        power_io->settle();
    }

    if (method++ >= skip) {
        if (firmware_takes_smi_reset()) {
            klog(KLOG_INFO, "reboot", "the firmware is %s: sending its own reset",
                 power_io->vendor());
            power_io->outb(APM_CONTROL_PORT, APM_CMD_RESET);
            power_io->settle();
        } else {
            klog(KLOG_INFO, "reboot", "the firmware is %s, not Insyde: its reset is not sent",
                 power_io->vendor()[0] != '\0' ? power_io->vendor() : "not named");
        }
    }

    if (method >= skip) {
        power_io->outb(RESET_CONTROL_PORT, RESET_CONTROL_ARM);
        power_io->outb(RESET_CONTROL_PORT, RESET_CONTROL_FIRE);
        power_io->settle();
    }

    if (power_io->kbc_ready()) {
        power_io->outb(KBC_COMMAND_PORT, KBC_PULSE_RESET);
    }
    power_settle();
    klog(KLOG_ERR, "reboot", "the platform did not reset; halting instead");
}

/**
 * The screen shows the log down to KLOG_INFO again, after the boot turned
 * it down for the login: the machine either stops and says so, or does not,
 * and a person at its screen has no log to read afterwards.
 *
 * Interrupts then go off and stay off, as in Linux's `acpi_power_off`:
 * entering ACPI mode can raise the SCI, whose source no handler here clears,
 * and a level-triggered line it shares with a device would interrupt the
 * processor for good before it reached SLP_EN. One processor stops the
 * machine; the others stop first, so that none goes on writing to a disk or
 * a device the stop leaves behind.
 */
void power_stop(int how)
{
    klog_set_screen_level(KLOG_INFO);
    arch_irq_disable();
    smp_stop_others();

    if (how == POWER_OFF) {
        power_acpi_s5();
        power_acpi_off();
    } else if (how == POWER_RESTART) {
        power_reset();
    }
    klog(KLOG_INFO, "halt", "system halted");
    power_halt_forever();
}
