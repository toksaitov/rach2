/*
 * board.c: identify the power controller before writing to it.
 *
 * The table below is short and every row is a claim about a real chipset. Two
 * of the three can be met on this desk; the third is the laptop's, and its row
 * is the one that has never been executed, which is why the judgements are
 * pure functions with a host test, not code that is only read.
 */

#include "board.h"
#include "pci.h"
#include "pic.h"
#include "cmdline.h"
#include "serial.h"
#include "kio.h"

/*
 * The profiles, in probe order.
 *
 * piix4 reproduces what power.c did before this file existed, field for field:
 * the same function, the same offsets, the same mask, and sleep type 0, which
 * is what QEMU's PIIX4 model takes as soft off.
 *
 * ich9 exists for one reason, and it is the only
 * PCH-shaped power controller available here, so it is what makes the
 * derivation, the decode check, the SCI selection read and the enable
 * handshake provable in QEMU at all (`-machine q35`). Its sleep type is
 * QEMU's convention, not the ICH9 datasheet's.
 *
 * The two Lynx Point rows are the laptop's family, split because the mobile
 * parts are a different device-id range from the desktop ones and the first
 * boot on the real machine matched neither: its SATA controller reports
 * 8086:9c03, which is Lynx Point-LP, so its LPC bridge is in the 0x9c4x range
 * and not the 0x8c4x one this table originally carried. Wildcat Point-LP is
 * the Broadwell refresh of the same chassis and is listed beside it.
 *
 * Their two constants of belief are marked as such: sleep type 7 is the Intel
 * PCH encoding for S5, and 0xA0 is the ACPI enable value the firmware
 * publishes. Neither is measured; only the machine powering off confirms them.
 */
static const board_profile_t profiles[] = {
    {"piix4", 0, 1, 3, 0x8086, 0x7113, 0x7113, 0xFF, 0xFF, 0x40, 0xFFC0, 0x80, 0, 0, 9, 0, 0, 0},
    {"ich9", 0, 31, 0, 0x8086, 0x2918, 0x2918, 0x06, 0x01, 0x40, 0xFF80, 0x44, 7, 0x44, -1, 0, 0xB2,
     0x02},
    {"lynxpoint", 0, 31, 0, 0x8086, 0x8C40, 0x8C5F, 0x06, 0x01, 0x40, 0xFF80, 0x44, 7, 0x44, -1, 7,
     0xB2, 0xA0},
    {"lynxpoint-lp", 0, 31, 0, 0x8086, 0x9C40, 0x9C4F, 0x06, 0x01, 0x40, 0xFF80, 0x44, 7, 0x44, -1,
     7, 0xB2, 0xA0},
    {"wildcatpoint-lp", 0, 31, 0, 0x8086, 0x9CC0, 0x9CCF, 0x06, 0x01, 0x40, 0xFF80, 0x44, 7, 0x44,
     -1, 7, 0xB2, 0xA0}};

#define BOARD_NPROFILES ((int)(sizeof(profiles) / sizeof(profiles[0])))

static int board_verdict = BOARD_ABSENT;
static const board_profile_t *board_found;
static uint16_t board_port;
static int board_sci = -1;

const board_profile_t *board_lookup(const char *name)
{
    int i, j;

    if (!name) {
        return 0;
    }
    for (i = 0; i < BOARD_NPROFILES; i++) {
        const char *a = profiles[i].name;

        for (j = 0; a[j] != '\0' && a[j] == name[j]; j++) {
            /* compare each byte of the profile's name against the caller's */
        }
        if (a[j] == '\0' && name[j] == '\0') {
            return &profiles[i];
        }
    }
    return 0;
}

int board_id_matches(const board_profile_t *p, uint16_t vendor, uint16_t device, uint8_t class_code,
                     uint8_t subclass)
{
    if (!p || vendor != p->vendor) {
        return 0;
    }
    if (device < p->id_first || device > p->id_last) {
        return 0;
    }
    if (p->class_code != 0xFF && class_code != p->class_code) {
        return 0;
    }
    if (p->subclass != 0xFF && subclass != p->subclass) {
        return 0;
    }
    return 1;
}

