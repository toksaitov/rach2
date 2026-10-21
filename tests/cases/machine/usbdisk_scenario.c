/*
 * usbdisk_scenario.c: the scenarios of tests/cases/machine.usbdisk.json, a
 * flash drive as a disk from its table to its removal, each case a boot of
 * its own.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbcase.h"

/* The family's cases begin with this, and their scenarios are named by the
 * rest. */
#define FAMILY "machine.usbdisk."

/* The machine every case boots, and the token of the probe it runs. */
#define MACHINE "usbdisk"
#define PROBE   "USBDISKPROBE"

/* The longest path a scenario passes to a host tool, and a line it judges. */
#define PATH_MAX_LEN 4096
#define LINE_MAX_LEN 160

/* The far partition of the stick past two terabytes: its first sector and
 * its sectors, as tests/system/usbdisk_sticks.sh makes its volume. */
#define FAR_START   "4294969344"
#define FAR_SECTORS "65536"

/* The raw writes of the recovery kernel: the 4 KiB pattern at sector 1000,
 * and 128 pieces of 32 KiB from byte 5 MiB, each a header `PIECE-NNN-` of
 * its index and the first 32758 bytes of the base pattern. */
#define RAW_OFFSET  (1000L * 512)
#define RAW_BYTES   4096L
#define BIG_OFFSET  (160L * 32768)
#define BIG_PIECES  128
#define PIECE_BYTES 32768L
#define BASE_BYTES  32758L

/* The lines typed at the recovery shell. Each echo splits its token with
 * "" so that the typed line does not have it. */
#define COUNT_COMMANDS "$(grep \"^disk-commands \" /proc/disk | cut -d\" \" -f2)"
#define RAW_LINE                                                                               \
    "yes RACH2RAW | head -c 4096 > /tmp/p; dd if=/tmp/p of=/dev/disk0 bs=512 seek=1000 2>&1; " \
    "dd if=/dev/disk0 bs=512 skip=1000 count=8 2>/dev/null | cksum; cksum < /tmp/p; "          \
    "echo RAW\"\"DONE"
#define BIG_START_LINE                                                                      \
    "yes RACH2-BIG-PATTERN-LINE | head -c 32768 > /tmp/base; p() { printf PIECE-%03d- $1; " \
    "dd if=/tmp/base bs=32758 count=1 2>/dev/null; }; c0=" COUNT_COMMANDS "; echo BIG\"\"START"
#define BIG_WRITE_LINE                                                                    \
    "i=0; while [ $i -lt 128 ]; do p $i > /tmp/blk; dd if=/tmp/blk of=/dev/disk0 bs=32k " \
    "seek=$((160 + i)) count=1 2>/dev/null; i=$((i + 1)); done; sync; c1=" COUNT_COMMANDS \
    "; echo BIG\"\"WROTE"
#define BIG_READ_LINE                                                                    \
    "dd if=/dev/disk0 bs=32k skip=160 count=128 2>/dev/null | cksum; c2=" COUNT_COMMANDS \
    "; i=0; while [ $i -lt 128 ]; do p $i; i=$((i + 1)); done | cksum; "                 \
    "echo BIGCMDS $((c1-c0)) $((c2-c1)); echo BIG\"\"DONE"

/* A POSIX cksum in progress: the CRC so far and the bytes it covers. */
struct cksum {
    uint32_t crc;
    uint64_t length;
};

/** Add byte `b` to the CRC `crc` of the POSIX cksum polynomial. */
static uint32_t crc_byte(uint32_t crc, unsigned char b)
{
    int i;

    crc ^= (uint32_t)b << 24;
    for (i = 0; i < 8; i++)
        crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    return crc;
}

/** Add the `n` bytes at `p` to the cksum `k`. */
static void cksum_add(struct cksum *k, const void *p, size_t n)
{
    const unsigned char *s = p;
    size_t i;

    for (i = 0; i < n; i++)
        k->crc = crc_byte(k->crc, s[i]);
    k->length += n;
}

/** Give the value cksum(1) prints for the bytes `k` covers: the CRC of the
 * bytes and then of their length, least significant byte first, inverted. */
static uint32_t cksum_end(const struct cksum *k)
{
    uint32_t crc = k->crc;
    uint64_t n;

    for (n = k->length; n != 0; n >>= 8)
        crc = crc_byte(crc, (unsigned char)(n & 0xFF));
    return ~crc;
}

