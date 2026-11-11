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
#include "../../acpi/acpica.h"
#include "../../klog.h"
#include "../../power.h"
#include "../../proc/smp.h"

#include "smbios.h"
#include "board.h"
#include "itco.h"
#include "../../cmdline.h"
#ifndef RACH2_HOSTTEST
#include "../arch.h"
#include "../../panic.h"
#include "../../udelay.h"
#endif

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
 * Ask the chipset to remove power with the board profile's sleep word.
 * Returns only if the machine is still running.
 *
 * The port is the PM1 control register `board_init` derived from the
 * chipset's own base register after its four checks, or 0 when no profile
 * passed them; then there is nothing here to write to. The board's name goes
 * on a line of its own, apart from "powering off", which the shutdown suite
 * reads.
 */
static void power_board_off(void)
{
    uint16_t port = board_pm1_cnt_port();

    if (port == 0) {
        klog(KLOG_ERR, "poweroff", "no board power controller; halting instead");
        return;
    }
    klog(KLOG_INFO, "poweroff", "board %s", board_name());
    klog(KLOG_INFO, "poweroff", "powering off");
    outw(port, board_sleep_word(board_current()));
    power_settle();
    klog(KLOG_ERR, "poweroff", "the platform did not power off; halting instead");
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
    return smbios_vendor_is("Insyde");
}

/**
 * Ask the machine to reset, up to four ways. Returns only if it is still
 * running.
 *
 * The first is the FADT's reset register, where the FADT says it has one:
 * the way ACPI defines, the register and the value the firmware publishes,
 * written by `acpi_reset_write`. The other three ports are fixed, not
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
    const struct acpi_info *a = acpi_get();
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
        acpi_reset_write(a);
        power_reset_settle();
    }

    if (method++ >= skip) {
        if (firmware_takes_smi_reset()) {
            klog(KLOG_INFO, "reboot", "the firmware is %s: sending its own reset",
                 smbios_bios_vendor());
            outb(APM_CONTROL_PORT, APM_CMD_RESET);
            power_reset_settle();
        } else {
            klog(KLOG_INFO, "reboot", "the firmware is %s, not Insyde: its reset is not sent",
                 smbios_bios_vendor()[0] != '\0' ? smbios_bios_vendor() : "not named");
        }
    }

    if (method >= skip) {
        outb(RESET_CONTROL_PORT, RESET_CONTROL_ARM);
        outb(RESET_CONTROL_PORT, RESET_CONTROL_FIRE);
        power_reset_settle();
    }

    if (kbc_ready()) {
        outb(KBC_COMMAND_PORT, KBC_PULSE_RESET);
    }
    power_settle();
    klog(KLOG_ERR, "reboot", "the platform did not reset; halting instead");
}

/* A host test runs the requests above without ACPICA and the stop, whose
 * `cli` and `hlt` it can neither assemble on every host nor run. */
#ifndef RACH2_HOSTTEST

/* The methods of Linux's `reboot=` (include/linux/reboot.h, `reboot_type`),
 * by the letters it reads: the FADT's reset register, the keyboard
 * controller, EFI's ResetSystem, the BIOS's restart, port 0xCF9 forced and
 * as Linux's PCI probe allows it, and a triple fault. */
#define REBOOT_ACPI     'a'
#define REBOOT_KBD      'k'
#define REBOOT_EFI      'e'
#define REBOOT_BIOS     'b'
#define REBOOT_CF9      'p'
#define REBOOT_CF9_SAFE 'q'
#define REBOOT_TRIPLE   't'

/* The BIOS data area's reset flag, which the BIOS reads at its next start:
 * 0x1234 skips the memory test, a warm start. */
#define BIOS_RESET_FLAG_PHYS 0x472u
#define BIOS_RESET_WARM      0x1234u

/* What `reboot=` gave: the method to start from, and the reset's warmth,
 * for every restart and for a panic's, where -1 is none given. */
struct reboot_options {
    char type;
    int warm;
    int panic_warm;
};