uint16_t board_pm_base(const board_profile_t *p, uint32_t base_reg_value)
{
    uint16_t base;

    if (!p) {
        return 0;
    }
    base = (uint16_t)(base_reg_value & p->base_mask);
    return base;
}

int board_pm_decoding(const board_profile_t *p, uint8_t decode_reg_value)
{
    if (!p) {
        return 0;
    }
    return (decode_reg_value & (uint8_t)(1u << p->decode_bit)) != 0;
}

int board_sci_irq(const board_profile_t *p, uint8_t sci_reg_value)
{
    if (!p) {
        return -1;
    }
    if (p->sci_reg == 0) {
        return p->sci_fixed;
    }
    switch (sci_reg_value & 0x07u) {
    case 0:
        return 9;
    case 1:
        return 10;
    case 2:
        return 11;
    default:
        /* 4 through 7 put the SCI on IOAPIC inputs a PIC-only kernel
         * cannot receive, and 3 is reserved. */
        return -1;
    }
}

uint16_t board_sleep_word(const board_profile_t *p)
{
    if (!p) {
        return 0;
    }
    return (uint16_t)(((p->slp_typ & 0x7u) << 10) | BOARD_PM1_CNT_SLP_EN);
}

int board_pm_answers(uint16_t pm1_cnt_value)
{
    /* All ones is an unclaimed I/O port, not a control register. */
    return pm1_cnt_value != 0xFFFFu;
}

int board_needs_acpi_enable(const board_profile_t *p, uint16_t pm1_cnt_value)
{
    if (!p || p->smi_port == 0 || p->acpi_enable == 0) {
        return 0;
    }
    if (!board_pm_answers(pm1_cnt_value)) {
        return 0;
    }
    return (pm1_cnt_value & BOARD_PM1_CNT_SCI_EN) == 0;
}

/**
 * Ask firmware to put the machine into ACPI mode, and wait, boundedly.
 *
 * Writing the enable value to the SMI command port raises a system management
 * interrupt, and the firmware's own handler does the work; the only signal
 * that it finished is SCI_EN setting in PM1_CNT. The wait is bounded because
 * firmware that ignores the request must not stop the boot, and its expiry is
 * an ordinary answer: the machine stays in the mode it was in, which is the
 * mode it booted in.
 *
 * @return 1 when ACPI is enabled, 0 when the request went unanswered.
 */
static int board_acpi_enable(const board_profile_t *p, uint16_t port)
{
    int spin;

    outb(p->smi_port, p->acpi_enable);
    for (spin = 0; spin < 100000; spin++) {
        if ((inw(port) & BOARD_PM1_CNT_SCI_EN) != 0) {
            return 1;
        }
    }
    return 0;
}

