/*
 * board_hosttest.c: which power controller may be written to, and with what.
 *
 * host-deps:
 *
 * board.c is included, not linked, so the profile table's own rows can
 * be reached by name and judged. Of the chipsets in that table, piix4 and
 * ich9 are QEMU's models, and the three PCH rows describe hardware no suite
 * boots, so for them this test is the one check that runs.
 *
 * What is being guarded is a 16-bit write to an I/O port derived from a
 * chipset register. Derived wrongly it lands on the DMA controller, the
 * interrupt controller, the timer, the keyboard controller, the CMOS index and
 * data pair, or the reset register, so most of this file is about the values
 * that must yield no port.
 */

extern int printf(const char *fmt, ...);

/* The stateful half of board.c reaches PCI, the PIC, the command line and the
   serial log. None of it runs here; the judgements depend only on their
   arguments. */
#include "../../rach2/sys/arch/x86/board.h"

/** Discard the kernel log: the test reads the judgements' results. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** A configuration read of no device: all ones. */
unsigned short pci_read_config16(unsigned char b, unsigned char d, unsigned char f, unsigned char o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0xFFFF;
}

/** A configuration read of no device: all ones. */
unsigned char pci_read_config8(unsigned char b, unsigned char d, unsigned char f, unsigned char o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0xFF;
}

/** A configuration read of no device: all ones. */
unsigned int pci_read_config32(unsigned char b, unsigned char d, unsigned char f, unsigned char o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0xFFFFFFFFu;
}

/** Mask nothing. */
void pic_mask_irq(unsigned char irq)
{
    (void)irq;
}

/** An empty command line: no flag. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

/** An empty command line: no value. */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    (void)key;
    (void)out;
    (void)outlen;
    return 0;
}

/** Discard the serial log. */
void k_printf(const char *fmt, ...)
{
    (void)fmt;
}

/** A read of an unclaimed port, for board.c's enable handshake, which no
 * row reaches: board_init is never called. */
unsigned short inw(unsigned short port)
{
    (void)port;
    return 0xFFFF;
}

/** A port write that goes nowhere, for the same handshake. */
void outb(unsigned short port, unsigned char value)
{
    (void)port;
    (void)value;
}

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/* The table's five rows, which main looks up by name. */
static const board_profile_t *piix4, *ich9, *lynx, *lynxlp, *wildcat;

/**
 * Judge the identity: the chipset must be the one the row describes, and a
 * range's edges, and the neighbouring rows' ids, are the cases that matter.
 */
static void check_identity(void)
{
    CHECK(board_id_matches(piix4, 0x8086, 0x7113, 0x06, 0x80),
          "the PIIX4's own identity matches its row");
    CHECK(!board_id_matches(piix4, 0x1022, 0x7113, 0x06, 0x80),
          "another vendor's device with the same id does not match");
    CHECK(!board_id_matches(piix4, 0x8086, 0x7112, 0x06, 0x80),
          "a neighbouring device id does not match");

    CHECK(board_id_matches(ich9, 0x8086, 0x2918, 0x06, 0x01), "the ICH9 matches its row");
    CHECK(!board_id_matches(ich9, 0x8086, 0x2918, 0x06, 0x00),
          "the right id with the wrong subclass does not match");
    CHECK(!board_id_matches(ich9, 0x8086, 0x2918, 0x0C, 0x01),
          "the right id with the wrong class does not match");

    CHECK(board_id_matches(lynx, 0x8086, 0x8C49, 0x06, 0x01),
          "the expected Lynx Point LPC bridge matches");
    CHECK(board_id_matches(lynx, 0x8086, 0x8C40, 0x06, 0x01), "the bottom of the range matches");
    CHECK(board_id_matches(lynx, 0x8086, 0x8C5F, 0x06, 0x01), "the top of the range matches");
    CHECK(!board_id_matches(lynx, 0x8086, 0x8C3F, 0x06, 0x01), "one below the range does not");
    CHECK(!board_id_matches(lynx, 0x8086, 0x8C60, 0x06, 0x01), "one above the range does not");
    CHECK(!board_id_matches(lynx, 0x8086, 0x9C43, 0x06, 0x01),
          "a Lynx Point-LP bridge is not a desktop Lynx Point");

    CHECK(board_id_matches(lynxlp, 0x8086, 0x9C43, 0x06, 0x01),
          "a Lynx Point-LP LPC bridge matches the LP row");
    CHECK(board_id_matches(lynxlp, 0x8086, 0x9C40, 0x06, 0x01),
          "the bottom of the Lynx Point-LP range matches");
    CHECK(board_id_matches(lynxlp, 0x8086, 0x9C4F, 0x06, 0x01),
          "the top of the Lynx Point-LP range matches");
    CHECK(!board_id_matches(lynxlp, 0x8086, 0x9C3F, 0x06, 0x01),
          "one below the Lynx Point-LP range does not");
    CHECK(!board_id_matches(lynxlp, 0x8086, 0x9C50, 0x06, 0x01),
          "one above the Lynx Point-LP range does not");
    CHECK(!board_id_matches(lynxlp, 0x8086, 0x9C43, 0x06, 0x00),
          "a Lynx Point-LP id with the wrong subclass does not match");
    CHECK(!board_id_matches(lynxlp, 0x8086, 0x8C49, 0x06, 0x01),
          "a desktop Lynx Point bridge is not an LP");

    CHECK(board_id_matches(wildcat, 0x8086, 0x9CC3, 0x06, 0x01),
          "a Wildcat Point-LP LPC bridge matches its row");
    CHECK(board_id_matches(wildcat, 0x8086, 0x9CC0, 0x06, 0x01),
          "the bottom of the Wildcat Point-LP range matches");
    CHECK(board_id_matches(wildcat, 0x8086, 0x9CCF, 0x06, 0x01),
          "the top of the Wildcat Point-LP range matches");
    CHECK(!board_id_matches(wildcat, 0x8086, 0x9CBF, 0x06, 0x01),
          "one below the Wildcat Point-LP range does not");
    CHECK(!board_id_matches(wildcat, 0x8086, 0x9CD0, 0x06, 0x01),
          "one above the Wildcat Point-LP range does not");
    CHECK(!board_id_matches(wildcat, 0x8086, 0x9CC3, 0x06, 0x00),
          "a Wildcat Point-LP id with the wrong subclass does not match");
    CHECK(!board_id_matches(wildcat, 0x8086, 0x9C43, 0x06, 0x01),
          "a Lynx Point-LP bridge is not a Wildcat Point-LP");
    CHECK(!board_id_matches(0, 0x8086, 0x8C49, 0x06, 0x01), "no profile matches nothing");
}

