/*
 * smbios_hosttest.c: where the SMBIOS reader finds the firmware's table, by
 * UEFI's configuration table or by the scan below a megabyte.
 *
 * host-deps: rach2/sys/arch/x86/smbios.c
 *
 * Physical memory is a few buffers at physical addresses, which the reader
 * reads through `smbios_set_memory`, so that a table may lie past 4 GiB as a
 * UEFI firmware's may. Into them go a UEFI system table, 64-bit or 32-bit,
 * whose configuration table names the SMBIOS 3 and the SMBIOS entry points
 * among another table's, each entry point's structures with a vendor of
 * their own, and an entry point in the region below a megabyte. The vendor
 * read says which table was found: the SMBIOS 3 one first, past 4 GiB; the
 * SMBIOS one when there is no other or the other's checksum fails; the
 * 32-bit system table's; and the scanned one when the loader passed no
 * system table or one whose signature is wrong.
 */

extern int printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);

#include "../../rach2/sys/arch/x86/smbios.h"
#include "../../rach2/sys/mm/paging.h"

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            printf("  FAIL: %s\n", (what)); \
            fails++;                        \
        }                                   \
    } while (0)

/** Fail the test: with memory given, every read goes through `mem`. */
uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    CHECK(0, "map_mmio is not asked when memory is given");
    return 0;
}

/* ---- physical memory ---- */

#define REGION_BYTES 0x10000u

static struct {
    uint64_t at; /* the physical address of mem[0] */
    uint8_t mem[REGION_BYTES];
} regions[5];

#define SCAN    0 /* 0xF0000, the region below a megabyte that is scanned */
#define SYSTAB  1 /* the system table, its configuration table, the entry points */
#define LOW     2 /* the SMBIOS entry point's structures */
#define HIGH    3 /* the SMBIOS 3 entry point's, past 4 GiB */
#define SCANNED 4 /* the scanned entry point's */

/** The reader's physical memory: the `len` bytes at `at`, or NULL when no
 * region holds them all. */
static const uint8_t *mem(uint64_t at, uint32_t len)
{
    int i;

    for (i = 0; i < 5; i++) {
        if (at >= regions[i].at && at + len <= regions[i].at + REGION_BYTES) {
            return regions[i].mem + (at - regions[i].at);
        }
    }
    return 0;
}

/** The byte at offset `off` of region `r`. */
static uint8_t *at_of(int r, uint32_t off)
{
    return regions[r].mem + off;
}

