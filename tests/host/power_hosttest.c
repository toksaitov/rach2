/*
 * power_hosttest.c: which ports a reset and a power off write, and on whose
 * firmware and chipset.
 *
 * host-deps: rach2/sys/arch/x86/smbios.c rach2/sys/arch/x86/board.c
 *
 * power.c is included whole, so that the reset and the power off, which are
 * static, run here against models of the I/O ports, of PCI configuration
 * space and of the ACPI tables; RACH2_HOSTTEST leaves out `power_stop` and
 * its halt, whose `cli` and `hlt` a host can neither assemble everywhere nor
 * run. Neither request returns on a machine, so what is watched is the
 * sequence of port writes each makes.
 *
 * The reset's first fixed mechanism is a command to a firmware's own
 * system-management handler. That command belongs to one firmware. Sent to a
 * machine running another it reaches a handler written for something else,
 * and what that handler does is not this kernel's to guess, so the decision
 * is made from what the firmware says it is, read here from SMBIOS tables
 * built byte by byte.
 *
 * The power off writes a sleep word to a port derived from a chipset
 * register, and only to a port that `board_init` in board.c accepted: a
 * PIIX4 it refused gets no write.
 */

extern int printf(const char *fmt, ...);

#include "arch/x86/power.c"
#include "mm/paging.h"
#include "pci.h"

/** Report a failure: `smbios.c` maps a table above the identity-mapped
 * window, and the tables here are given to the reader directly. */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    (void)phys_addr;
    (void)size;
    printf("  FAIL: a table inside the window was mapped anyway\n");
    return 0;
}

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            printf("  FAIL: %s\n", (what)); \
            fails++;                        \
        }                                   \
    } while (0)

/*
 * A firmware's structures, built here byte by byte: one BIOS Information
 * structure with its vendor string, then the end-of-table marker. The anchor
 * that would point at them on a machine is not built, because its address
 * field is thirty-two bits and this program's memory is not there; the reader
 * is given the structures directly instead.
 */
static unsigned char table[512];
static unsigned int table_len;

/**
 * Build the structures whose BIOS vendor is `vendor`, and present them.
 *
 * `index` is the number the structure's vendor field carries, and `before` is
 * the text of the strings that come before it, one per string, so that a
 * vendor which is not the first string can be presented. `index` of 0 is a
 * structure that names no vendor at all.
 */
static void present_at(const char *vendor, int index, const char *const *before, int nbefore)
{
    unsigned char *p = table;
    int i, j;
    unsigned n;

    for (i = 0; i < (int)sizeof(table); i++) {
        table[i] = 0;
    }

    /* Type 0, BIOS Information: 0x18 formatted bytes, then its strings. */
    p[0] = 0;
    p[1] = 0x18;
    p[2] = 0;
    p[3] = 0;
    p[4] = (unsigned char)index;
    n = 0x18;
    for (j = 0; j < nbefore; j++) {
        for (i = 0; before[j][i] != 0; i++) {
            p[n++] = (unsigned char)before[j][i];
        }
        p[n++] = 0;
    }
    for (i = 0; vendor[i] != 0; i++) {
        p[n++] = (unsigned char)vendor[i];
    }
    p[n++] = 0; /* end of the vendor string */
    p[n++] = 0; /* end of this structure's strings */
    /* The end-of-table structure, so a reader that walked past would stop. */
    p[n++] = 127;
    p[n++] = 4;
    p[n++] = 1;
    p[n++] = 0;
    p[n++] = 0;
    p[n++] = 0;

    table_len = n;
    smbios_set_table(table, table_len);
}

/** Build the structures whose BIOS vendor is `vendor`, the first string. */
static void present(const char *vendor)
{
    present_at(vendor, 1, 0, 0);
}

/*
 * The model ports. Every write is recorded in order, with its width; a read
 * of the keyboard controller's status says its input buffer is empty, and a
 * read of the PIIX4's PM1 control register gives `model_pm1`.
 */
#define MAX_WRITES 16
#define GAS_WRITE  0 /* the width of a write through `acpi_gas_write` */

static struct {
    uint16_t port;
    uint32_t value;
    int width;
} writes[MAX_WRITES];
static int nwrites;

