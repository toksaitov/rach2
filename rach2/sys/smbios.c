/*
 * smbios.c: find the firmware's table and read the BIOS vendor out of it.
 *
 * The anchor is "_SM_" on a sixteen-byte boundary between 0xF0000 and
 * 0xFFFFF, with a checksum over its own length; the newer "_SM3_" anchor is
 * read too, since a machine that publishes only that one is otherwise a
 * machine with no table. The anchor gives the address and length of the
 * structure table, which is a run of structures each with a four-byte header
 * followed by its formatted area and then its strings, one after another,
 * ending with an empty one.
 *
 * Only the first structure of type 0, BIOS Information, is wanted, and out of
 * it the string its vendor field names. Everything else is stepped over.
 *
 * The anchor is in the read-only region below a megabyte, which is inside the
 * identity-mapped window. The structures it points at need not be: a machine
 * with more than 32 MiB usually publishes them near the top of its memory, so
 * a table above the window is mapped before it is read.
 *
 * A firmware's table is data this kernel did not write, and every field read
 * out of it is bounded by the length the anchor gave. A structure that claims
 * more than the table holds, or a string that does not end inside it, is a
 * table this does not read rather than a read that runs off the end.
 */

#include "smbios.h"

#include "lib/kint.h"
#include "mm/paging.h"

#define SMBIOS_SCAN_START 0xF0000u
#define SMBIOS_SCAN_END   0x100000u
#define SMBIOS_SCAN_STEP  16u

/* The longest vendor string this keeps. The names are short; a longer one is
 * cut, which is enough to tell one firmware from another. */
#define SMBIOS_VENDOR_MAX 64

/* The entry points, as the specification fixes them: the 32-bit one is 31
 * bytes and carries the table's address at 0x18, and the 64-bit one is 24 and
 * carries it at 0x10. A candidate shorter than its own entry point is not one,
 * whatever its checksum says, because the address would be read from bytes the
 * checksum never covered. */
#define SMBIOS_EP32_LEN 0x1F
#define SMBIOS_EP64_LEN 0x18

static char vendor[SMBIOS_VENDOR_MAX];
static int vendor_read;
static const uint8_t *forced_table;
static uint32_t forced_len;

/** The sum of `len` bytes at `p`, which a valid anchor makes zero. */
static uint8_t sum_of(const uint8_t *p, uint32_t len)
{
    uint8_t s = 0;
    uint32_t i;

    for (i = 0; i < len; i++) {
        s = (uint8_t)(s + p[i]);
    }
    return s;
}

/** Whether `p` holds the four bytes of `s`. */
static int four_are(const uint8_t *p, const char *s)
{
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] && p[2] == (uint8_t)s[2] &&
           p[3] == (uint8_t)s[3];
}

/**
 * The anchor, or NULL.
 *
 * A test may present one instead of the region being scanned, which is the
 * only way to put a firmware's table in front of this code on a machine whose
 * firmware is not that one.
 */
static const uint8_t *find_anchor(void)
{
    uint32_t at;

    for (at = SMBIOS_SCAN_START; at + 32u <= SMBIOS_SCAN_END; at += SMBIOS_SCAN_STEP) {
        const uint8_t *p = (const uint8_t *)at;

        if (four_are(p, "_SM_") && p[5] >= SMBIOS_EP32_LEN && sum_of(p, p[5]) == 0 &&
            four_are(p + 0x10, "_DMI") && p[0x14] == '_' && sum_of(p + 0x10, 15) == 0) {
            /* The 32-bit entry point has a second anchor inside it, with a
             * checksum of its own over the fifteen bytes that hold the table's
             * address and length. Both are required: a candidate whose length
             * field is short passes the outer checksum over bytes that do not
             * include the address. */
            return p;
        }
        if (four_are(p, "_SM3") && p[4] == '_' && p[6] >= SMBIOS_EP64_LEN && sum_of(p, p[6]) == 0) {
            return p;
        }
    }
    return 0;
}

/**
 * Where the structure table is and how long it is, from an anchor.
 *
 * @return 1 with `*at` and `*len` filled, 0 when the anchor is one this does
 *         not read or names a region that does not fit an address.
 */
