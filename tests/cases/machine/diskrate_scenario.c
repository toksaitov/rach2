/*
 * diskrate_scenario.c: the scenario of machine.diskrate.storage
 * (tests/cases/machine.diskrate.json). The storage paths timed in the
 * machine and judged by the counts the kernel keeps: files on exFAT and
 * FAT32, small writes, a raw read through a pipe, a USB stick and an AHCI
 * disk written and read raw. No row is judged by its time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fileio.h"
#include "etude/scenario.h"

/* The most of a console that is read back to be judged. */
#define CONSOLE_BYTES (4u << 20)

/* One line typed at the console and the text its end prints. */
struct line {
    const char *send;
    const char *expect;
};

/* The machine's shell functions, typed first since the console takes a
 * line of bounded length. `t NAME COMMAND` runs COMMAND through `eval` and
 * prints `T_NAME HUNDREDTHS`, the machine's clock read before and after, or
 * `T_NAME FAIL`; `moved BYTES COMMAND...` runs a dd that must report BYTES
 * copied; `c NAME` prints `C_NAME` and the disk's commands, status reads,
 * wait ticks, cache hits and quarantines from /proc/disk. */
#define DEFS                                                                                  \
    "now() { read -r u _ < /proc/uptime; echo $(( ${u%.*} * 100 + 1${u#*.} - 100 )); }; "     \
    "t() { s=$(now); if eval \"$2\"; then echo T_$1 $(( $(now) - s )); else echo T_$1 FAIL; " \
    "fi; }; "                                                                                 \
    "moved() { n=$1; shift; \"$@\" 2>/tmp/dd.err && grep -q \"^$n bytes\" /tmp/dd.err; }; "   \
    "c() { echo C_$1 $(awk \"/^disk-commands /{c=\\$2} /^disk-polls /{p=\\$2} "               \
    "/^disk-wait-ticks /{w=\\$2} /^cache-hits /{h=\\$2} /^disk-quarantines /{q=\\$2} "        \
    "END{print c, p, w, h, q}\" /proc/disk); }; echo DEFS\"\"DONE\r"

#define PIPEHASH                                                                           \
    "t pipehash 'dd if=/dev/disk0 bs=1M count=64 2>/tmp/dd.err | sha256sum >/dev/null && " \
    "grep -q \"^67108864 bytes\" /tmp/dd.err'\r"

/* The rows of the os-real boot, after the login: a raw disk, an exFAT
 * volume and a FAT32 one, each on IDE. */
static const struct line paths_lines[] = {
    {DEFS, "DEFSDONE"},
    {"c boot0\r", "C_boot0"},
    {PIPEHASH, "T_pipehash"},
    {PIPEHASH, "T_pipehash"},
    {PIPEHASH, "T_pipehash"},
    {"t rawread 'moved 134217728 dd if=/dev/disk0 of=/dev/null bs=1M count=128'\r", "T_rawread"},
    {"mkdir -p /e /f && mount -t exfat /dev/disk1p1 /e && mount -t vfat /dev/disk2p1 /f && "
     "echo MOUNT\"\"ED\r",
     "MOUNTED"},
    {"c exwrite0\r", "C_exwrite0"},
    {"t exwrite 'moved 67108864 dd if=/dev/zero of=/e/big bs=1M count=64 && sync'\r", "T_exwrite"},
    {"c exwrite1\r", "C_exwrite1"},
    {"c exread0\r", "C_exread0"},
    {"t exread 'moved 67108864 dd if=/e/big of=/dev/null bs=1M && "
     "moved 67108864 dd if=/e/big of=/dev/null bs=1M'\r",
     "T_exread"},
    {"c exread1\r", "C_exread1"},
    {"c fatwrite0\r", "C_fatwrite0"},
    {"t fatwrite 'moved 67108864 dd if=/dev/zero of=/f/big bs=1M count=64 && sync'\r",
     "T_fatwrite"},
    {"c fatwrite1\r", "C_fatwrite1"},
    {"c fatread0\r", "C_fatread0"},
    {"t fatread 'moved 67108864 dd if=/f/big of=/dev/null bs=1M && "
     "moved 67108864 dd if=/f/big of=/dev/null bs=1M'\r",
     "T_fatread"},
    {"c fatread1\r", "C_fatread1"},
    {"c exsmall0\r", "C_exsmall0"},
    {"t exsmall 'moved 16777216 dd if=/dev/zero of=/e/small bs=4k count=4096 && sync'\r",
     "T_exsmall"},
    {"c exsmall1\r", "C_exsmall1"},
    {"c fatsmall0\r", "C_fatsmall0"},
    {"t fatsmall 'moved 16777216 dd if=/dev/zero of=/f/small bs=4k count=4096 && sync'\r",
     "T_fatsmall"},
    {"c fatsmall1\r", "C_fatsmall1"},
    {"c exfiles0\r", "C_exfiles0"},
    {"t exfiles 'i=0; n=0; while [ $i -lt 200 ]; do dd if=/dev/disk0 of=/e/small-$i bs=4k "
     "skip=$i count=8 2>/dev/null && n=$((n+1)); i=$((i+1)); done; sync; [ $n = 200 ] && "
     "[ $(stat -c %s /e/small-199) = 32768 ]'\r",
     "T_exfiles"},
    {"c exfiles1\r", "C_exfiles1"},
    {"umount /e && umount /f && echo ALL\"\"DONE\r", "ALLDONE"},
};

