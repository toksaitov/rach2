/*
 * diskinfo_hosttest.c: what a disk says about itself, rendered as text.
 *
 * host-deps: rach2/sys/block/diskinfo.c rach2/sys/block/mbr.c
 * host-deps: rach2/sys/block/partition.c rach2/sys/block/gpt.c
 *
 * The kernel side of this is exercised by `tests/run diskdiag`, which boots a
 * real disk and hashes its sectors. What that cannot reach is the arithmetic
 * and the fence posts, because a real device will not answer with a serial
 * number full of control bytes or a capacity above 2^32 on request. So those
 * are crafted here.
 *
 * Two of them are worth naming. ATA stores its strings big-endian WITHIN each
 * 16-bit word, which is the one detail a reader who has not checked the layout
 * gets backwards, and the field has no terminator. And the sector count is
 * a 48-bit number in a kernel that links no libgcc, so it is turned into
 * decimal by long division over 32-bit halves, not by a 64-bit divide
 * that would not link.
 */

extern int printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);
extern char *strcpy(char *to, const char *from);
extern char *strstr(const char *hay, const char *needle);

#include "../../rach2/sys/block/diskinfo.h"
#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/block/diskcmd.h"

/* The stub the driver half of diskinfo.c links against; nothing reaches it. */
block_device_t *block0 = 0;

int block_probe(void)
{
    return -1;
}

int block_ready(void)
{
    return -1;
}

uint64_t block_commands(void)
{
    return 0;
}

uint64_t block_flushes(void)
{
    return 0;
}

uint64_t block_polls(void)
{
    return 0;
}

uint64_t block_wait_ticks(void)
{
    return 0;
}

uint32_t block_in_flight(void)
{
    return 0;
}

uint32_t block_overlaps(void)
{
    return 0;
}

uint32_t block_quarantines(void)
{
    return 0;
}

uint32_t block_releases(void)
{
    return 0;
}

uint32_t block_release_tick(void)
{
    return 0;
}

uint32_t block_lag_completions(void)
{
    return 0;
}

uint64_t devfs_raw_calls(void)
{
    /* The rendered text has a live count; the fixture pins it so the
       expected bytes below are a constant, not whatever a previous case
       happened to leave behind. */
    return 0;
}

void devfs_disk_publish(struct disk *d, uint64_t window_sectors)
{
    (void)d;
    (void)window_sectors;
}

/*
 * The registry this test presents: none by default, and the two disks below
 * for the rows that read the `disk` and `identity` lines. They stand in for
 * the registry the kernel fills, which no host test links.
 */
static struct disk fake[2];
static block_device_t fake_dev[2];
static int fake_count;
static int fake_claimed;

int devfs_disk_claimed(struct disk *d)
{
    /* The second disk is the one with something mounted from it. */
    return fake_claimed && d == &fake[1];
}

int disk_count(void)
{
    return fake_count;
}

struct disk *disk_at(int n)
{
    return n >= 0 && n < fake_count ? &fake[n] : 0;
}

/* The probe says what disk answered through the kernel log, which this test
   has no use for; the records are dropped. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The record prints the buffer cache's counters; here there is no cache. */
uint64_t bcache_stat_hits(void)
{
    return 0;
}
uint64_t bcache_stat_fills(void)
{
    return 0;
}
uint64_t bcache_stat_waits(void)
{
    return 0;
}

/* The build policy diskinfo_format prints; both values are exercised. */
static int policy_now = DISKCMD_READ_WRITE;

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

/** Build an IDENTIFY word array carrying `text` from `first_word`. */
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

