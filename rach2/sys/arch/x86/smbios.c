/*
 * smbios.c: find the firmware's table and read out of it the strings and
 * numbers Linux's DMI code keeps: the BIOS's, the system's, the board's and
 * the chassis's.
 *
 * The anchor is the entry point UEFI's configuration table names, under SMBIOS
 * 3's GUID or the older one's, when the loader passed the firmware's system
 * table (SMBIOS 3.0, 5.2.1 and 5.2.2); and otherwise "_SM_" on a sixteen-byte
 * boundary between 0xF0000 and 0xFFFFF, with a checksum over its own length,
 * where a UEFI firmware need not put one. The newer "_SM3_" anchor is read too,
 * since a machine that publishes only that one is otherwise a machine with no
 * table. The anchor gives the address and length of the structure table, which
 * is a run of structures each with a four-byte header followed by its formatted
 * area and then its strings, one after another, ending with an empty one.
 *
 * Only the structures of type 0, BIOS Information, 1, System Information,
 * 2, Base Board Information, and 3, Chassis Information, are wanted, and out
 * of them the fields Linux's dmi_decode saves, each from the first structure
 * long enough to hold it. Everything else is stepped over.
 *
 * The region below a megabyte is inside the identity-mapped window. What
 * UEFI's tables and the anchors point at need not be: a machine with more
 * than 64 MiB usually publishes them near the top of its memory, so each is
 * reached through `table_at`, which maps what lies above the window.
 *
 * A firmware's table is data this kernel did not write, and every field read
 * out of it is bounded by the length the anchor gave. A structure that claims
 * more than the table holds, or a string that does not end inside it, is a
 * table this does not read rather than a read that runs off the end.
 */

#include "smbios.h"

#include "../../lib/kint.h"
#include "../../mm/paging.h"
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
#include "../x86_64/mmu.h"
#endif

#define SMBIOS_SCAN_START 0xF0000u
#define SMBIOS_SCAN_END   0x100000u
#define SMBIOS_SCAN_STEP  16u

/* The longest string this keeps, with its NUL. The names are short; a
 * longer one is cut, which is enough to tell one firmware from another. */
#define SMBIOS_VENDOR_MAX 64

/* The entry points, as the specification fixes them: the 32-bit one is 31
 * bytes and carries the table's address at 0x18, and the 64-bit one is 24 and
 * carries it at 0x10. A candidate shorter than its own entry point is not one,
 * whatever its checksum says, because the address would be read from bytes the
 * checksum never covered. */
#define SMBIOS_EP32_LEN 0x1F
#define SMBIOS_EP64_LEN 0x18

/* UEFI's system table: its signature, "IBI SYST", and where the count and
 * the address of the configuration table are in the 64-bit and the 32-bit
 * layouts, whose entries are a GUID and an address of 8 or 4 bytes. A
 * firmware lists a few dozen tables; more than this is not a table. */
#define EFI_SYSTAB_SIGNATURE 0x5453595320494249ULL
#define EFI_CFG_COUNT_64     104
#define EFI_CFG_TABLE_64     112
#define EFI_CFG_COUNT_32     64
#define EFI_CFG_TABLE_32     68
#define EFI_CFG_MAX          256u

/* The GUIDs of the SMBIOS 3 and the SMBIOS entry points, in the order of
 * their bytes in memory. */
static const uint8_t smbios3_guid[16] = {0x44, 0x15, 0xFD, 0xF2, 0x94, 0x97, 0x2C, 0x4A,
                                         0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94};
static const uint8_t smbios_guid[16] = {0x31, 0x2D, 0x9D, 0xEB, 0x88, 0x2D, 0xD3, 0x11,
                                        0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D};

/* How a field's bytes are read: the number of a string; a major and a minor
 * number, at the offset less one and at the offset, of which FF FF is none;
 * a number less its top bit; or sixteen bytes of a UUID, of which all ones
 * and all zeros are none. */
#define FIELD_STRING  0
#define FIELD_RELEASE 1
#define FIELD_TYPE    2
#define FIELD_UUID    3

/* Each field of dmi.h's DMI_ numbers: the type of its structure, its
 * offset in the formatted area, and how it is read, as Linux's dmi_decode
 * reads it. */