static uint16_t model_pm1;

/* The PIIX4 at 0:1.3: whether it is there, its PMREGMISC and its PMBA. */
static int model_piix4;
static uint8_t model_misc;
static uint32_t model_pmba;

/* The value of `rach2.resetskip` on the command line, or NULL for none. */
static const char *model_resetskip;

/* PM1_CNT's SLP_EN, with sleep type 0: QEMU's PIIX4 takes it as soft off. */
#define PM1A_CNT_SLP_EN_PIIX4 0x2000

/* The ACPI tables: a reset register where a row gives one, and nothing else. */
static struct acpi_info model_acpi;
static const struct acpi_info no_acpi;

static void record(uint16_t port, uint32_t value, int width)
{
    if (nwrites < MAX_WRITES) {
        writes[nwrites].port = port;
        writes[nwrites].value = value;
        writes[nwrites].width = width;
    }
    nwrites++;
}

void outb(uint16_t port, uint8_t val)
{
    record(port, val, 1);
}

void outw(uint16_t port, uint16_t val)
{
    record(port, val, 2);
}

uint8_t inb(uint16_t port)
{
    return port == KBC_STATUS_PORT ? 0 : 0xFF;
}

uint16_t inw(uint16_t port)
{
    if (model_piix4 && (model_misc & 1) && port == (uint16_t)((model_pmba & 0xFFC0u) + 4)) {
        return model_pm1;
    }
    return 0xFFFF;
}

/** Discard the kernel log: the test reads the writes. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** A configuration read of the model PIIX4, or of no device: all ones. */
uint32_t pci_read_config32(uint8_t b, uint8_t d, uint8_t f, uint8_t o)
{
    if (model_piix4 && b == 0 && d == 1 && f == 3) {
        if (o == 0x40) {
            return model_pmba;
        }
        if (o == PCI_VENDOR_ID) {
            return 0x71138086u;
        }
        return 0;
    }
    return 0xFFFFFFFFu;
}

uint16_t pci_read_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t o)
{
    return (uint16_t)(pci_read_config32(b, d, f, (uint8_t)(o & ~3u)) >> ((o & 2u) * 8));
}

uint8_t pci_read_config8(uint8_t b, uint8_t d, uint8_t f, uint8_t o)
{
    if (model_piix4 && b == 0 && d == 1 && f == 3 && o == 0x80) {
        return model_misc;
    }
    return (uint8_t)(pci_read_config32(b, d, f, (uint8_t)(o & ~3u)) >> ((o & 3u) * 8));
}

/** Mask nothing: the SCI is never enabled here. */
void pic_mask_irq(uint8_t irq)
{
    (void)irq;
}

int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

/** The command line has `rach2.resetskip` when the row gives it. */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    const char *want = "rach2.resetskip";
    unsigned int i;

    for (i = 0; want[i] != '\0' && key[i] == want[i]; i++) {
    }
    if (!model_resetskip || want[i] != '\0' || key[i] != '\0') {
        return 0;
    }
    for (i = 0; i + 1 < outlen && model_resetskip[i] != '\0'; i++) {
        out[i] = model_resetskip[i];
    }
    out[i] = '\0';
    return 1;
}

const struct acpi_info *acpi_get(void)
{
    return &model_acpi;
}

int acpi_gas_read(const struct acpi_gas *g, uint32_t *value)
{
    (void)g;
    (void)value;
    return -1;
}

int acpi_gas_write(const struct acpi_gas *g, uint32_t value)
{
    record((uint16_t)g->address, value, GAS_WRITE);
    return 0;
}

/** Whether write `i` was `value` to `port`, `width` wide. */
static int wrote(int i, uint16_t port, uint32_t value, int width)
{
    return i < nwrites && i < MAX_WRITES && writes[i].port == port && writes[i].value == value &&
           writes[i].width == width;
}

/** Whether any write went to `port`. */
static int wrote_to(uint16_t port)
{
    int i;

    for (i = 0; i < nwrites && i < MAX_WRITES; i++) {
        if (writes[i].port == port) {
            return 1;
        }
    }
    return 0;
}

/** Restart with `vendor` in the SMBIOS tables, `skip` on the command line,
 * and the FADT's reset register at I/O port `reset_port`, or none for 0. */