/** Fill `buf` of `n` bytes with `line` repeated, as `yes` and `head -c`
 * write it; `line` ends with its newline. */
static void repeat(char *buf, size_t n, const char *line)
{
    size_t len = strlen(line), i;

    for (i = 0; i < n; i++)
        buf[i] = line[i % len];
}

/** Give the cksum of the recovery case's 4 KiB pattern. */
static uint32_t raw_pattern(void)
{
    char buf[RAW_BYTES];
    struct cksum k = {0, 0};

    repeat(buf, sizeof(buf), "RACH2RAW\n");
    cksum_add(&k, buf, sizeof(buf));
    return cksum_end(&k);
}

/** Give the cksum of the recovery case's 128 pieces: each `PIECE-NNN-` and
 * the first 32758 bytes of the base pattern. */
static uint32_t big_pattern(void)
{
    static char base[PIECE_BYTES];
    struct cksum k = {0, 0};
    char head[16];
    int i;

    repeat(base, sizeof(base), "RACH2-BIG-PATTERN-LINE\n");
    for (i = 0; i < BIG_PIECES; i++) {
        snprintf(head, sizeof(head), "PIECE-%03d-", i);
        cksum_add(&k, head, strlen(head));
        cksum_add(&k, base, BASE_BYTES);
    }
    return cksum_end(&k);
}

/**
 * Give in `*sum` the cksum of the `n` bytes at `offset` of the file at
 * `path`; -1 when they cannot all be read. The offsets stay below 2^31 so
 * that stdio's long reaches them on every host.
 */
static int file_cksum(const char *path, long offset, long n, uint32_t *sum)
{
    struct cksum k = {0, 0};
    char buf[8192];
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    size_t got;

    if (f == NULL)
        return -1;
    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    while (n > 0 && (got = fread(buf, 1, n < (long)sizeof(buf) ? (size_t)n : sizeof(buf), f)) > 0) {
        cksum_add(&k, buf, got);
        n -= (long)got;
    }
    fclose(f);
    *sum = cksum_end(&k);
    return n == 0 ? 0 : -1;
}

/** Give the size of the file at `path` in bytes, or -1. */
static long file_size(const char *path)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    long size = -1;

    if (f == NULL)
        return -1;
    if (fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    fclose(f);
    return size;
}

/** Give the next line of `text` after the one at `p`, or NULL at the end. */
static const char *next_line(const char *p)
{
    p += strcspn(p, "\r\n");
    while (*p == '\r' || *p == '\n')
        p++;
    return *p != '\0' ? p : NULL;
}

/** Count the lines of `text` that begin with `prefix`, and give the last of
 * them in `*last` when `last` is not NULL. */
static unsigned count_begins(const char *text, const char *prefix, const char **last)
{
    const char *p = text != NULL && *text != '\0' ? text : NULL;
    size_t n = strlen(prefix);
    unsigned count = 0;

    if (last != NULL)
        *last = NULL;
    for (; p != NULL; p = next_line(p)) {
        if (strncmp(p, prefix, n) == 0) {
            count++;
            if (last != NULL)
                *last = p;
        }
    }
    return count;
}

/** Copy `path` into `buf` of PATH_MAX_LEN bytes, failing `u` with `what`
 * when there is none or it does not fit; give `buf`. */
static char *keep_path(struct usb_case *u, char *buf, const char *path, const char *what)
{
    buf[0] = '\0';
    if (path == NULL || strlen(path) >= PATH_MAX_LEN)
        usb_fail(u, "no path for %s", what);
    else
        strcpy(buf, path);
    return buf;
}

/** Boot the case's machine with all its disks in step `boot`, and wait for
 * `greeting`. */
static void boot(struct usb_case *u, struct usb_boot *b, const char *greeting)
{
    usb_step(u, "boot");
    usb_boot(b, u, MACHINE, 1, NULL);
    usb_wait(b, greeting);
}

/** Run `command` at the debug shell in step `step`, and report it as the
 * probe's assertion `id`. */
static void probe(struct usb_case *u, struct usb_boot *b, const char *step, const char *command,
                  const char *id)
{
    struct usb_run r;

    usb_step(u, step);
    usb_exec(b, command, &r);
    usb_probe(u, id, &r, PROBE);
    free(r.text);
}