/**
 * Read every `reboot=` of the boot line into `*o`, in order, as Linux's
 * `reboot_setup` reads each (kernel/reboot.c): a list split by commas, each
 * item judged by its first letter after an optional `panic_`, which makes a
 * mode the panic's alone. `w` is warm, and `c`, `h`, `g` and `s` without a
 * processor number are modes that are not warm; `s` with a number names the
 * processor to restart on, which a panic does not move to, and is passed
 * over; `a`, `k`, `t`, `e`, `b` and `p` name the method; `f`, which spares a
 * restart its shutdown, has no meaning here, where the caller shuts down;
 * any other letter is passed over, as Linux passes it over.
 */
static void reboot_read(struct reboot_options *o)
{
    char value[64];
    const char *p;
    int nth, *mode;

    o->type = REBOOT_ACPI;
    o->warm = 0;
    o->panic_warm = -1;
    for (nth = 0; kcmdline_value_nth("reboot", nth, value, (unsigned int)sizeof(value)); nth++) {
        for (p = value; *p != '\0';) {
            mode = &o->warm;
            if (p[0] == 'p' && p[1] == 'a' && p[2] == 'n' && p[3] == 'i' && p[4] == 'c' &&
                p[5] == '_') {
                mode = &o->panic_warm;
                p += 6;
            }
            switch (*p) {
            case 'w':
                *mode = 1;
                break;
            case 'c':
            case 'h':
            case 'g':
                *mode = 0;
                break;
            case 's':
                p += p[1] == 'm' && p[2] == 'p' ? 3 : 1;
                if (*p < '0' || *p > '9') {
                    *mode = 0;
                }
                break;
            case 'a':
            case 'k':
            case 't':
            case 'e':
            case 'b':
            case 'p':
                o->type = *p;
                break;
            default:
                break;
            }
            while (*p != '\0' && *p != ',') {
                p++;
            }
            if (*p == ',') {
                p++;
            }
        }
    }
}

/** Log `what`, the method about to be tried, the first time it is tried;
 * `tried` holds a bit for each letter already logged. */
static void reboot_say(uint32_t *tried, char type, const char *what)
{
    uint32_t bit = 1u << (type - 'a');

    if ((*tried & bit) == 0) {
        *tried |= bit;
        klog(KLOG_INFO, "reboot", "emergency: %s", what);
    }
}

/**
 * Write the FADT's reset value to its reset register, as Linux's
 * `acpi_reboot` does (drivers/acpi/reboot.c:36-82): only where the FADT,
 * revision 2 or later, says it has one (`reset_supported`), a byte in I/O
 * or PCI space as `acpi_reset_write` says, and then wait 15 ms, which some
 * machines take to act before the next method could race them.
 */
static void reboot_acpi(void)
{
    const struct acpi_info *a = acpi_get();

    if (!a->reset_supported) {
        return;
    }
    acpi_reset_write(a);
    k_udelay(15000);
}

/**
 * Wait, as Linux's `kb_wait` does, until the keyboard controller's input
 * buffer is empty, or 0x10000 reads 2 us apart have found it full.
 */
static void reboot_kb_wait(void)
{
    int i;

    for (i = 0; i < 0x10000; i++) {
        if ((inb(KBC_STATUS_PORT) & KBC_INPUT_FULL) == 0) {
            break;
        }
        k_udelay(2);
    }
}

/**
 * Load an empty interrupt table and raise a breakpoint: the processor finds
 * no gate for it, nor for the faults that follow, and shuts down, which a PC
 * turns into a reset. The ten zero bytes are a limit of 0 and a base of 0 in
 * the form either `lidt` reads, six bytes on i686 and ten on x86-64.
 */
static void reboot_triple_fault(void)
{
    static const uint16_t no_idt[5];

    __asm__ __volatile__("lidt (%0)\n\tint3" : : "r"(no_idt) : "memory");
}