static const struct {
    uint8_t type;
    uint8_t offset;
    uint8_t how;
} dmi_fields[DMI_FIELDS] = {
    {0, 4, FIELD_STRING},   {0, 5, FIELD_STRING}, {0, 8, FIELD_STRING},  {0, 21, FIELD_RELEASE},
    {0, 23, FIELD_RELEASE}, {1, 4, FIELD_STRING}, {1, 5, FIELD_STRING},  {1, 6, FIELD_STRING},
    {1, 7, FIELD_STRING},   {1, 8, FIELD_UUID},   {1, 25, FIELD_STRING}, {1, 26, FIELD_STRING},
    {2, 4, FIELD_STRING},   {2, 5, FIELD_STRING}, {2, 6, FIELD_STRING},  {2, 7, FIELD_STRING},
    {2, 8, FIELD_STRING},   {3, 4, FIELD_STRING}, {3, 5, FIELD_TYPE},    {3, 6, FIELD_STRING},
    {3, 7, FIELD_STRING},   {3, 8, FIELD_STRING}};

/* The fields as read, and whether each was there, and whether the table has
 * been read; the table's version as Linux's dmi_ver keeps it, major << 16 |
 * minor << 8 | revision; a host test's structures and their length, NULL
 * for the search; and the UEFI system table's physical address, 0 for none,
 * and whether its pointers are 64 bits. */
static char dmi_text[DMI_FIELDS][SMBIOS_VENDOR_MAX];
static unsigned char dmi_have[DMI_FIELDS];
static int vendor_read;
static uint32_t dmi_version;
static const uint8_t *forced_table;
static uint32_t forced_len;
static uint64_t efi_systab;
static int efi_64;

/* How physical memory is read: `table_at`, or a host test's reader. */
static const uint8_t *table_at(uint64_t at, uint32_t len);
static const uint8_t *(*phys_at)(uint64_t at, uint32_t len) = table_at;

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

/** The little-endian value of `n` bytes at `p`. */
static uint64_t le(const uint8_t *p, int n)
{
    uint64_t v = 0;

    while (n-- > 0) {
        v = v << 8 | p[n];
    }
    return v;
}

/**
 * Whether the 32 bytes at `p` begin an entry point: the 32-bit one, with a
 * second anchor inside it and a checksum of its own over the fifteen bytes
 * that hold the table's address and length, both required, since a
 * candidate whose length field is short passes the outer checksum over
 * bytes that do not include the address; or the 64-bit one.
 */
static int anchor_ok(const uint8_t *p)
{
    if (four_are(p, "_SM_")) {
        return p[5] >= SMBIOS_EP32_LEN && p[5] <= 32 && sum_of(p, p[5]) == 0 &&
               four_are(p + 0x10, "_DMI") && p[0x14] == '_' && sum_of(p + 0x10, 15) == 0;
    }
    return four_are(p, "_SM3") && p[4] == '_' && p[6] >= SMBIOS_EP64_LEN && p[6] <= 32 &&
           sum_of(p, p[6]) == 0;
}

/** Whether the 16 bytes at `p` are the GUID `guid`. */
static int guid_is(const uint8_t *p, const uint8_t *guid)
{
    int i;

    for (i = 0; i < 16 && p[i] == guid[i]; i++) {
    }
    return i == 16;
}

/**
 * The entry point UEFI's configuration table names under `guid`, or NULL:
 * the system table's signature checked, its count of tables bounded, and
 * the entry point's bytes checked as the scan checks them.
 */
static const uint8_t *efi_anchor(const uint8_t *guid)
{
    const uint8_t *st, *cfg, *e, *p;
    uint32_t n, size = efi_64 ? 24u : 20u, i;
    uint64_t at;

    st = phys_at(efi_systab, efi_64 ? EFI_CFG_TABLE_64 + 8 : EFI_CFG_TABLE_32 + 4);
    if (!st || le(st, 8) != EFI_SYSTAB_SIGNATURE) {
        return 0;
    }
    n = (uint32_t)(efi_64 ? le(st + EFI_CFG_COUNT_64, 8) : le(st + EFI_CFG_COUNT_32, 4));
    at = efi_64 ? le(st + EFI_CFG_TABLE_64, 8) : le(st + EFI_CFG_TABLE_32, 4);
    if (n == 0 || n > EFI_CFG_MAX || (cfg = phys_at(at, n * size)) == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        e = cfg + i * size;
        if (guid_is(e, guid)) {
            p = phys_at(le(e + 16, efi_64 ? 8 : 4), 32);
            return p && anchor_ok(p) ? p : 0;
        }
    }
    return 0;
}

/**
 * The anchor, or NULL: UEFI's SMBIOS 3 entry point, then its SMBIOS one,
 * then the scan of the region below a megabyte.
 */