/** Store the low `n` bytes of `v` at `p`, little-endian. */
static void put(uint8_t *p, uint64_t v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/** Set byte `at` of the `len` bytes at `p` so that they sum to zero. */
static void sum_fix(uint8_t *p, int len, int at)
{
    uint8_t s = 0;
    int i;

    p[at] = 0;
    for (i = 0; i < len; i++) {
        s = (uint8_t)(s + p[i]);
    }
    p[at] = (uint8_t)(0x100u - s);
}

/** Write one BIOS Information structure naming `vendor`, then the end of
 * the table, at `p` in a region still zero; return their length. */
static uint32_t structures(uint8_t *p, const char *vendor)
{
    uint32_t n = 0x18, i;

    p[0] = 0;
    p[1] = 0x18;
    p[4] = 1;
    for (i = 0; vendor[i] != '\0'; i++) {
        p[n++] = (uint8_t)vendor[i];
    }
    p[n++] = 0;
    p[n++] = 0;
    p[n] = 127;
    p[n + 1] = 4;
    n += 6;
    return n;
}

/** Write at `p` an SMBIOS 2 entry point, `_SM_` with its `_DMI_` part, for
 * `len` bytes of structures at `table`, both checksums right. */
static void ep32(uint8_t *p, uint32_t table, uint32_t len)
{
    p[0] = '_', p[1] = 'S', p[2] = 'M', p[3] = '_';
    p[5] = 0x1F;
    p[0x10] = '_', p[0x11] = 'D', p[0x12] = 'M', p[0x13] = 'I', p[0x14] = '_';
    put(p + 0x16, len, 2);
    put(p + 0x18, table, 4);
    sum_fix(p + 0x10, 15, 5);
    sum_fix(p, 0x1F, 4);
}

/** Write at `p` an SMBIOS 3 entry point, `_SM3_`, for at most `len` bytes
 * of structures at `table`, its checksum right. */
static void ep64(uint8_t *p, uint64_t table, uint32_t len)
{
    p[0] = '_', p[1] = 'S', p[2] = 'M', p[3] = '3', p[4] = '_';
    p[6] = 0x18;
    put(p + 0x0C, len, 4);
    put(p + 0x10, table, 8);
    sum_fix(p, 0x18, 5);
}

static const uint8_t smbios3_guid[16] = {0x44, 0x15, 0xFD, 0xF2, 0x94, 0x97, 0x2C, 0x4A,
                                         0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94};
static const uint8_t smbios_guid[16] = {0x31, 0x2D, 0x9D, 0xEB, 0x88, 0x2D, 0xD3, 0x11,
                                        0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D};
static const uint8_t acpi_guid[16] = {0x71, 0xE8, 0x68, 0x88, 0xF1, 0xE4, 0xD3, 0x11,
                                      0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81};

#define SYSTAB_AT 0x7F000000ull
#define LOW_AT    0x7E000000ull
#define HIGH_AT   0x120000000ull

/**
 * Write the system table, 64-bit when `wide` is set and 32-bit otherwise,
 * with the first `n` of its three configuration entries: an ACPI one, the
 * SMBIOS one and the SMBIOS 3 one, whose entry points are at 0x2000 and
 * 0x2100 of its region.
 */
static void systab(int wide, int n)
{
    uint8_t *st = at_of(SYSTAB, 0), *cfg = at_of(SYSTAB, 0x1000);
    const uint8_t *guids[3];
    uint64_t eps[3];
    uint32_t size = wide ? 24 : 20, i, j;

    guids[0] = acpi_guid;
    guids[1] = smbios_guid;
    guids[2] = smbios3_guid;
    eps[0] = 0;
    eps[1] = SYSTAB_AT + 0x2000;
    eps[2] = SYSTAB_AT + 0x2100;
    for (i = 0; i < 0x1000; i++) {
        st[i] = 0;
        cfg[i] = 0;
    }
    put(st, 0x5453595320494249ull, 8);
    put(st + (wide ? 104 : 64), n, wide ? 8 : 4);
    put(st + (wide ? 112 : 68), SYSTAB_AT + 0x1000, wide ? 8 : 4);
    for (i = 0; i < (uint32_t)n; i++) {
        for (j = 0; j < 16; j++) {
            cfg[i * size + j] = guids[i][j];
        }
        put(cfg + i * size + 16, eps[i], wide ? 8 : 4);
    }
}

/** Whether the BIOS vendor the reader found is `want`. */
static int vendor_is(const char *want)
{
    return strcmp(smbios_bios_vendor(), want) == 0;
}

/** Run the rows: 0 when every check passes, 1 otherwise. */
int main(void)
{
    uint32_t low_len, high_len, scanned_len;

    printf("smbios\n");
    regions[SCAN].at = 0xF0000;
    regions[SYSTAB].at = SYSTAB_AT;
    regions[LOW].at = LOW_AT;
    regions[HIGH].at = HIGH_AT;
    regions[SCANNED].at = 0xE0000;
    low_len = structures(at_of(LOW, 0), "LENOVO");
    high_len = structures(at_of(HIGH, 0), "Insyde Corp.");
    scanned_len = structures(at_of(SCANNED, 0), "SeaBIOS");
    ep32(at_of(SYSTAB, 0x2000), (uint32_t)LOW_AT, low_len);
    ep64(at_of(SYSTAB, 0x2100), HIGH_AT, high_len);
    ep32(at_of(SCAN, 0x5A30), 0xE0000, scanned_len);
    smbios_set_memory(mem);

    systab(1, 3);
    smbios_init(SYSTAB_AT, 1);
    CHECK(vendor_is("Insyde Corp."), "UEFI's SMBIOS 3 entry point first, its table past 4 GiB");

    systab(1, 2);
    smbios_init(SYSTAB_AT, 1);
    CHECK(vendor_is("LENOVO"), "UEFI's SMBIOS entry point when there is no SMBIOS 3 one");

    systab(1, 3);
    at_of(SYSTAB, 0x2100)[0x10] ^= 1;
    smbios_init(SYSTAB_AT, 1);
    CHECK(vendor_is("LENOVO"), "an SMBIOS 3 entry point whose checksum fails gives way");
    ep64(at_of(SYSTAB, 0x2100), HIGH_AT, high_len);

    systab(0, 2);
    smbios_init(SYSTAB_AT, 0);
    CHECK(vendor_is("LENOVO"), "a 32-bit system table's entries of 20 bytes");

    systab(1, 3);
    at_of(SYSTAB, 0)[0] ^= 1;
    smbios_init(SYSTAB_AT, 1);
    CHECK(vendor_is("SeaBIOS"), "a system table whose signature is wrong gives way to the scan");

    smbios_init(0, 0);
    CHECK(vendor_is("SeaBIOS"), "with no system table the region below a megabyte is scanned");

    at_of(SCAN, 0x5A30)[0] = 0;
    smbios_init(0, 0);
    CHECK(vendor_is(""), "with no entry point anywhere the vendor is empty");

    if (fails) {
        printf("smbios_hosttest: %d check(s) failed\n", fails);
        return 1;
    }
    printf("smbios_hosttest: all checks passed\n");
    return 0;
}