static void restart(const char *vendor, const char *skip, uint16_t reset_port)
{
    present(vendor);
    model_resetskip = skip;
    model_acpi = no_acpi;
    if (reset_port != 0) {
        model_acpi.reset_supported = 1;
        model_acpi.reset_reg.space = ACPI_SPACE_IO;
        model_acpi.reset_reg.address = reset_port;
        model_acpi.reset_value = 0x0E;
    }
    nwrites = 0;
    power_reset();
}

/** Run the boot's probe of a PIIX4 whose PMREGMISC is `misc`, PMBA `pmba`
 * and PM1 control register `pm1`, or of none for `present` 0, then power
 * off through the board profile. */
static void power_off(int present, uint8_t misc, uint32_t pmba, uint16_t pm1)
{
    const char *why;

    model_piix4 = present;
    model_misc = misc;
    model_pmba = pmba;
    model_pm1 = pm1;
    (void)board_init(&why);
    nwrites = 0;
    power_board_off();
}

/**
 * The reset writes its mechanisms in order: the FADT's reset register where
 * there is one, Insyde's command on Insyde alone, the chipset's two writes
 * to 0xCF9, and the keyboard controller's pulse, which `rach2.resetskip`
 * never leaves out; a skip counts the FADT's register only where it exists.
 */
static void reset_rows(void)
{
    restart("Insyde Corp.", 0, 0);
    CHECK(nwrites == 4 && wrote(0, APM_CONTROL_PORT, APM_CMD_RESET, 1) &&
              wrote(1, RESET_CONTROL_PORT, RESET_CONTROL_ARM, 1) &&
              wrote(2, RESET_CONTROL_PORT, RESET_CONTROL_FIRE, 1) &&
              wrote(3, KBC_COMMAND_PORT, KBC_PULSE_RESET, 1),
          "an Insyde reset is not its command, the chipset's two writes and the pulse");

    restart("SeaBIOS", 0, 0);
    CHECK(nwrites == 3 && !wrote_to(APM_CONTROL_PORT),
          "a SeaBIOS reset sent Insyde's command or lost a mechanism");

    restart("", 0, 0);
    CHECK(nwrites == 3 && !wrote_to(APM_CONTROL_PORT),
          "a reset with no vendor sent Insyde's command or lost a mechanism");

    restart("Insyde Corp.", 0, 0xCF9);
    CHECK(nwrites == 5 && wrote(0, 0xCF9, 0x0E, 1) && wrote(1, APM_CONTROL_PORT, APM_CMD_RESET, 1),
          "the FADT's reset register was not written first");

    restart("Insyde Corp.", "1", 0xCF9);
    CHECK(nwrites == 4 && wrote(0, APM_CONTROL_PORT, APM_CMD_RESET, 1),
          "rach2.resetskip=1 did not leave out the FADT's register alone");

    restart("Insyde Corp.", "1", 0);
    CHECK(nwrites == 3 && wrote(0, RESET_CONTROL_PORT, RESET_CONTROL_ARM, 1),
          "rach2.resetskip=1 without a FADT register did not leave out Insyde's command");

    restart("Insyde Corp.", "3", 0xCF9);
    CHECK(nwrites == 1 && wrote(0, KBC_COMMAND_PORT, KBC_PULSE_RESET, 1),
          "rach2.resetskip=3 left out the pulse or kept another mechanism");

    restart("SeaBIOS", "12", 0xCF9);
    CHECK(nwrites == 4 && wrote(0, 0xCF9, 0x0E, 1),
          "a resetskip of two digits was taken as a skip");
}

/**
 * The power off writes the PIIX4's sleep word, sleep type 0 with SLP_EN, to
 * PM1_CNT four bytes past the base, and nothing to a PIIX4 that is not
 * decoding, whose base was never programmed, or whose PM1_CNT reads all
 * ones, nor to a machine with no PIIX4.
 */
