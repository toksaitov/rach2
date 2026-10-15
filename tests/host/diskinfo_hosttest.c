/*
 * diskinfo_hosttest.c: the text rach2/sys/block/diskinfo.c renders for
 * /proc/disk, from IDENTIFY words and disk records built in memory.
 *
 * host-deps: rach2/sys/block/diskinfo.c rach2/sys/block/mbr.c
 * host-deps: rach2/sys/block/partition.c rach2/sys/block/gpt.c
 *
 * `tests/run diskdiag` boots a disk and hashes its sectors. A device in a
 * guest does not answer with a serial number full of control bytes or a
 * capacity above 2^32 on request, so the conversions and the boundaries are
 * tested here.
 *
 * ATA stores its strings with the first character of each pair in the high
 * byte of its 16-bit word, and a field that fills its width has no
 * terminator. The sector count is a 48-bit number in a kernel that links no
 * libgcc, so it is turned into decimal by long division over 32-bit halves.
 */

extern int printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *to, const char *from);
extern char *strstr(const char *hay, const char *needle);

#include "../../rach2/sys/block/diskinfo.h"
#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/block/diskcmd.h"

/* The disk the probe half of diskinfo.c reads; no row runs the probe. */
block_device_t *block0 = 0;

/** No disk answers the probe. */
int block_probe(void)
{
    return -1;
}

/** No disk is ready. */
int block_ready(void)
{
    return -1;
}

/** The block layer's command count, 0 for every row. */
uint64_t block_commands(void)
{
    return 0;
}

/** The block layer's flush count, 0 for every row. */
uint64_t block_flushes(void)
{
    return 0;
}

/** The block layer's poll count, 0 for every row. */
uint64_t block_polls(void)
{
    return 0;
}

/** The block layer's ticks spent waiting, 0 for every row. */
uint64_t block_wait_ticks(void)
{
    return 0;
}

/** The block layer's requests in flight, 0 for every row. */
uint32_t block_in_flight(void)
{
    return 0;
}

/** The block layer's overlap count, 0 for every row. */
uint32_t block_overlaps(void)
{
    return 0;
}

/** The block layer's quarantine count, 0 for every row. */
uint32_t block_quarantines(void)
{
    return 0;
}

/** The block layer's release count, 0 for every row. */
uint32_t block_releases(void)
{
    return 0;
}

/** The tick of the block layer's last release, 0 for every row. */
uint32_t block_release_tick(void)
{
    return 0;
}

/** The block layer's late completions, 0 for every row. */
uint32_t block_lag_completions(void)
{
    return 0;
}

/** The count of raw disk calls through devfs. The kernel's count changes as
 *  the machine runs; this one is 0, so the expected text is a constant. */
uint64_t devfs_raw_calls(void)
{
    return 0;
}

/** Publishing a disk's nodes does nothing here. */
void devfs_disk_publish(struct disk *d)
{
    (void)d;
}

/*
 * The registry this test presents: none by default, and the two disks below
 * for the rows that read the `disk` and `identity` lines. They stand in for
 * the registry the kernel fills, which no host test links.
 */
static struct disk fake[2];
static block_device_t fake_dev[2];
static int fake_count;
static int fake_claimed; /* nonzero: a mount holds fake[1] */

/** Whether a mount holds `d`: only the second disk, and only while
 *  `fake_claimed` is set. */
int devfs_disk_claimed(const struct disk *d)
{
    return fake_claimed && d == &fake[1];
}

/** The number of records in the registry. */
int disk_count(void)
{
    return fake_count;
}

/** Record `n` of the registry, or NULL past its end. */
struct disk *disk_at(int n)
{
    return n >= 0 && n < fake_count ? &fake[n] : 0;
}

/** The kernel log; the probe's records are dropped. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** The buffer cache's hit count; the test links no cache, so 0. */
uint64_t bcache_stat_hits(void)
{
    return 0;
}
/** The buffer cache's fill count, 0. */
uint64_t bcache_stat_fills(void)
{
    return 0;
}
/** The buffer cache's wait count, 0. */
uint64_t bcache_stat_waits(void)
{
    return 0;
}
/** The buffer cache's count of buffers visited, 0. */
uint64_t bcache_stat_visits(void)
{
    return 0;
}

/* The build policy diskinfo_format prints; the rows set both values. */
static int policy_now = DISKCMD_READ_WRITE;

/** The policy `policy_now` gives. */
int diskcmd_policy(void)
{
    return policy_now;
}

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** Store `text` in the IDENTIFY words `id` from `first_word`, `chars`
 *  characters wide, as a drive does: two characters a word, the first in the
 *  high byte, and spaces after the text. */