/**
 * Run `command` in step `step`, pull device `device` out once the probe
 * prints `USBDISKPROBE HOLDING`, and report the run as assertion `id`. The
 * wait is for the probe's first line with its token, so that a probe that
 * fails before it holds the stick ends the wait at once, and the stick is
 * then not pulled.
 */
static void pulled(struct usb_case *u, struct usb_boot *b, const char *step, const char *command,
                   const char *device, const char *id)
{
    char line[LINE_MAX_LEN];
    struct usb_run r;
    uint64_t from, mark;
    char *text;
    int holding;

    usb_step(u, step);
    from = usb_start(b, command);
    usb_wait(b, PROBE " ");
    mark = b->at;
    usb_wait(b, "\n");
    text = usb_text(b, mark, b->at);
    usb_copy_line(text, line, sizeof(line));
    holding = strcmp(line, "HOLDING") == 0;
    free(text);
    if (holding)
        usb_pull(b, device);
    usb_finish(b, from, &r);
    if (!holding && r.text != NULL && usb_find(r.text, PROBE " FAIL") == NULL)
        usb_assert(u, id, 0, "USBDISKPROBE %s before USBDISKPROBE HOLDING", line);
    else
        usb_probe(u, id, &r, PROBE);
    free(r.text);
}

/** Run `usbdiskprobe free` in step `step`, and report as assertion `id`
 * that it printed `USBDISKPROBE FREE 8` and passed. */
static void free_names(struct usb_case *u, struct usb_boot *b, const char *step, const char *id)
{
    char line[LINE_MAX_LEN];
    const char *count;
    struct usb_run r;

    usb_step(u, step);
    usb_exec(b, "/usbdiskprobe free", &r);
    usb_copy_line(r.text != NULL ? usb_find(r.text, PROBE " FREE #") : NULL, line, sizeof(line));
    count = strstr(line, PROBE " FREE ");
    if (r.text != NULL && (count == NULL || strcmp(count + strlen(PROBE " FREE "), "8") != 0))
        usb_assert(u, id, 0, "no USBDISKPROBE FREE 8: %s", line[0] != '\0' ? line : "no count");
    else
        usb_probe(u, id, &r, PROBE);
    free(r.text);
}

/** Report assertion `no-fail`: no `USBDISKPROBE FAIL` line on the console
 * up to the offset read. */
static void no_fail(struct usb_case *u, struct usb_boot *b)
{
    char *text = usb_text(b, 0, b->at);
    char line[LINE_MAX_LEN];
    const char *f = usb_find(text, PROBE " FAIL");

    usb_assert(u, "no-fail", f == NULL, "%s", usb_copy_line(f, line, sizeof(line)));
    free(text);
}

/** Wait in step `record` for the kernel's record `record` anywhere on the
 * console, and report it as assertion `id`. */
static void recorded(struct usb_case *u, struct usb_boot *b, const char *record, const char *id)
{
    usb_step(u, "record");
    usb_seen(b, record);
    usb_assert(u, id, 1, "");
}

/** Log in as root at the login prompt the boot waited for, with the demo
 * password, in step `login`, and wait for the shell's prompt. */
static void login(struct usb_case *u, struct usb_boot *b)
{
    char path[PATH_MAX_LEN];
    char *password;

    usb_step(u, "login");
    keep_path(u, path, etude_artifact(u->c, "demo_password"), "the password");
    if (!usb_ok(u))
        return;
    password = usb_file(path);
    password[strcspn(password, "\r\n")] = '\0';
    if (password[0] == '\0')
        usb_fail(u, "the password file is empty");
    usb_line(b, "root");
    usb_wait(b, "Password:");
    usb_line(b, password);
    usb_wait(b, "# ");
    free(password);
}

/** Type `line` at the shell and wait for `token`, which its output ends
 * with. */
static void shell(struct usb_boot *b, const char *line, const char *token)
{
    usb_line(b, line);
    usb_wait(b, token);
}

/** The MBR stick and the GUID stick: `report`, then `volumes` with the
 * nodes the table gives. */
static int tabled(struct usb_case *u, const char *volumes)
{
    struct usb_boot b;

    boot(u, &b, USB_SHELL);
    probe(u, &b, "report", "/usbdiskprobe report", "report");
    probe(u, &b, "volumes", volumes, "volumes");
    no_fail(u, &b);
    return usb_end(u);
}