/* The rows of the recovery boot with a USB stick: 4 MiB written raw in 128
 * pieces of 32 KiB, each numbered, read back in 32 KiB requests and in 512
 * KiB ones. */
static const struct line stick_lines[] = {
    {DEFS, "DEFSDONE"},
    {"yes RATE-STICK-PATTERN-LINE | head -c 32768 > /tmp/base; p() { printf PIECE-%03d- $1; "
     "dd if=/tmp/base bs=32758 count=1 2>/dev/null; }; echo PAT\"\"DONE\r",
     "PATDONE"},
    {"c stickwrite0\r", "C_stickwrite0"},
    {"t stickwrite 'i=0; while [ $i -lt 128 ]; do p $i > /tmp/blk; moved 32768 dd if=/tmp/blk "
     "of=/dev/disk0 bs=32k seek=$((32 + i)) count=1 || break; i=$((i + 1)); done; sync; "
     "[ $i = 128 ]'\r",
     "T_stickwrite"},
    {"c stickwrite1\r", "C_stickwrite1"},
    {"t stickread 'dd if=/dev/disk0 bs=32k skip=32 count=128 2>/tmp/dd.err | cksum > /tmp/sum "
     "&& grep -q \"^4194304 bytes\" /tmp/dd.err'\r",
     "T_stickread"},
    {"c stickread1\r", "C_stickread1"},
    {"t stickbig 'dd if=/dev/disk0 bs=512k skip=2 count=8 2>/tmp/dd.err | cksum > /tmp/sumbig "
     "&& grep -q \"^4194304 bytes\" /tmp/dd.err'\r",
     "T_stickbig"},
    {"c stickbig1\r", "C_stickbig1"},
    {"i=0; while [ $i -lt 128 ]; do p $i; i=$((i + 1)); done | cksum > /tmp/want; "
     "cmp /tmp/sum /tmp/want && echo SAME; cmp /tmp/sumbig /tmp/want && echo SAME\"\"BIG; "
     "echo STICK\"\"DONE\r",
     "STICKDONE"},
};

/* The rows of the recovery boot with an AHCI disk, whose requests are the
 * block layer's whole limit: a pattern of 1 MiB, copied 32 times in 1 MiB
 * calls and read back in 1 MiB and in 32 KiB calls. */
static const struct line ahci_lines[] = {
    {DEFS, "DEFSDONE"},
    {"yes RATE-AHCI-PATTERN-LINE | head -c 32768 > /tmp/b32; echo B32\"\"DONE\r", "B32DONE"},
    {"i=0; while [ $i -lt 32 ]; do dd if=/tmp/b32 of=/dev/disk0 bs=32k seek=$((128 + i)) "
     "count=1 2>/dev/null || break; i=$((i + 1)); done; sync; if [ $i = 32 ]; then "
     "echo PAT\"\"DONE; else echo PAT\"\"FAIL; fi; echo PAT\"\"END\r",
     "PATEND"},
    {"c ahciwrite0\r", "C_ahciwrite0"},
    {"t ahciwrite 'i=0; while [ $i -lt 32 ]; do moved 1048576 dd if=/dev/disk0 of=/dev/disk0 "
     "bs=1M skip=4 seek=$((8 + i)) count=1 || break; i=$((i + 1)); done; sync; [ $i = 32 ]'\r",
     "T_ahciwrite"},
    {"c ahciwrite1\r", "C_ahciwrite1"},
    {"t ahciread 'dd if=/dev/disk0 bs=1M skip=8 count=32 2>/tmp/dd.err | cksum > /tmp/suma && "
     "grep -q \"^33554432 bytes\" /tmp/dd.err'\r",
     "T_ahciread"},
    {"c ahciread1\r", "C_ahciread1"},
    {"t ahcismall 'dd if=/dev/disk0 bs=32k skip=256 count=1024 2>/tmp/dd.err | cksum > /tmp/sumb "
     "&& grep -q \"^33554432 bytes\" /tmp/dd.err'\r",
     "T_ahcismall"},
    {"c ahcismall1\r", "C_ahcismall1"},
    {"i=0; while [ $i -lt 1024 ]; do cat /tmp/b32; i=$((i + 1)); done | cksum > /tmp/wanta; "
     "cmp /tmp/suma /tmp/wanta && cmp /tmp/sumb /tmp/wanta && echo SAME\"\"AHCI; "
     "echo AHCI\"\"DONE\r",
     "AHCIDONE"},
};