static void put_field(uint16_t *id, int first_word, int chars, const char *text)
{
    int len = 0, i;

    while (text[len] != '\0') {
        len++;
    }
    for (i = 0; i < chars; i += 2) {
        uint8_t hi = (uint8_t)((i < len) ? text[i] : ' ');
        uint8_t lo = (uint8_t)((i + 1 < len) ? text[i + 1] : ' ');

        id[first_word + (i / 2)] = (uint16_t)(((uint16_t)hi << 8) | lo);
    }
}

/**
 * Run the rows, in four groups.
 *
 * - `diskinfo_ata_string`: a model and a serial read back in order without
 *   trailing spaces; a field that fills its width, which has no terminator
 *   in the IDENTIFY data, is terminated in the output; an all-space field is
 *   empty; and bytes of all ones, NUL, newline and DEL each become '?', so
 *   no byte the drive sends can split a line that awk or grep reads.
 * - `diskinfo_u64_dec`: small numbers, 2^32 - 1 and 2^32, where a carry
 *   lost between the halves shows, a 1 TB disk's sector count, the largest
 *   48-bit value, and a buffer too short, which gets nothing.
 * - `diskinfo_format`: the whole record of a kernel never asked, one with no
 *   device and one under the read-only policy; the widest record, which must
 *   fit `DISKINFO_RECORD_BYTES`; the lines of a present disk, each compared
 *   as rendered; each `mbr-signature` state; and buffers one byte short, of
 *   one byte and of none, with a sentinel past the cap to catch a byte
 *   written at the edge.
 * - `diskinfo_format_disks`: a fixed disk of 512-byte sectors with nothing
 *   mounted and a removable one of 4096-byte sectors held by a mount. Every
 *   field of the `identity` line decides whether an installer offers the
 *   disk, so each is compared as rendered. A removed disk is hidden while
 *   its mount still holds the record.
 *
 * Returns the count of failed checks, at most 255.
 */