/** An MBR stick is named and sized, and both its volumes mount. */
static int mbr(struct usb_case *u)
{
    return tabled(u, "/usbdiskprobe volumes");
}

/** A GUID stick's volumes are p7 and p10, its entries' own numbers. */
static int gpt(struct usb_case *u)
{
    return tabled(u, "/usbdiskprobe volumes /dev/disk0p7 /dev/disk0p10");
}

/** A stick with no table mounts through its whole node, and the kernel
 * records it as one volume. */
static int superfloppy(struct usb_case *u)
{
    struct usb_boot b;

    boot(u, &b, USB_SHELL);
    probe(u, &b, "probe", "/usbdiskprobe super", "super");
    no_fail(u, &b);
    recorded(u, &b, "usbdisk: disk0: one volume", "one-volume");
    return usb_end(u);
}

/** A file written on the machine comes back after a remount, and mtools
 * reads it off the stick's copy on the host. */
static int written(struct usb_case *u)
{
    char image[PATH_MAX_LEN], spec[PATH_MAX_LEN + 16], out[PATH_MAX_LEN];
    char *argv[5];
    struct usb_boot b;
    char *text;

    boot(u, &b, USB_SHELL);
    probe(u, &b, "probe", "/usbdiskprobe write", "write");
    no_fail(u, &b);
    usb_step(u, "read");
    keep_path(u, image, etude_private_copy(u->c, "stick"), "the stick's copy");
    snprintf(spec, sizeof(spec), "%s@@1048576", image);
    argv[0] = "mtype";
    argv[1] = "-i";
    argv[2] = spec;
    argv[3] = "::/written.txt";
    argv[4] = NULL;
    usb_host(u, argv, "written.txt");
    keep_path(u, out, etude_evidence(u->c, "written.txt"), "mtype's output");
    text = usb_ok(u) ? usb_file(out) : NULL;
    usb_assert(u, "host-read", text != NULL && strstr(text, "a stick takes a write") != NULL,
               "written.txt does not have the text the probe wrote");
    free(text);
    return usb_end(u);
}

/** The stick pulled while the probe part `command` holds a file open, then
 * plugged in again from the same copy. */
static int held(struct usb_case *u, const char *command)
{
    char image[PATH_MAX_LEN];
    struct usb_boot b;
    struct usb_run r;

    boot(u, &b, USB_SHELL);
    pulled(u, &b, "hold", command, "st0", "hold");
    free_names(u, &b, "free", "free");
    usb_step(u, "replug");
    keep_path(u, image, etude_private_copy(u->c, "stick"), "the stick's copy");
    usb_medium(&b, "again", image, 0);
    usb_plug(&b, "usb-storage", "xhci.0", "st1", "\"drive\": \"again\"");
    usb_wait(&b, "usbdisk: disk0: mbr");
    usb_exec(&b, "/usbdiskprobe replug", &r);
    usb_probe(u, "replug", &r, PROBE);
    free(r.text);
    no_fail(u, &b);
    recorded(u, &b, "usbdisk: disk0: reclaimed", "reclaimed");
    return usb_end(u);
}

/** The pull with the FAT32 volume held and the exFAT one below it. */
static int holdnested(struct usb_case *u)
{
    return held(u, "/usbdiskprobe holdnested");
}

/** The pull with the exFAT volume held and the FAT32 one below it. */
static int holdnestedexfat(struct usb_case *u)
{
    return held(u, "/usbdiskprobe holdnestedexfat");
}

/**
 * Ten sticks plugged in and pulled out unopened. The medium is one
 * read-only node of the MBR stick's artifact, made once: a node made by
 * blockdev-add stays when the device on it is pulled, so each plug is a new
 * device on the same node.
 */
static int churn(struct usb_case *u)
{
    char image[PATH_MAX_LEN], id[16];
    struct usb_boot b;
    char *text;
    unsigned n;
    int i;

    boot(u, &b, USB_SHELL);
    usb_step(u, "churn");
    keep_path(u, image, etude_artifact(u->c, "stick"), "the stick");
    usb_medium(&b, "medium", image, 1);
    for (i = 0; i < 10; i++) {
        snprintf(id, sizeof(id), "st%d", i);
        usb_plug(&b, "usb-storage", "xhci.0", id, "\"drive\": \"medium\"");
        usb_wait(&b, "usbdisk: disk0: mbr");
        usb_pull(&b, id);
        usb_wait(&b, "usbdisk: disk0: reclaimed");
    }
    usb_assert(u, "churn", 1, "");
    free_names(u, &b, "free", "free");
    text = usb_text(&b, 0, b.at);
    n = usb_count(text, "usbdisk: disk0: reclaimed");
    usb_assert(u, "reclaimed-ten", n == 10, "%u records of a reclaimed disk0, not 10", n);
    free(text);
    no_fail(u, &b);
    return usb_end(u);
}