/* The console of the boot being judged, carriage returns dropped. */
static char console[CONSOLE_BYTES];

/**
 * Read the console of boot 1 of machine `machine` into `console`; the
 * empty string when it cannot be read.
 */
static void read_console(struct etude_case *c, const char *machine)
{
    char name[64];
    const char *path;
    FILE *f;
    size_t n = 0;
    int ch;

    snprintf(name, sizeof(name), "%s.1.console", machine);
    path = etude_evidence(c, name);
    if (path != NULL && (f = fopen(path, "rb")) != NULL) {
        while (n + 1 < sizeof(console) && (ch = getc(f)) != EOF)
            if (ch != '\r')
                console[n++] = (char)ch;
        fclose(f);
    }
    console[n] = '\0';
}

/** Say whether a line of the console is `line` whole. */
static int has_line(const char *line)
{
    size_t len = strlen(line);
    const char *p = console;

    while ((p = strstr(p, line)) != NULL) {
        if ((p == console || p[-1] == '\n') && (p[len] == '\n' || p[len] == '\0'))
            return 1;
        p += len;
    }
    return 0;
}

/**
 * Give in `v` the `n` numbers of the console's last line that begins with
 * `prefix` and a space; 0, or -1 when there is none or it has fewer.
 */
static int numbers(const char *prefix, long v[], int n)
{
    size_t len = strlen(prefix);
    const char *p = console, *last = NULL;
    char *end;
    int i;

    while ((p = strstr(p, prefix)) != NULL) {
        if ((p == console || p[-1] == '\n') && p[len] == ' ')
            last = p + len;
        p += len;
    }
    if (last == NULL)
        return -1;
    for (i = 0; i < n; i++) {
        v[i] = strtol(last, &end, 10);
        if (end == last || (*end != ' ' && *end != '\n' && *end != '\0'))
            return -1;
        last = end;
    }
    return 0;
}

/**
 * Give the least time in hundredths of a second of the console's lines
 * `T_NAME N`, or -1 when no run of the row printed one, as one whose command
 * failed prints `T_NAME FAIL`.
 */
static long least(const char *name)
{
    char prefix[48];
    size_t len;
    const char *p = console;
    long best = -1, t;
    char *end;

    snprintf(prefix, sizeof(prefix), "T_%s ", name);
    len = strlen(prefix);
    while ((p = strstr(p, prefix)) != NULL) {
        if (p == console || p[-1] == '\n') {
            t = strtol(p + len, &end, 10);
            if (end != p + len && (*end == '\n' || *end == '\0') && (best < 0 || t < best))
                best = t;
        }
        p += len;
    }
    return best;
}

/* The counters of /proc/disk that `c NAME` prints, in its order. */
enum { COMMANDS, POLLS, WAITS, HITS, QUARANTINES, COUNTERS };

/**
 * Give in `d` the counters' change from the console's line `C_FROM` to
 * `C_TO`; 0, or -1 when either is missing.
 */
static int between(const char *from, const char *to, long d[COUNTERS])
{
    char a[48], b[48];
    long va[COUNTERS], vb[COUNTERS];
    int i;

    snprintf(a, sizeof(a), "C_%s", from);
    snprintf(b, sizeof(b), "C_%s", to);
    if (numbers(a, va, COUNTERS) != 0 || numbers(b, vb, COUNTERS) != 0)
        return -1;
    for (i = 0; i < COUNTERS; i++)
        d[i] = vb[i] - va[i];
    return 0;
}