int main(void)
{
    static uint16_t id[256];
    char out[64];
    char buf[DISKINFO_RECORD_BYTES];
    struct diskinfo d;
    int i;

    put_field(id, 27, 40, "Samsung SSD 870 EVO 1TB");
    diskinfo_ata_string(out, 40, id, 27);
    CHECK(strcmp(out, "Samsung SSD 870 EVO 1TB") == 0,
          "a model comes back in order, with its interior spaces and no trailing ones");

    put_field(id, 10, 20, "S5Y2NJ0RA12345X");
    diskinfo_ata_string(out, 20, id, 10);
    CHECK(strcmp(out, "S5Y2NJ0RA12345X") == 0, "a serial comes back in order");

    put_field(id, 27, 40, "0123456789012345678901234567890123456789");
    diskinfo_ata_string(out, 40, id, 27);
    CHECK(strlen(out) == 40, "a field that fills its width is terminated by diskinfo_ata_string");
    CHECK(out[39] == '9' && out[40] == '\0', "and its last character survives");

    for (i = 0; i < 20; i++) {
        id[27 + i] = 0x2020;
    }
    diskinfo_ata_string(out, 40, id, 27);
    CHECK(out[0] == '\0', "an all-space field is empty");

    for (i = 0; i < 20; i++) {
        id[27 + i] = 0xFFFF;
    }
    diskinfo_ata_string(out, 40, id, 27);
    CHECK(strlen(out) == 40, "an all-ones field is still forty characters");
    for (i = 0; i < 40; i++) {
        if (out[i] != '?') {
            fails++;
            printf("  FAIL: an all-ones field left a byte that is not '?'\n");
            break;
        }
    }
    id[27] = 0x000A; /* NUL and a newline */
    id[28] = 0x7F41; /* DEL and 'A' */
    diskinfo_ata_string(out, 40, id, 27);
    CHECK(out[0] == '?' && out[1] == '?' && out[2] == '?' && out[3] == 'A',
          "a NUL, a newline and a DEL each become '?', so no line can be split");

    CHECK(diskinfo_u64_dec(out, (int)sizeof(out), 0) == 1 && strcmp(out, "0") == 0, "zero");
    CHECK(diskinfo_u64_dec(out, (int)sizeof(out), 9) == 1 && strcmp(out, "9") == 0, "nine");
    CHECK(diskinfo_u64_dec(out, (int)sizeof(out), 10) == 2 && strcmp(out, "10") == 0, "ten");
    CHECK(diskinfo_u64_dec(out, (int)sizeof(out), 99) == 2 && strcmp(out, "99") == 0,
          "ninety-nine");
    CHECK(diskinfo_u64_dec(out, (int)sizeof(out), 4294967295u) == 10 &&
              strcmp(out, "4294967295") == 0,
          "the largest 32-bit value");
    CHECK(diskinfo_u64_dec(out, (int)sizeof(out), 4294967296ull) == 10 &&
              strcmp(out, "4294967296") == 0,
          "one past it, which is where a lost carry between the halves shows");
    CHECK(diskinfo_u64_dec(out, (int)sizeof(out), 1953525168ull) == 10 &&
              strcmp(out, "1953525168") == 0,
          "a 1 TB disk's sector count");
    CHECK(diskinfo_u64_dec(out, (int)sizeof(out), 281474976710655ull) == 15 &&
              strcmp(out, "281474976710655") == 0,
          "the largest 48-bit value, which is the field's real width");
    CHECK(diskinfo_u64_dec(out, 3, 4294967295u) == 0 && out[0] == '\0',
          "a buffer too short writes nothing, not half a number");

    for (i = 0; i < (int)sizeof(d); i++) {
        ((char *)&d)[i] = 0;
    }
    d.state = DISKINFO_NOT_ASKED;
    d.driver = "none";
    diskinfo_format(&d, buf, (int)sizeof(buf));
    CHECK(strcmp(buf,
                 "state not-asked\ndriver none\npolicy read-write\ndisk-commands 0\n"
                 "disk-flushes 0\ndisk-polls 0\ndisk-wait-ticks 0\ndisk-in-flight 0\ndisk-overlaps "
                 "0\ndisk-quarantines 0\ndisk-releases 0\n"
                 "disk-release-tick 0\nlag-completions 0\ncache-hits 0\ncache-fills 0\ncache-waits "
                 "0\ncache-visits 0\n") == 0,
          "a kernel that was never asked says so and stops there");

    d.state = DISKINFO_NO_DEVICE;
    diskinfo_format(&d, buf, (int)sizeof(buf));
    CHECK(strcmp(buf,
                 "state no-device\ndriver none\npolicy read-write\ndisk-commands 0\n"
                 "disk-flushes 0\ndisk-polls 0\ndisk-wait-ticks 0\ndisk-in-flight 0\ndisk-overlaps "
                 "0\ndisk-quarantines 0\ndisk-releases 0\n"
                 "disk-release-tick 0\nlag-completions 0\ncache-hits 0\ncache-fills 0\ncache-waits "
                 "0\ncache-visits 0\n") == 0,
          "a kernel that asked and got nothing says a different thing");

    policy_now = DISKCMD_READ_ONLY;
    diskinfo_format(&d, buf, (int)sizeof(buf));
    CHECK(strcmp(buf,
                 "state no-device\ndriver none\npolicy read-only\ndisk-commands 0\n"
                 "disk-flushes 0\ndisk-polls 0\ndisk-wait-ticks 0\ndisk-in-flight 0\ndisk-overlaps "
                 "0\ndisk-quarantines 0\ndisk-releases 0\n"
                 "disk-release-tick 0\nlag-completions 0\ncache-hits 0\ncache-fills 0\ncache-waits "
                 "0\ncache-visits 0\n") == 0,
          "the file names the build policy the kernel is running");
    policy_now = DISKCMD_READ_WRITE;

    d.state = DISKINFO_PRESENT;
    d.driver = "ahci";
    d.port = 0;
    d.mounted = 0;
    d.sectors = 1953525168ull;
    d.sector_size = 512;
    d.mbr_state = DISKINFO_MBR_OK;
    d.mbr_id = 0x4905B015u;
    d.parts[0].type = 0x07;
    d.parts[0].start_lba = 2048;
    d.parts[0].sector_count = 1024000;
    d.parts[1].type = 0xFF;
    d.parts[1].start_lba = 4294967295u;
    d.parts[1].sector_count = 4294967295u;
    {
        int n;

        for (i = 0; i < DISKINFO_MODEL_CHARS; i++) {
            d.model[i] = 'M';
        }
        d.model[DISKINFO_MODEL_CHARS] = '\0';
        for (i = 0; i < DISKINFO_SERIAL_CHARS; i++) {
            d.serial[i] = 'S';
        }
        d.serial[DISKINFO_SERIAL_CHARS] = '\0';
        for (i = 0; i < DISKINFO_FIRMWARE_CHARS; i++) {
            d.firmware[i] = 'F';
        }
        d.firmware[DISKINFO_FIRMWARE_CHARS] = '\0';

        n = diskinfo_format(&d, buf, (int)sizeof(buf));
        CHECK(n > 0 && buf[n] == '\0', "the widest record renders and is terminated");
        CHECK(n < (int)sizeof(buf), "and fits the buffer /proc/disk gives it, with room over");
    }

    {
        static const char *want[] = {"state present\n",
                                     "driver ahci\n",
                                     "policy read-write\ndisk-commands 0\ndisk-flushes 0\n",
                                     "cache-hits 0\ncache-fills 0\ncache-waits 0\ncache-visits 0\n",
                                     "port 0\n",
                                     "mounted 0\n",
                                     "sector-size 512\n",
                                     "sectors 1953525168\n",
                                     "raw-batch-sectors 2048\n",
                                     "raw-driver-calls 0\n",
                                     "mbr-signature ok\n",
                                     "mbr-id 4905b015\n",
                                     "part1 07 2048 1024000\n",
                                     "part2 ff 4294967295 4294967295\n",
                                     "part3 00 0 0\n",
                                     "part4 00 0 0\n"};
        int w;

        for (w = 0; w < (int)(sizeof(want) / sizeof(want[0])); w++) {
            const char *p = buf;
            int found = 0;

            while (*p) {
                const char *a = p;
                const char *b = want[w];

                while (*b && *a == *b) {
                    a++;
                    b++;
                }
                if (!*b) {
                    found = 1;
                    break;
                }
                p++;
            }
            if (!found) {
                fails++;
                printf("  FAIL: the rendered file does not carry %s", want[w]);
            }
        }
    }

    d.mbr_state = DISKINFO_MBR_NONE;
    diskinfo_format(&d, buf, (int)sizeof(buf));
    CHECK(buf[0] != '\0', "a table with no signature still renders");
    d.mbr_state = DISKINFO_MBR_UNREAD;
    diskinfo_format(&d, buf, (int)sizeof(buf));
    CHECK(buf[0] != '\0', "a first sector that could not be read still renders");
    d.mbr_state = DISKINFO_MBR_OK;

    {
        int full = diskinfo_format(&d, buf, (int)sizeof(buf));
        static char small[600];
        int n;

        for (i = 0; i < (int)sizeof(small); i++) {
            small[i] = (char)0x7E;
        }
        n = diskinfo_format(&d, small, full);
        CHECK(n == full - 1, "one byte short renders one character less");
        CHECK(small[n] == '\0', "and terminates inside its own buffer");
        CHECK(small[full] == (char)0x7E, "and writes nothing past the cap it was given");

        n = diskinfo_format(&d, small, 1);
        CHECK(n == 0 && small[0] == '\0', "a one-byte buffer has only the terminator");
        n = diskinfo_format(&d, small, 0);
        CHECK(n == 0, "a zero-byte buffer writes nothing at all");
    }

    {
        static char text[1024];

        fake_dev[0].sector_size = 512;
        fake_dev[0].sector_count = 3389440;
        fake[0].present = 1;
        fake[0].number = 0;
        fake[0].dev = &fake_dev[0];
        fake[0].transport = "ahci";
        fake[0].location = 0;
        fake[0].removable = 0;
        strcpy(fake[0].model, "QEMU HARDDISK");
        strcpy(fake[0].serial, "QM00001");
        fake[0].table.kind = PART_MBR;
        fake[0].table.count = 0;

        fake_dev[1].sector_size = 4096;
        fake_dev[1].sector_count = 1000;
        fake[1].present = 1;
        fake[1].number = 1;
        fake[1].dev = &fake_dev[1];
        fake[1].transport = "usb";
        fake[1].location = 4;
        fake[1].removable = 1;
        strcpy(fake[1].model, "A Stick With Spaces");
        strcpy(fake[1].serial, "S 1");
        fake[1].table.kind = PART_NONE;
        fake[1].table.count = 0;

        fake_count = 2;
        fake_claimed = 1;
        text[0] = '\0';
        diskinfo_format_disks(text, (int)sizeof(text));
        CHECK(strstr(text, "disk 0 ahci 0 3389440 mbr QEMU HARDDISK\n") != 0,
              "a disk is named with its transport, its sectors, its table and its model");
        CHECK(strstr(text, "identity 0 512 fixed free QM00001\n") != 0,
              "and described with its sector size, its kind, its mount state and its serial");
        CHECK(strstr(text, "disk 1 usb 4 1000 none A Stick With Spaces\n") != 0,
              "a second disk is named after it");
        CHECK(strstr(text, "identity 1 4096 removable mounted S 1\n") != 0,
              "with its own sector size, a removable kind, a mount, and a serial holding a space");

        fake_claimed = 0;
        text[0] = '\0';
        diskinfo_format_disks(text, (int)sizeof(text));
        CHECK(strstr(text, "identity 1 4096 removable free S 1\n") != 0,
              "and a disk nothing mounts is free");
        fake[1].present = 0;
        fake_claimed = 1;
        diskinfo_format_disks(text, (int)sizeof(text));
        CHECK(strstr(text, "disk 1 ") == 0 && strstr(text, "identity 1 ") == 0,
              "a removed disk is hidden while its mount still holds the record");
        CHECK(strstr(text, "disk 0 ahci ") != 0, "the other disk stays listed");
        fake_count = 0;
    }

    return fails > 255 ? 255 : fails;
}
