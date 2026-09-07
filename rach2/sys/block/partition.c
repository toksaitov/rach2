/*
 * The partition service. `partition.h` says what it reads.
 */

#include "partition.h"
#include "block.h"

#define SECTOR 512u

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static int extended_type(uint8_t type)
{
    return type == PART_MBR_EXTENDED_CHS || type == PART_MBR_EXTENDED_LBA ||
           type == PART_MBR_EXTENDED_LINUX;
}

static void clear_entry(struct part_entry *e)
{
    int i;

    e->number = 0;
    e->start_lba = 0;
    e->sector_count = 0;
    e->mbr_type = 0;
    for (i = 0; i < 16; i++) {
        e->type_guid[i] = 0;
        e->guid[i] = 0;
    }
    e->label[0] = '\0';
}

/**
 * Add a partition to `t` unless it lies outside `[low, high]` or overlaps
 * one already taken, in which case it is counted in `refused`. Returns the
 * entry, or NULL when it was refused.
 */
/* Whether sector `lba` lies inside a partition already accepted: the next
   record of an extended chain may not, or the partition holding it would
   be written over its own table. */
static int inside_accepted(const struct part_table *t, uint64_t lba)
{
    int i;

    for (i = 0; i < t->count; i++) {
        if (lba >= t->parts[i].start_lba &&
            lba < t->parts[i].start_lba + t->parts[i].sector_count) {
            return 1;
        }
    }
    return 0;
}

static struct part_entry *take(struct part_table *t, int number, uint64_t start, uint64_t count,
                               uint64_t low, uint64_t high)
{
    struct part_entry *e;
    int i;

    if (count == 0 || start < low || start > high || count - 1 > high - start) {
        t->refused++;
        return 0;
    }
    for (i = 0; i < t->count; i++) {
        uint64_t other = t->parts[i].start_lba;

        if (start <= other + t->parts[i].sector_count - 1 && other <= start + count - 1) {
            t->refused++;
            return 0;
        }
    }
    /* The count and the number both, since devfs gives a minor per number
     * and a number past the table's would take the next disk's. */
    if (t->count >= PART_MAX || number < 1 || number > (int)PART_MAX) {
        t->refused++;
        return 0;
    }
    e = &t->parts[t->count++];
    clear_entry(e);
    e->number = number;
    e->start_lba = start;
    e->sector_count = count;
    return e;
}

/**
 * Follow the chain of extended boot records inside the extended partition
 * at `ext_start` of `ext_count` sectors, taking each logical partition.
 *
 * Each record has the logical partition in its first entry, relative to the
 * record, and the next record in its second, relative to the extended
 * partition. The chain ends at a record without a next entry, at one that
 * cannot be read or has no signature, or after `PART_MAX` records, so a
 * chain that loops ends too.
 */
static void follow_logical(struct part_table *t, part_read_fn read, void *ctx,
                           uint64_t disk_sectors, uint64_t ext_start, uint64_t ext_count,
                           uint8_t *sector)
{
    uint64_t ebr = ext_start;
    uint64_t ext_last = ext_start + ext_count - 1;
    int number = 5;
    int steps;

    if (ext_count == 0 || ext_start == 0 || ext_start >= disk_sectors ||
        ext_count - 1 > disk_sectors - 1 - ext_start) {
        t->refused++;
        return;
    }
    for (steps = 0; steps < PART_MAX; steps++) {
        const uint8_t *e0 = sector + 446;
        const uint8_t *e1 = sector + 446 + 16;
        uint64_t next;

        if (read(ctx, ebr, 1, sector) != 0 || sector[510] != 0x55 || sector[511] != 0xAA) {
            return;
        }
        /* The record itself is not data: a partition that began at the
         * EBR would be published over the bytes that name it, and a later
         * record in the chain would sit inside an accepted range, so
         * writing the first would destroy the second. */
        if (e0[4] != 0 && le32(e0 + 12) != 0 && le32(e0 + 8) != 0) {
            struct part_entry *e = take(t, number, ebr + le32(e0 + 8), le32(e0 + 12), ebr + 1,
                                        ext_last);

            if (e) {
                e->mbr_type = e0[4];
            }
        }
        number++;
        if (!extended_type(e1[4]) || le32(e1 + 12) == 0) {
            return;
        }
        next = ext_start + le32(e1 + 8);
        if (next <= ext_start || next > ext_last || next == ebr || inside_accepted(t, next)) {
            t->refused++;
            return;
        }
        ebr = next;
    }
}

/** Whether `sector` opens a FAT or exFAT volume: a jump, or the exFAT name. */
static int volume_boot_sector(const uint8_t *sector)
{
    if (sector[0] == 0xEB || sector[0] == 0xE9) {
        return 1;
    }
    return sector[3] == 'E' && sector[4] == 'X' && sector[5] == 'F' && sector[6] == 'A' &&
           sector[7] == 'T';
}

static void scan_mbr(struct part_table *t, part_read_fn read, void *ctx, uint64_t disk_sectors,
                     uint8_t *sector, uint8_t *spare)
{
    uint64_t ext_start = 0, ext_count = 0;
    int i;

    t->kind = PART_MBR;
    t->mbr_id = le32(sector + 440);
    for (i = 0; i < 4; i++) {
        const uint8_t *e = sector + 446 + i * 16;
        uint32_t start = le32(e + 8), count = le32(e + 12);
        struct part_entry *p;

        if (e[4] == 0 || count == 0) {
            continue;
        }
        if (extended_type(e[4])) {
            if (ext_count == 0) {
                ext_start = start;
                ext_count = count;
            } else {
                t->refused++;
            }
            continue;
        }
        p = take(t, i + 1, start, count, 1, disk_sectors - 1);
        if (p) {
            p->mbr_type = e[4];
        }
    }
    if (ext_count != 0) {
        follow_logical(t, read, ctx, disk_sectors, ext_start, ext_count, spare);
    }
    if (t->count == 0 && t->refused == 0) {
        t->kind = PART_NONE;
        t->volume = volume_boot_sector(sector);
    }
}

