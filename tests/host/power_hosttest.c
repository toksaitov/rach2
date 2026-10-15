/*
 * power_hosttest.c: which ports a reset writes, and on whose firmware.
 *
 * host-deps: rach2/sys/arch/x86/smbios.c
 *
 * The reset sequence writes to three ports, and the first of them is a
 * command to a firmware's own system-management handler. That command belongs
 * to one firmware. Sent to a machine running another it reaches a handler
 * written for something else, and what that handler does is not this kernel's
 * to guess, so the decision is made from what the firmware says it is.
 *
 * A reset does not return, so the decision cannot be watched by running one.
 * What is watched here instead is the decision itself, given a table that says
 * Insyde and one that says something else, and the reader that gets the name
 * out of a firmware's table at all.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/arch/x86/smbios.h"
#include "../../rach2/sys/mm/paging.h"

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
 * The rule of `firmware_takes_smi_reset` in `power.c`: the BIOS vendor
 * begins with "Insyde". It is kept here rather than reached into, because
 * `power.c` pulls in the whole platform to compile and the rule under test
 * is this one comparison.
 */
static int takes_smi_reset(void)
{
    return smbios_vendor_is("Insyde");
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
    printf("power_hosttest: the reset's firmware check\n");

    present("Insyde Corp.");
    CHECK(smbios_vendor_is("Insyde"), "an Insyde firmware was not recognised");
    CHECK(takes_smi_reset(), "an Insyde firmware was not sent its own reset");

    present("SeaBIOS");
    CHECK(!smbios_vendor_is("Insyde"), "SeaBIOS was taken for Insyde");
    CHECK(!takes_smi_reset(), "a firmware that is not Insyde was sent Insyde's reset");

    present("Insyd");
    CHECK(!takes_smi_reset(), "a shorter name was taken for Insyde");

    smbios_set_table(table, 0);
    CHECK(smbios_bios_vendor()[0] == '\0', "an empty table named a vendor");
    CHECK(!takes_smi_reset(), "a machine that names no firmware was sent Insyde's reset");

    {
        static const char *const version_first[1] = {"1.42"};

        present_at("Insyde Corp.", 2, version_first, 1);
        CHECK(takes_smi_reset(), "a vendor that is the second string was not read");
    }

    {
        static const char *const insyde_first[1] = {"Insyde Corp."};

        present_at("Phoenix", 2, insyde_first, 1);
        CHECK(!takes_smi_reset(), "a string that is not the vendor was taken for one");
    }

    present_at("Insyde Corp.", 0, 0, 0);
    CHECK(smbios_bios_vendor()[0] == '\0', "a vendor field of zero named a vendor");
    CHECK(!takes_smi_reset(), "and a machine that names none was sent Insyde's reset");

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
    CHECK(!takes_smi_reset(), "a structure longer than its table was walked anyway");

    present("Insyde");
    smbios_set_table(table, 0x18 + 6);
    CHECK(smbios_bios_vendor()[0] == '\0', "an unterminated string was published as a vendor");
    CHECK(!takes_smi_reset(), "and it authorised the firmware's own reset");

    if (fails == 0) {
        printf("power_hosttest: OK\n");
        return 0;
    }
    printf("power_hosttest: %d failure(s)\n", fails);
    return 1;
}