/**
 * Judge the base, the number a mistake turns into a wrong port; the decode
 * bit, which says whether the block answers at all; and the SCI line, which
 * is masked and must not be the wrong one.
 */
static void check_port_and_line(void)
{
    CHECK(board_pm_base(piix4, 0x00001001u) == 0x1000, "the PIIX4 base is masked to its bits");
    CHECK(board_pm_base(piix4, 0x00000001u) == 0,
          "a register holding only the I/O indicator is refused, not read as port 0");
    CHECK(board_pm_base(piix4, 0x00000000u) == 0, "an unprogrammed register is refused");
    CHECK(board_pm_base(lynx, 0x00001801u) == 0x1800,
          "the laptop's expected base comes out of its register");
    CHECK(board_pm_base(lynx, 0x0000183Fu) == 0x1800,
          "and the low bits below the mask are not part of it");
    CHECK(board_pm_base(lynx, 0xFFFFFFFFu) == 0xFF80,
          "an all-ones register yields the masked value, not a wild one");
    CHECK(board_pm_base(lynxlp, 0x0000183Fu) == 0x1800, "the Lynx Point-LP base has the PCH mask");
    CHECK(board_pm_base(lynxlp, 0xFFFFFFFFu) == 0xFF80,
          "and an all-ones Lynx Point-LP register yields the masked value");
    CHECK(board_pm_base(wildcat, 0x0000183Fu) == 0x1800,
          "the Wildcat Point-LP base has the PCH mask");
    CHECK(board_pm_base(wildcat, 0xFFFFFFFFu) == 0xFF80,
          "and an all-ones Wildcat Point-LP register yields the masked value");
    CHECK(board_pm_base(0, 0x1801u) == 0, "no profile yields no base");

    CHECK(board_pm_decoding(piix4, 0x01), "the PIIX4 decode bit is bit 0");
    CHECK(!board_pm_decoding(piix4, 0xFE), "and everything but bit 0 does not enable it");
    CHECK(board_pm_decoding(lynx, 0x80), "the PCH decode bit is bit 7");
    CHECK(!board_pm_decoding(lynx, 0x7F), "and the low bits do not stand in for it");
    CHECK(board_pm_decoding(lynxlp, 0x80), "the Lynx Point-LP decode bit is bit 7");
    CHECK(!board_pm_decoding(lynxlp, 0x7F), "and its low bits do not stand in for it");
    CHECK(board_pm_decoding(wildcat, 0x80), "the Wildcat Point-LP decode bit is bit 7");
    CHECK(!board_pm_decoding(wildcat, 0x7F), "and its low bits do not stand in for it");
    CHECK(!board_pm_decoding(0, 0xFF), "no profile decodes nothing");

    CHECK(board_sci_irq(piix4, 0x00) == 9, "the PIIX4's SCI line is fixed at 9");
    CHECK(board_sci_irq(piix4, 0xFF) == 9, "and no register value changes it");
    CHECK(board_sci_irq(lynx, 0x00) == 9, "selection 0 is IRQ 9");
    CHECK(board_sci_irq(lynx, 0x01) == 10, "selection 1 is IRQ 10");
    CHECK(board_sci_irq(lynx, 0x02) == 11, "selection 2 is IRQ 11");
    CHECK(board_sci_irq(lynx, 0x03) == -1, "selection 3 is reserved and yields no line");
    CHECK(board_sci_irq(lynx, 0x04) == -1, "selection 4 is an IOAPIC input this kernel cannot see");
    CHECK(board_sci_irq(lynx, 0x07) == -1, "and so is selection 7");
    CHECK(board_sci_irq(lynx, 0x82) == 11,
          "the selection is the low three bits, whatever the rest of the register has");
    CHECK(board_sci_irq(lynxlp, 0x81) == 10, "the Lynx Point-LP reads its selection register");
    CHECK(board_sci_irq(lynxlp, 0x03) == -1, "and its reserved selection yields no line");
    CHECK(board_sci_irq(wildcat, 0x81) == 10, "the Wildcat Point-LP reads its selection register");
    CHECK(board_sci_irq(wildcat, 0x03) == -1, "and its reserved selection yields no line");
    CHECK(board_sci_irq(0, 0x00) == -1, "no profile has no line");
}