/** Report assertion `id`: a line of `text` begins with `prefix` and then
 * `node`, as the line `echo PREFIX$(findfs ...)` prints. */
static void named(struct usb_case *u, const char *id, const char *text, const char *prefix,
                  const char *node)
{
    char want[LINE_MAX_LEN], line[LINE_MAX_LEN];
    const char *last;

    snprintf(want, sizeof(want), "%s%s", prefix, node);
    count_begins(text, prefix, &last);
    usb_assert(u, id, count_begins(text, want, NULL) > 0, "%s",
               last != NULL ? usb_copy_line(last, line, sizeof(line)) : "findfs printed nothing");
}

/** BusyBox on the os-real image finds the volumes by their labels, mounts
 * one, copies a file off it and unmounts it. */
static int busybox(struct usb_case *u)
{
    struct usb_boot b;
    const char *p;
    uint64_t from, to;
    char *text, *copy;

    boot(u, &b, "rach2 login:");
    login(u, &b);
    usb_step(u, "commands");
    shell(&b, "blkid; echo BLK\"\"DONE", "BLKDONE");
    shell(&b,
          "echo FOUND1=$(findfs LABEL=STICKFAT); echo FOUND2=$(findfs LABEL=FIXTURE); "
          "echo FIND\"\"DONE",
          "FINDDONE");
    shell(&b, "mkdir -p /mnt && mount -t vfat LABEL=STICKFAT /mnt && echo MOUNT\"\"ED", "MOUNTED");
    from = b.at;
    shell(&b, "cp /mnt/hello.txt /tmp/h && cat /tmp/h; echo COPY\"\"DONE", "COPYDONE");
    to = b.at;
    shell(&b, "umount /mnt && echo BB\"\"DONE", "BBDONE");
    usb_assert(u, "busybox", 1, "");
    text = usb_text(&b, 0, b.at);
    named(u, "findfs-fat", text, "FOUND1=", "/dev/disk0p1");
    named(u, "findfs-exfat", text, "FOUND2=", "/dev/disk0p2");
    for (p = text; p != NULL && *p != '\0'; p = next_line(p)) {
        if (strncmp(p, "/dev/disk0p1:", 13) == 0 && usb_match(p, strcspn(p, "\r\n"), "STICKFAT"))
            break;
    }
    usb_assert(u, "blkid-label", p != NULL && *p != '\0', "blkid gave /dev/disk0p1 no STICKFAT");
    copy = usb_text(&b, from, to);
    usb_assert(u, "copied", strstr(copy, "hello from the stick") != NULL,
               "cat did not print the stick's text");
    free(copy);
    free(text);
    return usb_end(u);
}

/** A 16 MiB read of the stick, its rate printed for a person and not
 * judged, needs no recovery of a command. */
static int throughput(struct usb_case *u)
{
    char line[LINE_MAX_LEN];
    struct usb_boot b;
    struct usb_run r;
    char *text;
    unsigned n;

    boot(u, &b, USB_SHELL);
    usb_step(u, "probe");
    usb_exec(&b, "/usbdiskprobe throughput", &r);
    if (r.text != NULL && usb_find(r.text, PROBE " THROUGHPUT") != NULL) {
        printf("usbdisk_scenario: %s\n",
               usb_copy_line(usb_find(r.text, PROBE " THROUGHPUT"), line, sizeof(line)));
        fflush(stdout);
    }
    usb_probe(u, "throughput", &r, PROBE);
    free(r.text);
    text = usb_text(&b, 0, b.at);
    n = usb_count(text, "usbdisk: recovery");
    usb_assert(u, "no-recovery", n == 0, "%s",
               usb_copy_line(usb_find(text, "usbdisk: recovery"), line, sizeof(line)));
    free(text);
    no_fail(u, &b);
    return usb_end(u);
}

/** The stick image booted by the firmware is the machine's disk0, and its
 * second partition mounts. */