int main(void)
{
    static uint16_t id[256];
    char out[64];
    char buf[DISKINFO_RECORD_BYTES];
    struct diskinfo d;
    int i;

    /* ---- the string field, which ATA stores byte-swapped ---- */
    put_field(id, 27, 40, "Samsung SSD 870 EVO 1TB");
    diskinfo_ata_string(out, 40, id, 27);
    CHECK(strcmp(out, "Samsung SSD 870 EVO 1TB") == 0,
          "a model comes back in order, with its interior spaces and no trailing ones");

    put_field(id, 10, 20, "S5Y2NJ0RA12345X");
    diskinfo_ata_string(out, 20, id, 10);
    CHECK(strcmp(out, "S5Y2NJ0RA12345X") == 0, "a serial comes back in order");

    /* A field that is exactly full has no terminator on the wire. */
    put_field(id, 27, 40, "0123456789012345678901234567890123456789");
    diskinfo_ata_string(out, 40, id, 27);
    CHECK(strlen(out) == 40, "a field that fills its width is terminated by us, not by the device");
    CHECK(out[39] == '9' && out[40] == '\0', "and its last character survives");

    /* All spaces is an empty string, not forty of them. */
    for (i = 0; i < 20; i++) {
        id[27 + i] = 0x2020;
    }
    diskinfo_ata_string(out, 40, id, 27);
    CHECK(out[0] == '\0', "an all-space field is empty");

    /* A device that answers all ones, and one that answers control bytes:
       neither may put a byte into the file that awk or grep would act on. */
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

    /* ---- decimal, over 32-bit halves, in a kernel with no 64-bit divide
     * ---- */
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

    /* ---- the window clamp ---- */
    CHECK(diskinfo_window(1000) == 1000, "a small disk is not clamped");
    CHECK(diskinfo_window(DISKINFO_WINDOW_MAX_SECTORS) == DISKINFO_WINDOW_MAX_SECTORS,
          "a disk exactly at the bound is not clamped");
    CHECK(diskinfo_window((uint64_t)DISKINFO_WINDOW_MAX_SECTORS + 1) == DISKINFO_WINDOW_MAX_SECTORS,
          "one sector past it is clamped");
    CHECK(diskinfo_window(1953525168ull) == 1953525168ull,
          "a 1 TB drive is reachable whole, which it was not while the offset was 32 bits");
    CHECK(diskinfo_window(8388608ull) == 8388608ull,
          "the old 4 GiB byte bound is gone: one sector past it is no longer clamped");
    CHECK(diskinfo_window(0x1FFFFFFFFull) == DISKINFO_WINDOW_MAX_SECTORS,
          "past the driver's 32-bit LBA the window stops at what an LBA can name");
    CHECK(DISKINFO_WINDOW_MAX_SECTORS == 0x100000000ull,
          "the cap is a sector count of 2^32, not the largest LBA, which would lose a sector");

    /* ---- the rendered file ---- */
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
                 "0\n") == 0,
          "a kernel that was never asked says so and stops there");

    d.state = DISKINFO_NO_DEVICE;
    diskinfo_format(&d, buf, (int)sizeof(buf));
    CHECK(strcmp(buf,
                 "state no-device\ndriver none\npolicy read-write\ndisk-commands 0\n"
                 "disk-flushes 0\ndisk-polls 0\ndisk-wait-ticks 0\ndisk-in-flight 0\ndisk-overlaps "
                 "0\ndisk-quarantines 0\ndisk-releases 0\n"
                 "disk-release-tick 0\nlag-completions 0\ncache-hits 0\ncache-fills 0\ncache-waits "
                 "0\n") == 0,
          "a kernel that asked and got nothing says a different thing");

    policy_now = DISKCMD_READ_ONLY;
    diskinfo_format(&d, buf, (int)sizeof(buf));
    CHECK(strcmp(buf,
                 "state no-device\ndriver none\npolicy read-only\ndisk-commands 0\n"
                 "disk-flushes 0\ndisk-polls 0\ndisk-wait-ticks 0\ndisk-in-flight 0\ndisk-overlaps "
                 "0\ndisk-quarantines 0\ndisk-releases 0\n"
                 "disk-release-tick 0\nlag-completions 0\ncache-hits 0\ncache-fills 0\ncache-waits "
                 "0\n") == 0,
          "the file names the build policy the kernel is running");
    policy_now = DISKCMD_READ_WRITE;

    d.state = DISKINFO_PRESENT;
    d.driver = "ahci";
    d.port = 0;
    d.mounted = 0;
    d.sectors = 1953525168ull;
    d.sector_size = 512;
    d.window_sectors = diskinfo_window(d.sectors);
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

    /* The exact text, because what reads it is awk and grep on the machine. */
    {
        static const char *want[] = {"state present\n",
                                     "driver ahci\n",
                                     "policy read-write\ndisk-commands 0\ndisk-flushes 0\n",
                                     "cache-hits 0\ncache-fills 0\ncache-waits 0\n",
                                     "port 0\n",
                                     "mounted 0\n",
                                     "sector-size 512\n",
                                     "sectors 1953525168\n",
                                     "window-sectors 1953525168\n",
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

    /* mbr-signature has three values and the file must distinguish them. */
    d.mbr_state = DISKINFO_MBR_NONE;
    diskinfo_format(&d, buf, (int)sizeof(buf));
    CHECK(buf[0] != '\0', "a table with no signature still renders");
    d.mbr_state = DISKINFO_MBR_UNREAD;
    diskinfo_format(&d, buf, (int)sizeof(buf));
    CHECK(buf[0] != '\0', "a first sector that could not be read still renders");
    d.mbr_state = DISKINFO_MBR_OK;

    /* A buffer one byte short of the text must truncate, terminate, and write
       nothing past its end. The sanitizers catch the overrun a wrong guard
       would cause; the sentinel catches a byte written exactly at the edge. */
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

    /* ---- the lines a program choosing a disk reads ----
     *
     * Two disks: a fixed one of 512-byte sectors with nothing mounted, and a
     * removable one of 4096-byte sectors with a mount holding it. Every field
     * of the `identity` line decides whether an installer offers the disk, so
     * each is compared as it is rendered rather than as a count of lines.
     */
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

    return fails;
}