/**
 * Judge the sleep word, which the table gives and no register is read for;
 * whether anything answered; and whether ACPI must be enabled.
 */
static void check_sleep_and_enable(void)
{
    CHECK(board_sleep_word(piix4) == 0x2000,
          "the PIIX4 word is sleep type 0 with SLP_EN, which is what QEMU takes as off");
    CHECK(board_sleep_word(ich9) == 0x2000, "and so is the ICH9's");
    CHECK(board_sleep_word(lynx) == 0x3C00,
          "the laptop's word is sleep type 7 with SLP_EN, the PCH encoding for S5");
    CHECK(board_sleep_word(lynxlp) == 0x3C00, "and so is the Lynx Point-LP's");
    CHECK(board_sleep_word(wildcat) == 0x3C00, "and the Wildcat Point-LP's");
    CHECK((board_sleep_word(piix4) & board_sleep_word(ich9) & board_sleep_word(lynx) &
           board_sleep_word(lynxlp) & board_sleep_word(wildcat) & 0x2000u) != 0,
          "every sleep word has SLP_EN");
    CHECK(((board_sleep_word(lynx) >> 10) & 0x7u) != 5 &&
              ((board_sleep_word(lynxlp) >> 10) & 0x7u) != 5 &&
              ((board_sleep_word(wildcat) >> 10) & 0x7u) != 5,
          "and none of the PCH words is sleep type 5, which is suspend, not off");
    CHECK(board_sleep_word(0) == 0, "no profile yields no word");

    CHECK(!board_pm_answers(0xFFFF), "an all-ones read is an unclaimed port, not a register");
    CHECK(board_pm_answers(0x0000), "a zero read is a register reporting zero");
    CHECK(board_pm_answers(0x0001), "and so is one with SCI_EN set");

    CHECK(board_needs_acpi_enable(lynx, 0x0000), "a PCH with SCI_EN clear still has to be asked");
    CHECK(!board_needs_acpi_enable(lynx, 0x0001), "and one with it set is already in ACPI mode");
    CHECK(!board_needs_acpi_enable(lynx, 0xFFFF),
          "a port that answers all ones is not asked, since nothing is there");
    CHECK(board_needs_acpi_enable(lynxlp, 0x0000), "the Lynx Point-LP is asked too");
    CHECK(!board_needs_acpi_enable(lynxlp, 0x0001), "unless it is already in ACPI mode");
    CHECK(board_needs_acpi_enable(wildcat, 0x0000), "and so is the Wildcat Point-LP");
    CHECK(!board_needs_acpi_enable(wildcat, 0x0001), "unless it is already in ACPI mode");
    CHECK(!board_needs_acpi_enable(piix4, 0x0000),
          "a profile with no SMI command is never asked, whatever SCI_EN says");
    CHECK(!board_needs_acpi_enable(0, 0x0000), "and no profile is never asked");
}

/**
 * Run the rows; the exit status is the count of failed checks, at most 255.
 *
 * The names are looked up first, since every other check needs the rows;
 * then each judgement runs over them; and last, before board_init runs, the
 * state must say that nothing may be written.
 */
int main(void)
{
    piix4 = board_lookup("piix4");
    ich9 = board_lookup("ich9");
    lynx = board_lookup("lynxpoint");
    lynxlp = board_lookup("lynxpoint-lp");
    wildcat = board_lookup("wildcatpoint-lp");

    CHECK(piix4 && ich9 && lynx && lynxlp && wildcat, "all five profiles are in the table");
    CHECK(board_lookup("nonesuch") == 0, "an unknown name is not a profile");
    CHECK(board_lookup("piix") == 0, "a prefix of a name is not that name");
    CHECK(board_lookup("piix44") == 0, "a name with something appended is not that name");
    CHECK(board_lookup(0) == 0, "no name at all is not a profile");
    if (!piix4 || !ich9 || !lynx || !lynxlp || !wildcat) {
        return fails > 255 ? 255 : fails;
    }

    check_identity();
    check_port_and_line();
    check_sleep_and_enable();

    CHECK(board_state() == BOARD_ABSENT, "nothing is identified until the probe runs");
    CHECK(board_pm1_cnt_port() == 0, "so there is no port to write");
    CHECK(board_current() == 0, "and no profile in force");
    CHECK(board_sci_line() == -1, "and no line masked");

    return fails > 255 ? 255 : fails;
}

#include "../../rach2/sys/arch/x86/board.c"