static void power_off_rows(void)
{
    power_off(1, 0x01, 0x0601, 0x0000);
    CHECK(nwrites == 1 && wrote(0, 0x0604, PM1A_CNT_SLP_EN_PIIX4, 2),
          "a PIIX4 that answers was not sent its sleep word");

    power_off(1, 0x01, 0x0601, 0xFFFF);
    CHECK(nwrites == 0, "a PIIX4 whose PM1_CNT reads all ones was written");

    power_off(1, 0x00, 0x0601, 0x0000);
    CHECK(nwrites == 0, "a PIIX4 whose PM block is not decoding was written");

    power_off(1, 0x01, 0x0001, 0x0000);
    CHECK(nwrites == 0, "a PIIX4 whose base was never programmed was written");

    power_off(0, 0x01, 0x0601, 0x0000);
    CHECK(nwrites == 0, "a machine with no PIIX4 was written");
}

/**
 * Run the rows: 0 when every check passes, 1 otherwise.
 *
 * The firmware the command belongs to takes it. Another does not: QEMU's is
 * SeaBIOS, which every suite boots, so that row runs everywhere. A name that
 * begins the same way and is shorter is another firmware. An empty table
 * names no firmware, so the command is not sent; the scan for an anchor is
 * not run, since on this host the region it reads is not this program's.
 *
 * The vendor field says which string is the vendor, and it is not always
 * the first: a firmware that puts its version first would lose its own
 * reset if the reader took string one, and the reverse must not be read as
 * though the first string were the vendor. A field of zero names none.
 *
 * The vendor is read once and kept: the bytes behind it are changed without
 * presenting them, which a reader that scanned every time would pick up. A
 * structure that claims a length past the table's end is not walked. A
 * string that does not end inside the table is not a string: cutting the
 * table at the last byte of "Insyde" leaves it with no terminator, and a
 * reader that supplied one would publish a vendor the firmware never wrote.
 */
int main(void)
{
    printf("power_hosttest: the reset's firmware check and the power off's port\n");

    present("Insyde Corp.");
    CHECK(smbios_vendor_is("Insyde"), "an Insyde firmware was not recognised");
    CHECK(firmware_takes_smi_reset(), "an Insyde firmware was not sent its own reset");

    present("SeaBIOS");
    CHECK(!smbios_vendor_is("Insyde"), "SeaBIOS was taken for Insyde");
    CHECK(!firmware_takes_smi_reset(), "a firmware that is not Insyde was sent Insyde's reset");

    present("Insyd");
    CHECK(!firmware_takes_smi_reset(), "a shorter name was taken for Insyde");

    smbios_set_table(table, 0);
    CHECK(smbios_bios_vendor()[0] == '\0', "an empty table named a vendor");
    CHECK(!firmware_takes_smi_reset(), "a machine that names no firmware was sent Insyde's reset");

    {
        static const char *const version_first[1] = {"1.42"};

        present_at("Insyde Corp.", 2, version_first, 1);
        CHECK(firmware_takes_smi_reset(), "a vendor that is the second string was not read");
    }

    {
        static const char *const insyde_first[1] = {"Insyde Corp."};

        present_at("Phoenix", 2, insyde_first, 1);
        CHECK(!firmware_takes_smi_reset(), "a string that is not the vendor was taken for one");
    }

    present_at("Insyde Corp.", 0, 0, 0);
    CHECK(smbios_bios_vendor()[0] == '\0', "a vendor field of zero named a vendor");
    CHECK(!firmware_takes_smi_reset(), "and a machine that names none was sent Insyde's reset");

    present("Insyde Corp.");
    {
        const char *first = smbios_bios_vendor();

        table[0x18] = 'X';
        CHECK(smbios_bios_vendor()[0] == 'I' && first[0] == 'I',
              "the vendor was read again instead of kept");
    }

    present("Insyde Corp.");
    table[1] = 0xFF;
    smbios_set_table(table, table_len);
    CHECK(!firmware_takes_smi_reset(), "a structure longer than its table was walked anyway");

    present("Insyde");
    smbios_set_table(table, 0x18 + 6);
    CHECK(smbios_bios_vendor()[0] == '\0', "an unterminated string was published as a vendor");
    CHECK(!firmware_takes_smi_reset(), "and it authorised the firmware's own reset");

    reset_rows();
    power_off_rows();

    if (fails == 0) {
        printf("power_hosttest: OK\n");
        return 0;
    }
    printf("power_hosttest: %d failure(s)\n", fails);
    return 1;
}