/**
 * The loop of Linux's `native_machine_emergency_restart`, method by method,
 * as power.h describes it. Linux tries 0xCF9 in its default order only once
 * its PCI probe has found configuration mechanism 1 or 2
 * (arch/x86/pci/direct.c, `port_cf9_safe`); this kernel reaches PCI through
 * mechanism 1 or the MCFG window on every PC it runs on (pciconf.c), so the
 * port is safe here from the start. The warm reset's value at 0xCF9 is 0x06,
 * a cold one's 0x0E, whose bit 3 asks the chipset to cycle the power too,
 * with the register's other bits kept, as Linux writes them.
 */
static void reboot_linux(void)
{
    struct reboot_options o;
    char type, first;
    int attempt = 0, i, warm;
    uint32_t tried = 0;
    uint8_t code, cf9;

    reboot_read(&o);
    warm = k_panicking() && o.panic_warm >= 0 ? o.panic_warm : o.warm;
    *(volatile uint16_t *)phys_to_virt(BIOS_RESET_FLAG_PHYS) =
        (uint16_t)(warm ? BIOS_RESET_WARM : 0u);
    type = first = o.type;
    for (;;) {
        switch (type) {
        case REBOOT_ACPI:
            reboot_say(&tried, type,
                       acpi_get()->reset_supported ? "acpi reset register"
                                                   : "acpi: the fadt has no reset register");
            reboot_acpi();
            type = REBOOT_KBD;
            break;
        case REBOOT_KBD:
            reboot_say(&tried, type, "keyboard controller");
            for (i = 0; i < 10; i++) {
                reboot_kb_wait();
                k_udelay(50);
                outb(KBC_COMMAND_PORT, KBC_PULSE_RESET);
                k_udelay(50);
            }
            if (attempt == 0 && first == REBOOT_ACPI) {
                attempt = 1;
                type = REBOOT_ACPI;
            } else {
                type = REBOOT_EFI;
            }
            break;
        case REBOOT_EFI:
            reboot_say(&tried, type, "efi: not called by this kernel");
            type = REBOOT_BIOS;
            break;
        case REBOOT_BIOS:
            reboot_say(&tried, type, "bios: not called by this kernel");
            type = REBOOT_CF9_SAFE;
            break;
        case REBOOT_CF9:
        case REBOOT_CF9_SAFE:
            reboot_say(&tried, REBOOT_CF9, "port 0xcf9");
            code = (uint8_t)(warm ? 0x06u : 0x0Eu);
            cf9 = (uint8_t)(inb(RESET_CONTROL_PORT) & ~code);
            outb(RESET_CONTROL_PORT, (uint8_t)(cf9 | 0x02u));
            k_udelay(50);
            outb(RESET_CONTROL_PORT, (uint8_t)(cf9 | code));
            k_udelay(50);
            type = REBOOT_TRIPLE;
            break;
        default:
            reboot_say(&tried, REBOOT_TRIPLE, "triple fault");
            reboot_triple_fault();
            type = REBOOT_KBD;
            break;
        }
    }
}

void power_emergency_restart(void)
{
    arch_irq_disable();
    reboot_linux();
    for (;;) {
    }
}

void power_prepare(int how)
{
    itco_shutdown();
    if (how == POWER_OFF) {
        (void)acpica_power_off_prepare();
    }
}

/**
 * Ask for the soft-off state through ACPICA, where `power_prepare` ran the
 * firmware's \_PTS(5). Returns only if the machine is still running, or
 * ACPICA did not write SLP_EN.
 *
 * ACPICA clears every status and disables every GPE before the write, as
 * Linux's acpi_power_off does, since a chipset wakes at once from a status
 * left set and enabled, as a press of the power button leaves PWRBTN_STS.
 */
static void power_acpica_s5(void)
{
    if (acpica_power_off() != 0) {
        return;
    }
    power_settle();
    klog(KLOG_ERR, "poweroff", "acpica: the platform did not power off; trying \\_S5 itself");
}

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
        power_acpica_s5();
        power_acpi_s5();
        power_board_off();
    } else if (how == POWER_RESTART) {
        if (kcmdline_count("reboot") > 0) {
            reboot_linux();
        }
        power_reset();
    }
    klog(KLOG_INFO, "halt", "system halted");
    power_halt_forever();
}

#endif /* !RACH2_HOSTTEST */
