/*
 * power.c - stopping the machine: halt, power off, restart.
 *
 * The kernel knew one way to stop before this file: a single `cli; hlt` in the
 * debug shell's halt command, which an NMI or an SMI resumes straight into
 * whatever follows. A stop has to be a loop, and a machine that can be asked
 * to power off should be asked, not parked.
 *
 * Neither the power-off nor the reset is synchronous. An `out` has no
 * completion signal and the platform acts in its own time, so control returning
 * from one is not proof that it was refused; it is only proof that the machine
 * has not stopped yet. Each path therefore waits before it says anything, and
 * then reports what is actually known: that it asked, that it waited, and that
 * it is parking the CPU instead.
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

/* Between reset attempts, not after the last one. A reset that is
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
 * stop the power off. A hardware-reduced machine has no PM1 registers and
 * takes the sleep type and its enable in its sleep control register.
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
 * Ask a PIIX4 to remove power. Returns only if the machine is still running.
 *
 * This is QEMU's power-off and not a general one, the fallback for a machine
 * whose ACPI tables gave no \_S5 and whose chipset no board profile matched.
 * The three checks below establish where the PM block is; they do not
 * establish that sleep type 0 means soft-off, which on real hardware is the
 * value \_S5 publishes. On QEMU's PIIX4 model the write below powers the
 * machine off; on a board whose S5 is some other type it could ask for a
 * different sleep state instead of falling through to the halt, which is the
 * one outcome here that the fallback cannot catch.
 *
 * The PM block's base is read out of configuration space, not assumed,
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

        /* The token stays exactly what it was: the shutdown suite watches
         * for it, so the board's name goes on its own line, not into
         * the middle of a string another program reads. */
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

/*
 * The reset's own writes, so that the decision can be watched.
 *
 * A reset does not return, so the only way to see which ports it writes is to
 * put the writes behind something a test can replace. The machine's binding
 * is `outb` and the real firmware's table; a host test supplies its own.
 */
struct power_io {
    void (*outb)(uint16_t port, uint8_t value);
    int (*kbc_ready)(void);
    void (*settle)(void);
    const char *(*vendor)(void);
    const struct acpi_info *(*acpi)(void);
};

static void power_outb(uint16_t port, uint8_t value)
{
    outb(port, value);
}

static void power_io_settle(void)
{
    power_reset_settle();
}

static const struct power_io power_mmio = {power_outb, kbc_ready, power_io_settle,
                                           smbios_bios_vendor, acpi_get};
static const struct power_io *power_io = &power_mmio;

/**
 * Whether the firmware's own reset command is this firmware's.
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
 * them in the other order would work on QEMU, where the chipset register is
 * the one that acts, and leave the target machine parked at a halt, which
 * over SSH is indistinguishable from a kernel that died.
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

    /* `rach2.resetskip=N` leaves out the first N mechanisms before the
     * keyboard controller's pulse: the FADT's register, where there is one,
     * then the firmware's command and the chipset register. It exists so
     * the later ones can be shown to work on their own, which is the only
     * way to know the sequence is a real fallback chain, not a first method
     * that happens to work everywhere it has been tried; `tests/run reset`
     * boots each value. It is also the switch to reach for on a machine
     * where an earlier method makes things worse. */
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

    if (method++ >= skip) {
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
 * Stop the machine the way `how` asks. Does not return.
 */
void power_stop(int how)
{
    /* The screen comes back. It was handed to the login when init started, and
     * what follows is the end of the machine: either it stops and says so, or
     * the hardware refuses and the person is looking at a laptop that is still
     * on with no way left to ask why. There is no log to read afterwards. */
    klog_set_screen_level(KLOG_INFO);
    /* One processor stops the machine; the others stop first, so that none
     * goes on writing to a disk or a device the stop leaves behind. */
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
