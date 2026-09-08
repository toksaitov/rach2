/*
 * power_hosttest.c: which ports a reset writes, and on whose firmware.
 *
 * host-deps: rach2/sys/smbios.c
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

#include "../../rach2/sys/smbios.h"

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

/** Build the structures whose BIOS vendor is `vendor`, and present them. */
static void present(const char *vendor)
{
    unsigned char *p = table;
    int i;
    unsigned n;

    for (i = 0; i < (int)sizeof(table); i++) {
        table[i] = 0;
    }

    /* One BIOS Information structure: type 0, an 0x18-byte formatted area,
     * a handle, then its strings. Its first string is the vendor. */
    p[0] = 0;
    p[1] = 0x18;
    p[2] = 0;
    p[3] = 0;
    p[4] = 1; /* the vendor is string 1 */
    n = 0x18;
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

/*
 * The decision, as `power.c` makes it. Kept here rather than reached into,
 * because `power.c` pulls in the whole platform to compile and the rule under
 * test is this one comparison.
 */
static int takes_smi_reset(void)
{
    return smbios_vendor_is("Insyde");
}

int main(void)
{
    printf("power_hosttest: the reset's firmware check\n");

    /* A machine whose firmware is the one the command belongs to. */
    present("Insyde Corp.");
    CHECK(smbios_vendor_is("Insyde"), "an Insyde firmware was not recognised");
    CHECK(takes_smi_reset(), "an Insyde firmware was not sent its own reset");

    /* A machine whose firmware is another. QEMU's is SeaBIOS, which is what
     * every suite boots, so this is the case that runs everywhere. */
    present("SeaBIOS");
    CHECK(!smbios_vendor_is("Insyde"), "SeaBIOS was taken for Insyde");
    CHECK(!takes_smi_reset(), "a firmware that is not Insyde was sent Insyde's reset");

    /* A name that begins the same way and is not the same firmware. */
    present("Insyd");
    CHECK(!takes_smi_reset(), "a shorter name was taken for Insyde");

    /* A machine whose table is empty: the command is not sent, since nothing
     * says it belongs there. The scan for an anchor is not run here; on this
     * host the region it reads is not this program's to read. */
    smbios_set_table(table, 0);
    CHECK(smbios_bios_vendor()[0] == '\0', "an empty table named a vendor");
    CHECK(!takes_smi_reset(), "a machine that names no firmware was sent Insyde's reset");

    /* The vendor is read once and kept, so a second ask does not scan again
     * and does not give a different answer. */
    present("Insyde Corp.");
    {
        const char *first = smbios_bios_vendor();
        const char *again = smbios_bios_vendor();

        CHECK(first == again, "the vendor was read twice into two places");
    }

    /* A table whose structure claims a length past its end is refused rather
     * than walked off the end of. */
    present("Insyde Corp.");
    table[1] = 0xFF;
    smbios_set_table(table, table_len);
    CHECK(!takes_smi_reset(), "a structure longer than its table was walked anyway");

    if (fails == 0) {
        printf("power_hosttest: OK\n");
        return 0;
    }
    printf("power_hosttest: %d failure(s)\n", fails);
    return 1;
}