int board_init(const char **why)
{
    int i;
    const char *forced;
    char name[24];

    *why = "";
    board_verdict = BOARD_ABSENT;
    board_found = 0;
    board_port = 0;
    board_sci = -1;

    /* `rach2.board=<name>` names a profile instead of probing for one, for a
     * machine whose identity this table does not yet know. It still refuses if
     * the named chipset's own registers do not check out. */
    forced = 0;
    if (kcmdline_value("rach2.board", name, (unsigned int)sizeof(name))) {
        forced = name;
        if (!board_lookup(forced)) {
            *why = "the named board profile is not one this kernel knows";
            board_verdict = BOARD_REFUSED;
            return board_verdict;
        }
    }

    for (i = 0; i < BOARD_NPROFILES; i++) {
        const board_profile_t *p = &profiles[i];
        uint16_t vendor, device, pm1;
        uint8_t class_code, subclass, decode;
        uint32_t base_reg;
        uint16_t base;

        if (forced && board_lookup(forced) != p) {
            continue;
        }
        vendor = pci_read_config16(p->bus, p->device, p->function, PCI_VENDOR_ID);
        device = pci_read_config16(p->bus, p->device, p->function, PCI_DEVICE_ID);
        class_code = pci_read_config8(p->bus, p->device, p->function, PCI_CLASS);
        subclass = pci_read_config8(p->bus, p->device, p->function, PCI_SUBCLASS);
        if (!board_id_matches(p, vendor, device, class_code, subclass)) {
            continue;
        }

        /* From here the chipset IS this profile's, so a failed check is a
         * refusal that gets reported, not a silent move to the next
         * row: something is wrong with a machine this code understands. */
        decode = pci_read_config8(p->bus, p->device, p->function, p->decode_reg);
        if (!board_pm_decoding(p, decode)) {
            *why = "the chipset's ACPI range is not decoding";
            board_verdict = BOARD_REFUSED;
            board_found = p;
            return board_verdict;
        }
        base_reg = pci_read_config32(p->bus, p->device, p->function, p->base_reg);
        base = board_pm_base(p, base_reg);
        if (base == 0) {
            *why = "the chipset's ACPI base was never programmed";
            board_verdict = BOARD_REFUSED;
            board_found = p;
            return board_verdict;
        }

        board_port = (uint16_t)(base + BOARD_PM1_CNT_OFFSET);
        pm1 = inw(board_port);
        if (!board_pm_answers(pm1)) {
            *why = "nothing answers at the chipset's ACPI base";
            board_verdict = BOARD_REFUSED;
            board_found = p;
            board_port = 0;
            return board_verdict;
        }

        if (board_needs_acpi_enable(p, pm1)) {
            if (board_acpi_enable(p, board_port)) {
                serial_printf("board: %s: ACPI enabled\n", p->name);
            } else {
                serial_printf("board: %s: the firmware did not answer the ACPI enable\n", p->name);
            }
            pm1 = inw(board_port);
        }

        /*
         * Mask the SCI, if it is on a line this kernel could take. Nothing
         * here handles it: with ACPI enabled, a power button or a thermal
         * event raises a level-triggered interrupt that no handler clears, and
         * the machine then takes it forever the moment interrupts are enabled.
         * The line comes from the chipset's own selection instead of being
         * assumed to be 9, because masking a line the firmware put a device on
         * would silently take that device away.
         */
        if ((pm1 & BOARD_PM1_CNT_SCI_EN) != 0 && !kcmdline_has("rach2.nosci")) {
            board_sci =
                board_sci_irq(p, pci_read_config8(p->bus, p->device, p->function, p->sci_reg));
            if (board_sci >= 0) {
                pic_mask_irq((uint8_t)board_sci);
                serial_printf("board: %s: masked the SCI on IRQ %d\n", p->name, board_sci);
            }
        }

        board_found = p;
        board_verdict = BOARD_FOUND;
        return board_verdict;
    }

    /*
     * Nothing matched, so say what IS there. A machine whose only output is
     * its screen cannot be asked twice cheaply, and "no profile matched" on
     * its own costs a rebuild and another trip to the machine to learn one
     * number. Printing the identity at the one place every profile looks turns
     * an unknown chipset into a row somebody can add.
     */
    {
        unsigned v = (unsigned)pci_read_config16(0, 31, 0, PCI_VENDOR_ID);
        unsigned d = (unsigned)pci_read_config16(0, 31, 0, PCI_DEVICE_ID);
        unsigned c = (unsigned)pci_read_config8(0, 31, 0, PCI_CLASS);
        unsigned sc = (unsigned)pci_read_config8(0, 31, 0, PCI_SUBCLASS);

        /* On the screen as well as the log: the machine this matters for has
         * no serial port, so a serial-only diagnostic is no diagnostic. */
        k_printf("  board: 0:31.0 is %X:%X class %X:%X\n", v, d, c, sc);
        serial_printf("board: no profile matched: 0:31.0 is %X:%X class %X:%X\n", v, d, c, sc);
    }
    if (forced) {
        *why = "the named board profile's chipset is not in this machine";
        board_verdict = BOARD_REFUSED;
    }
    return board_verdict;
}

int board_state(void)
{
    return board_verdict;
}

const char *board_name(void)
{
    return board_found ? board_found->name : "none";
}

uint16_t board_pm1_cnt_port(void)
{
    return (board_verdict == BOARD_FOUND) ? board_port : 0;
}

int board_sci_line(void)
{
    return board_sci;
}

const board_profile_t *board_current(void)
{
    return (board_verdict == BOARD_FOUND) ? board_found : 0;
}
