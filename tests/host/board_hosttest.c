/*
 * board_hosttest.c: which power controller may be written to, and with what.
 *
 * host-deps:
 *
 * board.c is included, not linked, so the profile table's own rows can
 * be reached by name and judged. That matters more here than usual: of the
 * three chipsets in that table, two exist on this desk only as QEMU models and
 * the third is a laptop nobody has booted yet, so the row that will decide
 * whether that machine powers off has never been executed.
 *
 * What is being guarded is a 16-bit write to an I/O port derived from a
 * chipset register. Derived wrongly it lands on the DMA controller, the
 * interrupt controller, the timer, the keyboard controller, the CMOS index and
 * data pair, or the reset register, so most of this file is about refusing.
 */

extern int printf(const char *fmt, ...);

/* The stateful half of board.c reaches PCI, the PIC, the command line and the
   serial log. None of it is exercised here; the judgements are pure. */
#include "../../rach2/sys/board.h"

/* The driver reports what it found through the kernel log, which this test
   has no use for; the records are dropped. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

unsigned short pci_read_config16(unsigned char b, unsigned char d, unsigned char f, unsigned char o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0xFFFF;
}

unsigned char pci_read_config8(unsigned char b, unsigned char d, unsigned char f, unsigned char o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0xFF;
}

unsigned int pci_read_config32(unsigned char b, unsigned char d, unsigned char f, unsigned char o)
{
    (void)b;
    (void)d;
    (void)f;
    (void)o;
    return 0xFFFFFFFFu;
}

void pic_mask_irq(unsigned char irq)
{
    (void)irq;
}

int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    (void)key;
    (void)out;
    (void)outlen;
    return 0;
}

void k_printf(const char *fmt, ...)
{
    (void)fmt;
}

/* The port I/O board.c's enable handshake performs. Nothing here reaches it:
   the judgements are pure and board_init is never called. */
unsigned short inw(unsigned short port)
{
    (void)port;
    return 0xFFFF;
}

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

int main(void)
{
    const board_profile_t *piix4 = board_lookup("piix4");
    const board_profile_t *ich9 = board_lookup("ich9");
    const board_profile_t *lynx = board_lookup("lynxpoint");

    CHECK(piix4 && ich9 && lynx, "all three profiles are in the table");
    CHECK(board_lookup("nonesuch") == 0, "an unknown name is not a profile");
    CHECK(board_lookup("piix") == 0, "a prefix of a name is not that name");
    CHECK(board_lookup("piix44") == 0, "a name with something appended is not that name");
    CHECK(board_lookup(0) == 0, "no name at all is not a profile");
    if (!piix4 || !ich9 || !lynx) {
        return fails > 255 ? 255 : fails;
    }

    /* ---- identity: the chipset must be the one the row describes ---- */
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

    /* The laptop's range is a range, and its edges are the interesting part. */
    CHECK(board_id_matches(lynx, 0x8086, 0x8C49, 0x06, 0x01),
          "the expected Lynx Point LPC bridge matches");
    CHECK(board_id_matches(lynx, 0x8086, 0x8C40, 0x06, 0x01), "the bottom of the range matches");
    CHECK(board_id_matches(lynx, 0x8086, 0x8C5F, 0x06, 0x01), "the top of the range matches");
    CHECK(!board_id_matches(lynx, 0x8086, 0x8C3F, 0x06, 0x01), "one below the range does not");
    CHECK(!board_id_matches(lynx, 0x8086, 0x8C60, 0x06, 0x01), "one above the range does not");
    CHECK(!board_id_matches(0, 0x8086, 0x8C49, 0x06, 0x01), "no profile matches nothing");

    /* ---- the base, which is the number a mistake turns into a wrong port
     * ---- */
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
    CHECK(board_pm_base(0, 0x1801u) == 0, "no profile yields no base");

    /* ---- the decode bit, which says whether the block answers at all ---- */
    CHECK(board_pm_decoding(piix4, 0x01), "the PIIX4 decode bit is bit 0");
    CHECK(!board_pm_decoding(piix4, 0xFE), "and everything but bit 0 does not enable it");
    CHECK(board_pm_decoding(lynx, 0x80), "the PCH decode bit is bit 7");
    CHECK(!board_pm_decoding(lynx, 0x7F), "and the low bits do not stand in for it");
    CHECK(!board_pm_decoding(0, 0xFF), "no profile decodes nothing");

    /* ---- the SCI line, which is masked and must never be the wrong one
     * ---- */
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
    CHECK(board_sci_irq(0, 0x00) == -1, "no profile has no line");

    /* ---- the sleep word, the one value that is believed, not read
     * ---- */
    CHECK(board_sleep_word(piix4) == 0x2000,
          "the PIIX4 word is sleep type 0 with SLP_EN, which is what QEMU takes as off");
    CHECK(board_sleep_word(ich9) == 0x2000, "and so is the ICH9's");
    CHECK(board_sleep_word(lynx) == 0x3C00,
          "the laptop's word is sleep type 7 with SLP_EN, the PCH encoding for S5");
    CHECK((board_sleep_word(lynx) & 0x2000u) != 0, "every sleep word has SLP_EN");
    CHECK(((board_sleep_word(lynx) >> 10) & 0x7u) != 5,
          "and none of them is sleep type 5, which is suspend, not off");
    CHECK(board_sleep_word(0) == 0, "no profile yields no word");

    /* ---- whether anything answered, and whether ACPI must be asked for
     * ---- */
    CHECK(!board_pm_answers(0xFFFF), "an all-ones read is an unclaimed port, not a register");
    CHECK(board_pm_answers(0x0000), "a zero read is a register reporting zero");
    CHECK(board_pm_answers(0x0001), "and so is one with SCI_EN set");

    CHECK(board_needs_acpi_enable(lynx, 0x0000), "a PCH with SCI_EN clear still has to be asked");
    CHECK(!board_needs_acpi_enable(lynx, 0x0001), "and one with it set is already in ACPI mode");
    CHECK(!board_needs_acpi_enable(lynx, 0xFFFF),
          "a port that answers all ones is not asked, since nothing is there");
    CHECK(!board_needs_acpi_enable(piix4, 0x0000),
          "a profile with no SMI command is never asked, whatever SCI_EN says");
    CHECK(!board_needs_acpi_enable(0, 0x0000), "and no profile is never asked");

    /* ---- before board_init runs, nothing may be written ---- */
    CHECK(board_state() == BOARD_ABSENT, "nothing is identified until the probe runs");
    CHECK(board_pm1_cnt_port() == 0, "so there is no port to write");
    CHECK(board_current() == 0, "and no profile in force");
    CHECK(board_sci_line() == -1, "and no line masked");

    return fails > 255 ? 255 : fails;
}

#include "../../rach2/sys/board.c"