static const uint8_t *find_anchor(void)
{
    const uint8_t *region, *p;
    uint32_t at;

    if (efi_systab != 0) {
        if ((p = efi_anchor(smbios3_guid)) != 0 || (p = efi_anchor(smbios_guid)) != 0) {
            return p;
        }
    }
    region = phys_at(SMBIOS_SCAN_START, SMBIOS_SCAN_END - SMBIOS_SCAN_START);
    if (!region) {
        return 0;
    }
    for (at = 0; at + 32u <= SMBIOS_SCAN_END - SMBIOS_SCAN_START; at += SMBIOS_SCAN_STEP) {
        if (anchor_ok(region + at)) {
            return region + at;
        }
    }
    return 0;
}

/**
 * Where the structure table is and how long it is, from an anchor.
 *
 * The 32-bit entry point has the length in two bytes at 0x16 and the
 * address in four at 0x18; the 64-bit one has the maximum length in four
 * bytes at 0x0C and the address in eight at 0x10, which `table_at` refuses
 * past 4 GiB on i686. A region of no length holds no structure, and one
 * that wraps the address space is not a region.
 *
 * @return 1 with `*at` and `*len` filled, 0 when the anchor is one this does
 *         not read or names a region that does not fit an address.
 */
static int table_of(const uint8_t *anchor, uint64_t *at, uint32_t *len)
{
    if (four_are(anchor, "_SM_")) {
        *len = (uint32_t)le(anchor + 0x16, 2);
        *at = le(anchor + 0x18, 4);
    } else if (four_are(anchor, "_SM3") && anchor[4] == '_') {
        *len = (uint32_t)le(anchor + 0x0C, 4);
        *at = le(anchor + 0x10, 8);
    } else {
        return 0;
    }
    if (*len == 0 || *at + *len < *at) {
        return 0;
    }
    return 1;
}

/**
 * An address this kernel can read the `len` bytes at physical `at` through.
 *
 * A table inside the identity-mapped window is read where it is. One above it
 * is mapped first: on a machine with more than 64 MiB the firmware usually
 * publishes its table near the top of memory, and reading that through its
 * physical address would fault. The mapping lasts for the life of the kernel,
 * which costs a few pages once, since the vendor is read once. On x86-64 the
 * direct map has every table already. On i686 a table past 4 GiB is not
 * reached.
 *
 * @return the address, or 0 when the table cannot be reached.
 */
