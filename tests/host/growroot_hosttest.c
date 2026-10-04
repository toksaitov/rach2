/**
 * Check the sector growroot validates and writes through its writer.
 *
 * host-deps: utils/gptdisk.c
 *
 * Device calls use memory; proc files use temporary host streams. The table
 * reread ioctl can stop after the write or publish the new size. Measured
 * runs fork real children; an exec substitute updates shared counters at
 * each simulated tool operation, independently of the measurement calls.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned char initial[512], current[512], written[512];
static int read_fault, writer_reads, writes, rereads, failures, opens;
static int complete;
static pid_t parent_pid;
static struct counters {
    unsigned long long us, commands, flushes;
    int tools, bad_args;
} *meter;

#define CHECK(condition, detail)                                  \
    do {                                                          \
        if (!(condition)) {                                       \
            fprintf(stderr, "growroot-test: fail: %s\n", detail); \
            failures++;                                           \
        }                                                         \
    } while (0)

/** Supply the inventory from before the writer opened the device. */
static FILE *fake_fopen(const char *path, const char *mode)
{
    FILE *f = tmpfile();

    CHECK(strcmp(mode, "r") == 0, "proc files are only read");
    if (!f) {
        perror("growroot-test: tmpfile");
        exit(2);
    }
    if (strcmp(path, "/proc/meminfo") == 0) {
        fputs("MemTotal: 65536 kB\n", f);
    } else if (strcmp(path, "/proc/disk") == 0) {
        fprintf(f,
                "disk 0 ahci 0 131072 mbr fixture\n"
                "part 0 1 2048 %u mbr:83\n"
                "disk-commands %llu\ndisk-flushes %llu\n",
                complete && rereads ? 129024u : 16384u, meter->commands, meter->flushes);
    } else {
        CHECK(0, "only the memory and disk inventories are opened");
    }
    rewind(f);
    return f;
}

/** Distinguish the discovery descriptors from the writer's descriptor. */
static int fake_open(const char *path, int flags, ...)
{
    opens++;
    if (strcmp(path, "/dev/disk0") == 0) {
        return flags == O_RDWR ? 12 : 10;
    }
    CHECK(strcmp(path, "/dev/disk0p1") == 0 && flags == O_RDONLY,
          "the ext2 magic is read from the selected partition");
    return 11;
}

/** Fail the writer's first read, then make a retry succeed if one occurs. */
static ssize_t fake_read(int fd, void *buffer, size_t size)
{
    unsigned char *bytes = buffer;

    CHECK(size == 512, "each device read requests one sector");
    if (fd == 11) {
        memset(bytes, 0, size);
        bytes[56] = 0x53;
        bytes[57] = 0xEF;
    } else if (fd == 10) {
        memcpy(bytes, initial, size);
    } else {
        CHECK(fd == 12, "only the writer reads the current table");
        writer_reads++;
        if (writer_reads == 1 && read_fault == 1) {
            errno = EIO;
            return -1;
        }
        memcpy(bytes, current, size);
        if (writer_reads == 1 && read_fault == 2) {
            return 511;
        }
    }
    return (ssize_t)size;
}

/** Capture the only device write, without opening a host device. */
static ssize_t fake_write(int fd, const void *buffer, size_t size)
{
    CHECK(fd == 12 && size == 512, "only a whole MBR sector is written");
    writes++;
    meter->commands += 3;
    meter->flushes++;
    meter->us += 20000;
    memcpy(written, buffer, size);
    return (ssize_t)size;
}

/** Accept the magic read's offset and rewinds of the table descriptor. */
static off_t fake_lseek(int fd, off_t offset, int whence)
{
    CHECK(whence == SEEK_SET && ((fd == 11 && offset == 1024) || (fd == 12 && offset == 0)),
          "device seeks use the superblock or the first sector");
    return offset;
}

/** Publish the larger partition, or simulate a busy disk for table cases. */
static int fake_ioctl(int fd, unsigned long request, ...)
{
    CHECK(fd == 12 && request == 0x125Fu, "the kernel is asked to reread the table");
    rereads++;
    meter->commands += 2;
    meter->us += 10000;
    if (complete) {
        return 0;
    }
    errno = EBUSY;
    return -1;
}

/** Count releases of the three substituted descriptors. */
static int fake_close(int fd)
{
    CHECK(fd >= 10 && fd <= 12, "only substituted descriptors are closed");
    opens--;
    return 0;
}

/** Time advances only when a simulated operation does work. */
static int fake_clock_gettime(clockid_t id, struct timespec *t)
{
    CHECK(id == CLOCK_MONOTONIC, "measurement uses the monotonic clock");
    t->tv_sec = (time_t)(meter->us / 1000000);
    t->tv_nsec = (long)(meter->us % 1000000) * 1000;
    return 0;
}