/**
 * Report assertion `id`, that row `name` printed a time, so that its command
 * moved its bytes, and print the time and rate for a person: `kib`
 * kibibytes in that time.
 */
static void row(struct etude_case *c, const char *id, const char *name, long kib, const char *what)
{
    long t = least(name);
    char detail[96];

    snprintf(detail, sizeof(detail), "no time for %s: its command failed or never ended", name);
    etude_assert(c, id, t >= 0, detail);
    if (t > 0)
        printf("diskrate: %s: %ld.%02ld s, %ld KiB/s\n", what, t / 100, t % 100, kib * 100 / t);
    else if (t == 0)
        printf("diskrate: %s: under 0.01 s\n", what);
}

/**
 * Report assertion `id`: from `C_FROM` to `C_TO` the driver made commands,
 * and at most 100 status reads a command, which a driver that spins in
 * place of sleeping passes by orders of magnitude.
 */
static void polls(struct etude_case *c, const char *id, const char *from, const char *to)
{
    long d[COUNTERS];
    char detail[128];

    if (between(from, to, d) != 0) {
        etude_assert(c, id, 0, "the counters were not printed");
        return;
    }
    snprintf(detail, sizeof(detail), "%ld commands and %ld status reads", d[COMMANDS], d[POLLS]);
    etude_assert(c, id, d[COMMANDS] > 0 && d[POLLS] <= d[COMMANDS] * 100, detail);
    if (d[COMMANDS] > 0)
        printf("diskrate: %s: %ld commands, %ld status reads and %ld wait ticks a command\n", id,
               d[COMMANDS], d[POLLS] / d[COMMANDS], d[WAITS] / d[COMMANDS]);
}

/**
 * Report assertion `id`: row `name`, from `C_NAME0` to `C_NAME1`, made
 * commands, at most `commands` of them and at most `hits` cache hits. A path
 * that walks a cluster chain from its start for every call, or writes each
 * entry through, is over them by far.
 */
static void counts(struct etude_case *c, const char *id, const char *name, long commands, long hits)
{
    char from[32], to[32], detail[128];
    long d[COUNTERS];

    snprintf(from, sizeof(from), "%s0", name);
    snprintf(to, sizeof(to), "%s1", name);
    if (between(from, to, d) != 0) {
        etude_assert(c, id, 0, "the counters were not printed");
        return;
    }
    snprintf(detail, sizeof(detail), "%ld commands against %ld, %ld cache hits against %ld",
             d[COMMANDS], commands, d[HITS], hits);
    etude_assert(c, id,
                 d[COMMANDS] > 0 && d[COMMANDS] <= commands && d[HITS] >= 0 && d[HITS] <= hits,
                 detail);
}

/**
 * Report assertion `id`: from `C_FROM` to `C_TO` exactly `n` commands, and
 * assertion `held_id`: commands and none quarantined. The ticks the commands
 * spent in flight are printed and not judged, since the host's storage
 * decides them.
 */
static void exact(struct etude_case *c, const char *id, const char *held_id, const char *from,
                  const char *to, long n)
{
    long d[COUNTERS];
    char detail[96];

    if (between(from, to, d) != 0) {
        etude_assert(c, id, 0, "the counters were not printed");
        etude_assert(c, held_id, 0, "the counters were not printed");
        return;
    }
    snprintf(detail, sizeof(detail), "%ld commands, not %ld", d[COMMANDS], n);
    etude_assert(c, id, d[COMMANDS] == n, detail);
    snprintf(detail, sizeof(detail), "%ld commands, %ld quarantined", d[COMMANDS], d[QUARANTINES]);
    etude_assert(c, held_id, d[COMMANDS] > 0 && d[QUARANTINES] == 0, detail);
    if (d[COMMANDS] > 0)
        printf("diskrate: %s: %ld commands, %ld ticks in flight a command\n", id, d[COMMANDS],
               d[WAITS] / d[COMMANDS]);
}

/**
 * Boot machine `machine` in the step of that name, wait for `ready`, type
 * `lines` in turn, each after the text the one before prints, and end the
 * machine with QMP's `quit`; then read its console. A wait that fails stops
 * the typing and is said on standard error; the caller judges what the
 * console has.
 *
 * @return 0 when every line's text came, -1 otherwise.
 */