static int stickboot(struct usb_case *u)
{
    char image[PATH_MAX_LEN], record[LINE_MAX_LEN];
    struct usb_boot b;
    char *text;
    long size;

    boot(u, &b, "rach2 login:");
    login(u, &b);
    usb_step(u, "commands");
    shell(&b, "cat /proc/usb | grep ^disk0; echo USB\"\"DONE", "USBDONE");
    shell(&b, "mkdir -p /mnt && mount -t exfat /dev/disk0p2 /mnt && echo MOUNT\"\"ED", "MOUNTED");
    shell(&b, "ls /mnt | head -3; grep exfat /proc/mounts; echo LIST\"\"DONE", "LISTDONE");
    shell(&b, "umount /mnt && echo STICK\"\"DONE", "STICKDONE");
    usb_assert(u, "mounted", 1, "");
    keep_path(u, image, etude_artifact(u->c, "stick"), "the stick image");
    size = usb_ok(u) ? file_size(image) : -1;
    if (usb_ok(u) && size < 0)
        usb_fail(u, "the stick image's size does not read");
    snprintf(record, sizeof(record), "usbdisk: disk0: mbr: %ld MiB, 3 partitions",
             size / 512 / 2048);
    recorded(u, &b, record, "own-stick");
    text = usb_text(&b, 0, b.at);
    usb_assert(u, "proc-mounts", strstr(text, "/dev/disk0p2 /mnt exfat") != NULL,
               "/proc/mounts did not name /dev/disk0p2 on /mnt");
    free(text);
    return usb_end(u);
}

/** Three sticks at once, then the middle one pulled under a reader. */
static int three(struct usb_case *u)
{
    struct usb_boot b;

    boot(u, &b, USB_SHELL);
    probe(u, &b, "three", "/usbdiskprobe three", "three");
    pulled(u, &b, "pullone", "/usbdiskprobe pullone", "st1", "pullone");
    no_fail(u, &b);
    recorded(u, &b, "usbdisk: disk1: reclaimed", "reclaimed");
    return usb_end(u);
}

/** Report assertion `id`: at least two lines of `text` begin with `sum` and
 * a space. */
static void sums(struct usb_case *u, const char *id, const char *text, uint32_t sum)
{
    char prefix[16];
    unsigned n;

    snprintf(prefix, sizeof(prefix), "%lu ", (unsigned long)sum);
    n = count_begins(text, prefix, NULL);
    usb_assert(u, id, n >= 2, "%u lines begin with the cksum %lu, not 2", n, (unsigned long)sum);
}

/** Report assertion `id`: the `n` bytes at `offset` of the stick's copy have
 * the cksum `sum`. */
static void on_copy(struct usb_case *u, const char *id, const char *image, long offset, long n,
                    uint32_t sum)
{
    uint32_t got = 0;

    if (usb_ok(u) && file_cksum(image, offset, n, &got) != 0)
        usb_assert(u, id, 0, "the copy did not read at byte %ld", offset);
    else
        usb_assert(u, id, got == sum, "byte %ld: cksum %lu, not %lu", offset, (unsigned long)got,
                   (unsigned long)sum);
}

/** The recovery kernel writes the stick raw, and the machine and the host
 * read the patterns back. */
static int recovery(struct usb_case *u)
{
    uint32_t raw = raw_pattern(), big = big_pattern();
    char image[PATH_MAX_LEN], line[LINE_MAX_LEN];
    const char *last;
    struct usb_boot b;
    char *text;

    boot(u, &b, "recovery image");
    usb_wait(&b, "~ #");
    usb_step(u, "raw");
    shell(&b, RAW_LINE, "RAWDONE");
    usb_assert(u, "raw", 1, "");
    text = usb_text(&b, 0, b.at);
    sums(u, "raw-machine", text, raw);
    free(text);
    usb_step(u, "big");
    shell(&b, BIG_START_LINE, "BIGSTART");
    shell(&b, BIG_WRITE_LINE, "BIGWROTE");
    shell(&b, BIG_READ_LINE, "BIGDONE");
    usb_assert(u, "big", 1, "");
    text = usb_text(&b, 0, b.at);
    sums(u, "big-machine", text, big);
    count_begins(text, "BIGCMDS ", &last);
    usb_copy_line(last, line, sizeof(line));
    usb_assert(u, "big-commands", strcmp(line, "BIGCMDS 128 128") == 0, "%s",
               line[0] != '\0' ? line : "no BIGCMDS line");
    free(text);
    usb_step(u, "host");
    keep_path(u, image, etude_private_copy(u->c, "stick"), "the stick's copy");
    on_copy(u, "raw-host", image, RAW_OFFSET, RAW_BYTES, raw);
    on_copy(u, "big-host", image, BIG_OFFSET, BIG_PIECES * PIECE_BYTES, big);
    return usb_end(u);
}