/** Real children simulate each tool; waitpid must collect their updates. */
static int fake_execve(const char *path, char *const args[], char *const env[])
{
    int n = meter->tools;
    int ok = getpid() != parent_pid && env != NULL;

    if (n < 2) {
        ok = ok && strcmp(path, "/sbin/e2fsck") == 0 && strcmp(args[0], "e2fsck") == 0 &&
             strcmp(args[1], n ? "-fp" : "-fn") == 0 && strcmp(args[2], "/dev/disk0p1") == 0 &&
             args[3] == NULL;
    } else {
        ok = ok && n == 2 && strcmp(path, "/sbin/resize2fs") == 0 &&
             strcmp(args[0], "resize2fs") == 0 && strcmp(args[1], "/dev/disk0p1") == 0 &&
             args[2] == NULL;
    }
    if (!ok) {
        meter->bad_args++;
        _exit(125);
    }
    meter->commands += n == 0 ? 7 : n == 1 ? 11 : 17;
    meter->flushes += n == 0 ? 0 : n == 1 ? 2 : 3;
    meter->us += n == 0 ? 40000 : n == 1 ? 50000 : 60000;
    meter->tools++;
    _exit(0);
}

#define clock_gettime fake_clock_gettime
#define execve        fake_execve
#define fopen         fake_fopen
#define open          fake_open
#define read          fake_read
#define write         fake_write
#define lseek         fake_lseek
#define ioctl         fake_ioctl
#define close         fake_close
#define main          growroot_main
#include "growroot.c"
#undef main
#undef close
#undef ioctl
#undef lseek
#undef write
#undef read
#undef open
#undef fopen
#undef execve
#undef clock_gettime

/** Write the fixture's little-endian fields independently of growroot. */
static void put32(unsigned char *p, unsigned long value)
{
    int i;

    for (i = 0; i < 4; i++) {
        p[i] = (unsigned char)(value >> (i * 8));
    }
}

/** Reset discovery and device state for an 8 MiB partition on 64 MiB. */
static void fixture(void)
{
    memset(initial, 0, sizeof(initial));
    memset(initial, 0x5A, 440);
    put32(initial + 440, 1);
    initial[446 + 4] = 0x83;
    put32(initial + 446 + 8, 2048);
    put32(initial + 446 + 12, 16384);
    initial[510] = 0x55;
    initial[511] = 0xAA;
    memcpy(current, initial, sizeof(current));
    memset(written, 0, sizeof(written));
    read_fault = writer_reads = writes = rereads = opens = complete = 0;
    memset(meter, 0, sizeof(*meter));
    meter->us = 1000000;
    meter->commands = 100;
    meter->flushes = 20;
    nparts = ndisks = 0;
    memset(parts, 0, sizeof(parts));
    memset(disk_sectors, 0, sizeof(disk_sectors));
    memset(disk_table, 0, sizeof(disk_table));
}

/** Run the real decision and require every descriptor to be released. */
static void invoke(void)
{
    char *args[] = {"growroot", "MBRID=1,1", NULL};

    CHECK(growroot_main(2, args) == 1, "validation or the simulated busy disk stops the run");
    CHECK(opens == 0, "all device descriptors close");
}

/** A changed table must not reach either the write or the table reread. */
static void refused(const char *detail)
{
    invoke();
    CHECK(writes == 0 && rereads == 0, detail);
}

/** Compare every reported edge with work done outside measured(). */
static void measured_run(void)
{
    static const unsigned long long times[] = {1000000, 1030000, 1030000, 1070000,
                                               1070000, 1120000, 1120000, 1180000};
    static const unsigned commands[] = {100, 105, 105, 112, 112, 123, 123, 140};
    static const unsigned flushes[] = {20, 21, 21, 21, 21, 23, 23, 26};
    static const char *phases[] = {"table", "check-readonly", "check-repair", "resize"};
    char *args[] = {"growroot", "MBRID=1,1", "--measure", NULL};
    char line[512], phase[32], edge[16], extra;
    unsigned long long us, cmd, flush;
    FILE *output = tmpfile();
    int saved, rc, n = 0;

    if (!output || fflush(stdout) || (saved = dup(STDOUT_FILENO)) < 0 ||
        dup2(fileno(output), STDOUT_FILENO) < 0) {
        perror("growroot-test: capture");
        exit(2);
    }
    fixture();
    complete = 1;
    rc = growroot_main(3, args);
    fflush(stdout);
    if (dup2(saved, STDOUT_FILENO) < 0) {
        perror("growroot-test: restore stdout");
        exit(2);
    }
    close(saved);
    CHECK(rc == 0 && meter->tools == 3 && meter->bad_args == 0 && opens == 0,
          "the measured run completes table publication and all three children");
    rewind(output);
    while (fgets(line, sizeof(line), output)) {
        if (strncmp(line, "growroot: measure:", 18) != 0) {
            continue;
        }
        rc = sscanf(line,
                    "growroot: measure: %31s %15s monotonic-us %llu "
                    "disk-commands %llu disk-flushes %llu %c",
                    phase, edge, &us, &cmd, &flush, &extra);
        CHECK(rc == 5 && n < 8, "eight complete measurement records");
        if (rc == 5 && n < 8) {
            CHECK(strcmp(phase, phases[n / 2]) == 0 &&
                      strcmp(edge, n % 2 ? "after" : "before") == 0,
                  "measurement edges follow the operation order");
            CHECK(us == times[n] && cmd == commands[n] && flush == flushes[n],
                  "phase samples match the independent operation counters");
        }
        n++;
    }
    CHECK(n == 8, "all four phases are measured");
    fclose(output);
}