static int drive(struct etude_case *c, const char *machine, const struct line *ready, size_t nready,
                 const struct line *lines, size_t n)
{
    struct etude_case_machine *m = NULL;
    struct etude_termination end;
    char *reply = NULL;
    uint64_t at = 0;
    size_t i;
    int rc = -1;

    if (etude_step(c, machine) != ETUDE_OK || etude_boot(c, machine, NULL, &m, &end) != ETUDE_OK) {
        console[0] = '\0';
        fprintf(stderr, "diskrate: %s: the machine did not boot\n", machine);
        return -1;
    }
    for (i = 0; i < nready + n; i++) {
        const struct line *l = i < nready ? &ready[i] : &lines[i - nready];

        if (l->send != NULL && etude_type(m, l->send) != ETUDE_OK)
            break;
        if (etude_console_wait(m, l->expect, at, &at) != ETUDE_OK)
            break;
    }
    if (i == nready + n)
        rc = 0;
    else
        fprintf(stderr, "diskrate: %s: no %s on the console\n", machine,
                i < nready ? ready[i].expect : lines[i - nready].expect);
    etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    etude_wait_end(m, &end);
    read_console(c, machine);
    return rc;
}

/**
 * Make the exFAT and FAT32 disks of the os-real boot in their empty
 * copies, each with an MBR whose partition starts at sector 2048: the
 * exFAT volume of 522240 sectors from the fixture builder, and a FAT32
 * volume of 520192 that mformat makes.
 *
 * @return 0, or -1 when a tool did not end with status 0.
 */
static int volumes(struct etude_case *c)
{
    const char *exfat = etude_private_copy(c, "exfat"), *fat = etude_private_copy(c, "fat");
    const char *kept = etude_evidence(c, "exfat.vol");
    char vol[4096], at[4200];
    char *make[] = {"mkexfatfixture", "--make",   vol,    "--sectors",
                    "522240",         "--offset", "2048", NULL};
    char *extable[] = {"mkmbr", "--lba", (char *)exfat, "2048:522240:07", NULL};
    char *fattable[] = {"mkmbr", "--lba", (char *)fat, "2048:520192:0c", NULL};
    char *format[] = {"mformat", "-i", at, "-F", "-T", "520192", "-v", "RATEFAT", "::", NULL};
    char *const *steps[] = {make, extable, fattable, format};
    const char *names[] = {"mkexfatfixture", "mkmbr-exfat", "mkmbr-fat", "mformat"};
    struct etude_termination end;
    size_t i;
    int rc = 0;

    if (exfat == NULL || fat == NULL || kept == NULL)
        return -1;
    snprintf(vol, sizeof(vol), "%s", kept);
    snprintf(at, sizeof(at), "%s@@1048576", fat);
    for (i = 0; i < sizeof(steps) / sizeof(steps[0]) && rc == 0; i++) {
        if (i == 1 && case_copy_range(vol, 0, UINT64_MAX, exfat, 1048576, 0) != 0) {
            rc = -1;
            break;
        }
        if (etude_host(c, steps[i], names[i], &end) != ETUDE_OK || end.end != ETUDE_END_EXITED ||
            end.value != 0) {
            fprintf(stderr, "diskrate: %s did not end with status 0\n", names[i]);
            rc = -1;
        }
    }
    remove(vol);
    return rc;
}

/** Give the demo password, the artifact `demo-password`'s first line, in
 * `buf`, or the empty string. */