/** A stick past two terabytes, its partition past sector 2^32 written on
 * the machine and read back on the host. */
static int far(struct usb_case *u)
{
    char image[PATH_MAX_LEN], volume[PATH_MAX_LEN], out[PATH_MAX_LEN], txt[PATH_MAX_LEN];
    char in_arg[PATH_MAX_LEN + 8], of_arg[PATH_MAX_LEN + 8];
    char *argv[8];
    struct usb_boot b;
    char *text;

    usb_step(u, "layout");
    keep_path(u, image, etude_private_copy(u->c, "big"), "the empty stick");
    keep_path(u, volume, etude_artifact(u->c, "far"), "the far volume");
    argv[0] = "mkgpt";
    argv[1] = image;
    argv[2] = FAR_START ":" FAR_SECTORS ":fat=FARAWAY";
    argv[3] = NULL;
    usb_host(u, argv, "mkgpt.out");
    snprintf(in_arg, sizeof(in_arg), "if=%s", volume);
    snprintf(of_arg, sizeof(of_arg), "of=%s", image);
    argv[0] = "dd";
    argv[1] = in_arg;
    argv[2] = of_arg;
    argv[3] = "bs=512";
    argv[4] = "seek=" FAR_START;
    argv[5] = "conv=notrunc";
    argv[6] = NULL;
    usb_host(u, argv, "dd-in.out");
    boot(u, &b, USB_SHELL);
    probe(u, &b, "probe", "/usbdiskprobe big", "big");
    no_fail(u, &b);
    usb_step(u, "read");
    keep_path(u, out, etude_evidence(u->c, "far-out.vol"), "the far volume read back");
    snprintf(in_arg, sizeof(in_arg), "if=%s", image);
    snprintf(of_arg, sizeof(of_arg), "of=%s", out);
    argv[0] = "dd";
    argv[1] = in_arg;
    argv[2] = of_arg;
    argv[3] = "bs=512";
    argv[4] = "skip=" FAR_START;
    argv[5] = "count=" FAR_SECTORS;
    argv[6] = NULL;
    usb_host(u, argv, "dd-out.out");
    argv[0] = "mtype";
    argv[1] = "-i";
    argv[2] = out;
    argv[3] = "::/far.txt";
    argv[4] = NULL;
    usb_host(u, argv, "far.txt");
    keep_path(u, txt, etude_evidence(u->c, "far.txt"), "mtype's output");
    text = usb_ok(u) ? usb_file(txt) : NULL;
    usb_assert(u, "host-read", text != NULL && strstr(text, "written past two terabytes") != NULL,
               "far.txt does not have the text the probe wrote");
    free(text);
    return usb_end(u);
}

/** Two logical units of one device are two disks, both mounted. */
static int luns(struct usb_case *u)
{
    struct usb_boot b;

    boot(u, &b, USB_SHELL);
    probe(u, &b, "probe", "/usbdiskprobe luns", "luns");
    no_fail(u, &b);
    return usb_end(u);
}

/* Each case's scenario, by the last part of its identity. */
static const struct {
    const char *name;
    int (*run)(struct usb_case *u);
} scenarios[] = {
    {"mbr", mbr},
    {"gpt", gpt},
    {"super", superfloppy},
    {"write", written},
    {"holdnested", holdnested},
    {"holdnestedexfat", holdnestedexfat},
    {"churn", churn},
    {"busybox", busybox},
    {"throughput", throughput},
    {"stickboot", stickboot},
    {"three", three},
    {"recovery", recovery},
    {"big", far},
    {"luns", luns},
};

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c);
    struct usb_case u;
    size_t i;

    usb_case(&u, c);
    if (strncmp(id, FAMILY, strlen(FAMILY)) != 0)
        return 1;
    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (strcmp(id + strlen(FAMILY), scenarios[i].name) == 0)
            return scenarios[i].run(&u);
    }
    return 1;
}