/** The GPT name at `entry + 56`, 36 UTF-16LE units, as ASCII with '?'. */
static void gpt_label(char *out, const uint8_t *entry)
{
    int i, last = -1;

    for (i = 0; i < PART_LABEL_CHARS; i++) {
        uint16_t u = (uint16_t)entry[56 + i * 2] | ((uint16_t)entry[57 + i * 2] << 8);

        if (u == 0) {
            break;
        }
        out[i] = (u >= 0x20 && u <= 0x7E) ? (char)u : '?';
        if (out[i] != ' ') {
            last = i;
        }
    }
    out[last + 1] = '\0';
}

/**
 * Read the header at `header_lba` and its array, and take the entries.
 *
 * Returns 0 when the header and the array checked and the entries were
 * taken, -1 when either failed; `t` is left untouched on -1 so the caller
 * can try the other header.
 */
static int scan_gpt_at(struct part_table *t, part_read_fn read, void *ctx, uint64_t disk_sectors,
                       uint64_t header_lba, uint8_t *work)
{
    uint8_t *header = work + GPT_MAX_ARRAY_BYTES;
    uint32_t count, size, bytes, sectors, i;
    uint64_t first, last, array_lba;

    if (read(ctx, header_lba, 1, header) != 0 ||
        gpt_header_ok(header, header_lba, disk_sectors) != 0) {
        return -1;
    }
    count = gpt_entry_count(header);
    size = gpt_entry_size(header);
    array_lba = gpt_array_lba(header);
    first = gpt_first_usable(header);
    last = gpt_last_usable(header);
    bytes = count * size;
    sectors = (bytes + SECTOR - 1) / SECTOR;
    if (read(ctx, array_lba, sectors, work) != 0 ||
        gpt_crc32(work, bytes) != gpt_array_crc(header)) {
        return -1;
    }
    t->kind = PART_GPT;
    for (i = 0; i < 16; i++) {
        t->disk_guid[i] = header[56 + i];
    }
    for (i = 0; i < count; i++) {
        const uint8_t *entry = work + i * size;
        struct part_entry *e;
        int j, used = 0;

        for (j = 0; j < 16; j++) {
            if (entry[j] != 0) {
                used = 1;
            }
        }
        if (!used) {
            continue;
        }
        e = take(t, (int)i + 1, le64(entry + 32), le64(entry + 40) - le64(entry + 32) + 1, first,
                 last);
        if (!e) {
            continue;
        }
        for (j = 0; j < 16; j++) {
            e->type_guid[j] = entry[j];
            e->guid[j] = entry[16 + j];
        }
        gpt_label(e->label, entry);
    }
    return 0;
}

int part_scan(struct part_table *t, part_read_fn read, void *ctx, uint64_t disk_sectors,
              uint8_t *work)
{
    uint8_t *sector = work + GPT_MAX_ARRAY_BYTES;
    uint8_t *spare = sector + SECTOR;
    int i;

    t->kind = PART_UNREAD;
    t->mbr_id = 0;
    t->count = 0;
    t->refused = 0;
    t->backup_used = 0;
    t->volume = 0;
    for (i = 0; i < 16; i++) {
        t->disk_guid[i] = 0;
    }
    for (i = 0; i < PART_MAX; i++) {
        clear_entry(&t->parts[i]);
    }
    if (disk_sectors == 0 || read(ctx, 0, 1, sector) != 0) {
        return -1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        t->kind = PART_NONE;
        t->volume = volume_boot_sector(sector);
        return 0;
    }
    if (gpt_protective(sector)) {
        if (scan_gpt_at(t, read, ctx, disk_sectors, 1, work) == 0) {
            return 0;
        }
        if (disk_sectors > 1 && scan_gpt_at(t, read, ctx, disk_sectors, disk_sectors - 1, work) == 0) {
            t->backup_used = 1;
            return 0;
        }
        t->kind = PART_GPT_BAD;
        return 0;
    }
    scan_mbr(t, read, ctx, disk_sectors, sector, spare);
    return 0;
}

static const char hex_digits[] = "0123456789abcdef";

void part_guid_text(const uint8_t guid[16], char out[37])
{
    /* The first three fields are little-endian on the disk; the text form
       prints them most significant byte first, and the last two as stored. */
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    int i, n = 0;

    for (i = 0; i < 16; i++) {
        uint8_t b = guid[order[i]];

        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[n++] = '-';
        }
        out[n++] = hex_digits[b >> 4];
        out[n++] = hex_digits[b & 0xF];
    }
    out[n] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int part_guid_parse(const char *text, uint8_t guid[16])
{
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    int i, n = 0;

    for (i = 0; i < 16; i++) {
        int hi, lo;

        if (i == 4 || i == 6 || i == 8 || i == 10) {
            if (text[n++] != '-') {
                return -1;
            }
        }
        hi = hex_value(text[n]);
        lo = hi < 0 ? -1 : hex_value(text[n + 1]);
        if (lo < 0) {
            return -1;
        }
        guid[order[i]] = (uint8_t)((hi << 4) | lo);
        n += 2;
    }
    return text[n] == '\0' ? 0 : -1;
}

int part_guid_equal(const uint8_t a[16], const uint8_t b[16])
{
    int i;

    for (i = 0; i < 16; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}