static char *password(struct etude_case *c, char *buf, size_t size)
{
    const char *path = etude_artifact(c, "demo-password");
    FILE *f = path != NULL ? fopen(path, "r") : NULL;

    buf[0] = '\0';
    if (f != NULL) {
        if (fgets(buf, (int)size, f) == NULL)
            buf[0] = '\0';
        fclose(f);
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf;
}

/** Step `paths`: the os-real boot with a raw disk, an exFAT and a FAT32
 * disk, all on IDE, and its rows. */
static void paths(struct etude_case *c)
{
    static const char *const drives[] = {"primary master", "primary slave", "secondary master"};
    char pass[128], send[160], want[64];
    struct line login[3];
    const char *p, *q;
    size_t i;
    int dma;

    snprintf(send, sizeof(send), "%s\r", password(c, pass, sizeof(pass)));
    login[0].send = NULL;
    login[0].expect = "rach2 login:";
    login[1].send = "root\r";
    login[1].expect = "Password:";
    login[2].send = send;
    login[2].expect = "# ";
    drive(c, "paths", login, 3, paths_lines, sizeof(paths_lines) / sizeof(paths_lines[0]));
    for (dma = 1, i = 0; i < sizeof(drives) / sizeof(drives[0]); i++) {
        snprintf(want, sizeof(want), "ata: %s: ", drives[i]);
        for (p = console; (p = strstr(p, want)) != NULL; p++) {
            q = strchr(p, '\n');
            if (strstr(p, "bus master DMA") != NULL &&
                (q == NULL || strstr(p, "bus master DMA") < q))
                break;
        }
        if (p == NULL)
            dma = 0;
    }
    etude_assert(c, "dma", dma, "an IDE disk did not select bus-master DMA");
    row(c, "row-rawread", "rawread", 131072, "128 MiB read from the raw device");
    row(c, "row-pipehash", "pipehash", 65536, "64 MiB read raw through a pipe into sha256sum");
    row(c, "row-exwrite", "exwrite", 65536, "64 MiB written into a file on exFAT and synced");
    row(c, "row-exread", "exread", 131072, "the exFAT file read twice, cold and from the cache");
    row(c, "row-fatwrite", "fatwrite", 65536, "64 MiB written into a file on FAT32 and synced");
    row(c, "row-fatread", "fatread", 131072, "the FAT32 file read twice, cold and from the cache");
    row(c, "row-exsmall", "exsmall", 16384, "16 MiB written on exFAT in 4 KiB calls");
    row(c, "row-fatsmall", "fatsmall", 16384, "16 MiB written on FAT32 in 4 KiB calls");
    row(c, "row-exfiles", "exfiles", 6400, "two hundred 32 KiB files written on exFAT");
    polls(c, "polls-boot", "boot0", "exfiles1");
    polls(c, "polls-exwrite", "exwrite0", "exwrite1");
    polls(c, "polls-fatwrite", "fatwrite0", "fatwrite1");
    polls(c, "polls-exsmall", "exsmall0", "exsmall1");
    polls(c, "polls-fatsmall", "fatsmall0", "fatsmall1");
    counts(c, "counts-exwrite", "exwrite", 9300, 400000);
    counts(c, "counts-exread", "exread", 9300, 400000);
    counts(c, "counts-fatread", "fatread", 14800, 1300000);
    counts(c, "counts-fatwrite", "fatwrite", 14800, 1300000);
    counts(c, "counts-exsmall", "exsmall", 82000, 120000);
    counts(c, "counts-fatsmall", "fatsmall", 99600, 350000);
    counts(c, "counts-exfiles", "exfiles", 35500, 65000);
}

/* The recovery system's greeting and prompt, which needs no login. */
static const struct line recovery[] = {{NULL, "recovery image"}, {NULL, "~ #"}};

/** Step `stick`: the recovery boot with a USB stick, written and read raw. */
static void stick(struct etude_case *c)
{
    drive(c, "stick", recovery, 2, stick_lines, sizeof(stick_lines) / sizeof(stick_lines[0]));
    etude_assert(c, "stick-same", has_line("SAME"), "the stick did not read back what was written");
    exact(c, "stick-write-commands", "stick-write-held", "stickwrite0", "stickwrite1", 128);
    exact(c, "stick-read-commands", "stick-read-held", "stickwrite1", "stickread1", 128);
    exact(c, "stick-big-commands", "stick-big-held", "stickread1", "stickbig1", 8);
    etude_assert(c, "stick-big-same", has_line("SAMEBIG"),
                 "the 512 KiB reads did not read what the 32 KiB reads read");
}

/** Step `ahci`: the recovery boot with an AHCI disk, written and read raw. */
static void ahci(struct etude_case *c)
{
    drive(c, "ahci", recovery, 2, ahci_lines, sizeof(ahci_lines) / sizeof(ahci_lines[0]));
    etude_assert(c, "ahci-pattern", has_line("PATDONE"),
                 "the pattern was not written to the AHCI disk");
    exact(c, "ahci-write-commands", "ahci-write-held", "ahciwrite0", "ahciwrite1", 96);
    exact(c, "ahci-read-commands", "ahci-read-held", "ahciwrite1", "ahciread1", 32);
    exact(c, "ahci-small-commands", "ahci-small-held", "ahciread1", "ahcismall1", 1024);
    etude_assert(c, "ahci-same", has_line("SAMEAHCI"),
                 "the AHCI disk did not read back what was written");
}

int etude_scenario(struct etude_case *c)
{
    if (etude_step(c, "volumes") != ETUDE_OK || volumes(c) != 0)
        return 1;
    paths(c);
    stick(c);
    ahci(c);
    return 0;
}