static const uint8_t *table_at(uint64_t at, uint32_t len)
{
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
    (void)len;
    return (const uint8_t *)MMU_VIRT(at);
#else
    uintptr_t mapped;

    if (at + len < at || at + len > 0x100000000ULL) {
        return 0;
    }
    if (at + len <= (uint32_t)HARD_MEMORY_LIMIT) {
        return (const uint8_t *)(uintptr_t)at;
    }
    mapped = map_mmio((uintptr_t)at, (size_t)len);
    return (const uint8_t *)mapped;
#endif
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

/** Append `v` in decimal to the string `out` of SMBIOS_VENDOR_MAX bytes. */
static void put_dec(char *out, uint32_t v)
{
    char digits[11];
    int n = 0, len = 0;

    while (out[len] != '\0') {
        len++;
    }
    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    while (n > 0 && len < SMBIOS_VENDOR_MAX - 1) {
        out[len++] = digits[--n];
    }
    out[len] = '\0';
}

/**
 * Write the sixteen bytes at `d` as a UUID into `out`, in lowercase, with
 * the first three fields' bytes reversed for a table of version 2.6 and
 * later, which stores them little-endian, as Linux's %pUl, and in their
 * order for an older one, as %pUb.
 */
static void put_uuid(char *out, const uint8_t *d)
{
    static const char hex[] = "0123456789abcdef";
    static const unsigned char swapped[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    int i, len = 0;

    for (i = 0; i < 16; i++) {
        int b = d[dmi_version >= 0x020600u ? swapped[i] : i];

        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[len++] = '-';
        }
        out[len++] = hex[b >> 4];
        out[len++] = hex[b & 15];
    }
    out[len] = '\0';
}

/**
 * Save the field `f` from the structure at `p`, whose strings begin at
 * `strings`, unless an earlier structure gave it or this one is too short
 * to hold it, as Linux's dmi_save_ident and its kin. A string of spaces
 * alone is saved empty, and a number of none is not saved.
 */
static void dmi_save(const uint8_t *p, const uint8_t *strings, const uint8_t *end, int f)
{
    uint8_t off = dmi_fields[f].offset, flen = p[1];
    char *out = dmi_text[f];
    int i;

    if (dmi_have[f] || flen <= off) {
        return;
    }
    out[0] = '\0';
    if (dmi_fields[f].how == FIELD_STRING) {
        copy_string(strings, end, (int)p[off], out, SMBIOS_VENDOR_MAX);
        for (i = 0; out[i] == ' '; i++) {
        }
        if (out[i] == '\0') {
            out[0] = '\0';
        }
    } else if (dmi_fields[f].how == FIELD_RELEASE) {
        if (p[off - 1] == 0xFF && p[off] == 0xFF) {
            return;
        }
        put_dec(out, p[off - 1]);
        for (i = 0; out[i] != '\0'; i++) {
        }
        out[i] = '.';
        out[i + 1] = '\0';
        put_dec(out, p[off]);
    } else if (dmi_fields[f].how == FIELD_TYPE) {
        put_dec(out, p[off] & 0x7Fu);
    } else {
        int zero = 1, ones = 1;

        if (flen < off + 16) {
            return;
        }
        for (i = 0; i < 16; i++) {
            zero = zero && p[off + i] == 0x00;
            ones = ones && p[off + i] == 0xFF;
        }
        if (zero || ones) {
            return;
        }
        put_uuid(out, p + off);
    }
    dmi_have[f] = 1;
}

/** The table's version from its anchor `p`, as Linux's dmi_ver: SMBIOS 3's
 * major, minor and revision, or the older anchor's major and minor with
 * the versions firmware misreports put right. */
static uint32_t anchor_version(const uint8_t *p)
{
    uint32_t v;

    if (four_are(p, "_SM3")) {
        return (uint32_t)p[7] << 16 | (uint32_t)p[8] << 8 | p[9];
    }
    v = (uint32_t)p[6] << 8 | p[7];
    if (v == 0x021Fu || v == 0x0221u) {
        v = 0x0203u;
    } else if (v == 0x0233u) {
        v = 0x0206u;
    }
    return v << 8;
}

/**
 * Read the fields out of the table, each absent when no structure holds
 * it. Every structure of types 0 to 3 is offered each field of its type,
 * and the first long enough to hold one gives it; a string field of 0
 * names no string and is saved empty.
 */
static void read_vendor(void)
{
    const uint8_t *anchor;
    const uint8_t *p;
    uint64_t at = 0;
    uint32_t len = 0;
    const uint8_t *end;
    int f;

    for (f = 0; f < DMI_FIELDS; f++) {
        dmi_text[f][0] = '\0';
        dmi_have[f] = 0;
    }
    vendor_read = 1;
    dmi_version = 0x030000u;

    if (forced_table) {
        p = forced_table;
        len = forced_len;
    } else {
        anchor = find_anchor();
        if (!anchor || !table_of(anchor, &at, &len)) {
            return;
        }
        dmi_version = anchor_version(anchor);
        p = phys_at(at, len);
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
        if (type == 127) {
            return; /* the end-of-table structure */
        }
        for (f = 0; f < DMI_FIELDS; f++) {
            if (dmi_fields[f].type == type) {
                dmi_save(p, strings, end, f);
            }
        }
        /* Past the strings, which end with an empty one. */
        p = strings;
        while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) {
            p++;
        }
        p += 2;
    }
}

const char *dmi_field(int field)
{
    if (!vendor_read) {
        read_vendor();
    }
    return field >= 0 && field < DMI_FIELDS && dmi_have[field] ? dmi_text[field] : 0;
}

const char *smbios_bios_vendor(void)
{
    const char *v = dmi_field(DMI_BIOS_VENDOR);

    return v ? v : "";
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

/** Whether `s` holds `sub`, as Linux's `strstr` in `dmi_matches`. */
static int contains(const char *s, const char *sub)
{
    int i, j;

    for (i = 0; s[i] != '\0'; i++) {
        for (j = 0; sub[j] != '\0' && s[i + j] == sub[j]; j++) {
        }
        if (sub[j] == '\0') {
            return 1;
        }
    }
    return sub[0] == '\0';
}

int smbios_system_is(const char *maker, const char *name)
{
    const char *sys_vendor = dmi_field(DMI_SYS_VENDOR);
    const char *product = dmi_field(DMI_PRODUCT_NAME);

    return sys_vendor && product && sys_vendor[0] != '\0' && product[0] != '\0' &&
           contains(sys_vendor, maker) && contains(product, name);
}

void smbios_init(uint64_t systab, int wide)
{
    efi_systab = systab;
    efi_64 = wide;
    vendor_read = 0;
}

void smbios_set_memory(const uint8_t *(*read)(uint64_t at, uint32_t len))
{
    phys_at = read ? read : table_at;
    vendor_read = 0;
}

void smbios_set_table(const void *at, unsigned int len)
{
    forced_table = (const uint8_t *)at;
    forced_len = (uint32_t)len;
    vendor_read = 0;
}