/** Put entry `n` of `t` over sectors `first` to `last`, used, or clear it. */
static void gpt_entry(struct gptdisk *t, int n, unsigned long long first, unsigned long long last)
{
    unsigned char *e = gptdisk_entry(t, n);

    memset(e, 0, 128);
    if (last >= first) {
        memset(e, 0xAF, 16);
        gptdisk_put64(e + 32, first);
        gptdisk_put64(e + 40, last);
    }
}

/**
 * Bound a GUID table's root, entry 3 over sectors 2048 to 18431 of 131072,
 * by the entries beside it: one starting in the root and ending past it lies
 * in the growth and stops the grow; one after it bounds the growth; with
 * neither, the growth reaches the backup array at the disk's end.
 */
static void gpt_bounds(void)
{
    static struct gptdisk t;
    unsigned long long limit = 0;

    memset(&t, 0, sizeof(t));
    t.sectors = 131072;
    t.entries = 128;
    t.entry_size = 128;
    gpt_entry(&t, 1, 34, 2047);
    gpt_entry(&t, 3, 2048, 18431);
    gpt_entry(&t, 4, 18000, 32767);
    CHECK(gpt_limit(&t, 3, 18432, &limit) == 4,
          "an entry from inside the root into the space after it stops the grow");
    gpt_entry(&t, 4, 18432, 32767);
    CHECK(gpt_limit(&t, 3, 18432, &limit) == 0 && limit == 18432,
          "an entry right after the root bounds the growth");
    gpt_entry(&t, 4, 1, 0);
    CHECK(gpt_limit(&t, 3, 18432, &limit) == 0 && limit == 131072 - 1 - 32,
          "with none after it, the growth reaches the backup array");
}

/** Cover successful publication, read failures and each checked table field. */
int main(void)
{
    unsigned char expected[512];
    int fault;
    FILE *shared = tmpfile();

    if (!shared || ftruncate(fileno(shared), sizeof(*meter)) != 0) {
        perror("growroot-test: shared counters");
        return 2;
    }
    meter = mmap(NULL, sizeof(*meter), PROT_READ | PROT_WRITE, MAP_SHARED, fileno(shared), 0);
    if (meter == MAP_FAILED) {
        perror("growroot-test: mmap");
        return 2;
    }
    parent_pid = getpid();

    fixture();
    memcpy(expected, initial, sizeof(expected));
    expected[446 + 5] = 0xFE;
    expected[446 + 6] = expected[446 + 7] = 0xFF;
    put32(expected + 446 + 12, 129024);
    invoke();
    CHECK(writer_reads == 1, "the writer uses the sector it checked without rereading");
    CHECK(writes == 1 && rereads == 1, "a valid table is written and reread once");
    CHECK(memcmp(written, expected, sizeof(expected)) == 0,
          "only the target length and ending CHS bytes change");
    for (fault = 1; fault <= 2; fault++) {
        fixture();
        read_fault = fault;
        refused("a failed or short writer read makes no write");
    }
    fixture();
    current[440] ^= 1;
    refused("a changed disk identity makes no write");
    fixture();
    current[510] = 0;
    refused("an invalid MBR signature makes no write");
    fixture();
    current[446 + 4] = 0x0C;
    refused("a changed partition type makes no write");
    fixture();
    put32(current + 446 + 8, 4096);
    refused("a changed partition start makes no write");
    fixture();
    put32(current + 446 + 12, 32768);
    refused("a changed partition length makes no write");
    fixture();
    current[462 + 4] = 0x83;
    put32(current + 462 + 8, 20000);
    put32(current + 462 + 12, 4096);
    refused("a new partition in the grow space makes no write");
    fixture();
    current[462 + 4] = 0x0F;
    put32(current + 462 + 8, 20000);
    put32(current + 462 + 12, 4096);
    refused("an extended partition in the grow space makes no write");
    measured_run();
    gpt_bounds();
    munmap(meter, sizeof(*meter));
    fclose(shared);
    if (failures) {
        return 1;
    }
    puts("growroot-test: result: ten table cases, three GUID table bounds and four measured "
         "phases passed");
    return 0;
}