static int table_of(const uint8_t *anchor, uint32_t *at, uint32_t *len)
{
    if (four_are(anchor, "_SM_")) {
        /* The 32-bit entry point: the length is two bytes at 0x16 and the
         * address four at 0x18. */
        *len = (uint32_t)anchor[0x16] | ((uint32_t)anchor[0x17] << 8);
        *at = (uint32_t)anchor[0x18] | ((uint32_t)anchor[0x19] << 8) |
              ((uint32_t)anchor[0x1A] << 16) | ((uint32_t)anchor[0x1B] << 24);
    } else if (four_are(anchor, "_SM3") && anchor[4] == '_') {
        /* The 64-bit entry point: the maximum length is four bytes at 0x0C
         * and the address eight at 0x10, whose upper half must be zero on a
         * machine this kernel addresses. */
        uint32_t hi = (uint32_t)anchor[0x14] | ((uint32_t)anchor[0x15] << 8) |
                      ((uint32_t)anchor[0x16] << 16) | ((uint32_t)anchor[0x17] << 24);

        if (hi != 0) {
            return 0;
        }
        *len = (uint32_t)anchor[0x0C] | ((uint32_t)anchor[0x0D] << 8) |
               ((uint32_t)anchor[0x0E] << 16) | ((uint32_t)anchor[0x0F] << 24);
        *at = (uint32_t)anchor[0x10] | ((uint32_t)anchor[0x11] << 8) |
              ((uint32_t)anchor[0x12] << 16) | ((uint32_t)anchor[0x13] << 24);
    } else {
        return 0;
    }
    /* A region of no length holds no structure, and one that wraps the address
     * space is not a region. */
    if (*len == 0 || *at + *len < *at) {
        return 0;
    }
    return 1;
}

/**
 * An address this kernel can read the `len` bytes at physical `at` through.
 *
 * A table inside the identity-mapped window is read where it is. One above it
 * is mapped first: on a machine with more than 32 MiB the firmware usually
 * publishes its table near the top of memory, and reading that through its
 * physical address would fault. The mapping lasts for the life of the kernel,
 * which costs a few pages once, since the vendor is read once.
 *
 * @return the address, or 0 when the table cannot be reached.
 */
static const uint8_t *table_at(uint32_t at, uint32_t len)
{
    uintptr_t mapped;

    if (at + len <= (uint32_t)HARD_MEMORY_LIMIT) {
        return (const uint8_t *)at;
    }
    mapped = map_mmio((uintptr_t)at, (size_t)len);
    return (const uint8_t *)mapped;
}

/**
 * Copy the `want`th string of a structure whose strings begin at `s`.
 *
 * The strings are NUL-terminated one after another and the run ends with an
 * empty one; `want` counts from one, as the structure's fields name them.
 * `end` bounds the read. A string that does not end before it is not copied:
 * the bytes are a firmware's, the length is a firmware's, and a table cut
 * short would otherwise yield whatever ran up to the cut as a vendor's name.
 */
static void copy_string(const uint8_t *s, const uint8_t *end, int want, char *out, int outlen)
{
    int n = 1;

    out[0] = '\0';
    if (want < 1) {
        return;
    }
    while (s < end && *s != 0) {
        const uint8_t *stop = s;

        while (stop < end && *stop != 0) {
            stop++;
        }
        if (stop >= end) {
            return; /* no terminator inside the table */
        }
        if (n == want) {
            int i = 0;

            while (s < stop && i < outlen - 1) {
                out[i++] = (char)*s++;
            }
            out[i] = '\0';
            return;
        }
        s = stop + 1;
        n++;
    }
}

/** Read the vendor out of the table, leaving `vendor` empty when there is
 * none. */
static void read_vendor(void)
{
    const uint8_t *anchor;
    const uint8_t *p;
    uint32_t at = 0;
    uint32_t len = 0;
    const uint8_t *end;

    vendor[0] = '\0';
    vendor_read = 1;

    if (forced_table) {
        p = forced_table;
        len = forced_len;
    } else {
        anchor = find_anchor();
        if (!anchor || !table_of(anchor, &at, &len)) {
            return;
        }
        p = table_at(at, len);
        if (!p) {
            return;
        }
    }
    if (len == 0) {
        return;
    }
    end = p + len;
    while (p + 4 <= end) {
        uint8_t type = p[0];
        uint8_t flen = p[1];
        const uint8_t *strings;

        if (flen < 4 || p + flen > end) {
            return; /* a header that does not fit is a table this cannot read */
        }
        strings = p + flen;
        if (type == 0) {
            /* BIOS Information: the byte at offset 4 is the number of the
             * string that is the vendor, and 0 there means the structure names
             * none. It is not always the first string. */
            if (flen >= 5 && p[4] != 0) {
                copy_string(strings, end, (int)p[4], vendor, SMBIOS_VENDOR_MAX);
            }
            return;
        }
        if (type == 127) {
            return; /* the end-of-table structure */
        }
        /* Past the strings, which end with an empty one. */
        p = strings;
        while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) {
            p++;
        }
        p += 2;
    }
}

const char *smbios_bios_vendor(void)
{
    if (!vendor_read) {
        read_vendor();
    }
    return vendor;
}

int smbios_vendor_is(const char *prefix)
{
    const char *v = smbios_bios_vendor();
    int i;

    for (i = 0; prefix[i] != '\0'; i++) {
        if (v[i] != prefix[i]) {
            return 0;
        }
    }
    return i > 0;
}

void smbios_set_table(const void *at, unsigned int len)
{
    forced_table = (const uint8_t *)at;
    forced_len = (uint32_t)len;
    vendor_read = 0;
    vendor[0] = '\0';
}
