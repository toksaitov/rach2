/*
 * mount_scenario.c: the scenario of machine.mount, the partition nodes, the
 * block path through a device that sleeps or never answers, mount(2) and the
 * claims it makes, and the FAT driver mounted from BusyBox's shell, each case
 * a boot of its own on a private copy of its disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The first byte of the FAT32 partition of the mount disk, at LBA 100352. */
#define FAT_OFFSET 51380224L

/* The byte of a FAT32 boot sector whose bit 0 is the volume's dirty flag. */
#define FAT_DIRTY_BYTE 65

/* The most of a console that is read back to be judged. */
#define CONSOLE_BYTES (1u << 20)

/* The record of a request that waits for the device's share of the pool. */
#define POOL_WAIT "request pool: waiting for a slot: the device holds its share"

/* A console record a case requires between its command and the probe's
 * line, in order. */
struct mark {
    const char *id;
    const char *text;
};

/* A case that runs a probe at the kernel's debug shell. */
struct probe_case {
    const char *name;     /* the last part of the case's identity */
    const char *command;  /* typed at the debug shell */
    const char *token;    /* the word of the probe's line before OK or FAIL */
    struct mark marks[4]; /* the records before the probe's line, ended by
                             an empty mark */
};

/* The cases that run a probe at the kernel's debug shell. */
static const struct probe_case probe_cases[] = {
    {"sleep", "exec /lagprobe sleep\r", "LAGPROBE", {{NULL, NULL}}},
    {"deadline",
     "exec /lagprobe deadline\r",
     "LAGPROBE",
     {{"quarantined", "quarantined"}, {"late-completion", "late completion"}, {NULL, NULL}}},
    {"root", "exec /lagprobe root\r", "LAGPROBE", {{NULL, NULL}}},
    {"write", "exec /lagprobe write\r", "LAGPROBE", {{NULL, NULL}}},
    {"mountprobe", "exec /mountprobe\r", "MOUNTPROBE", {{NULL, NULL}}},
    {"mounted", "exec /lagprobe mounted\r", "LAGPROBE", {{NULL, NULL}}},
    {"claim", "exec /mountprobe claim\r", "CLAIMPROBE", {{NULL, NULL}}},
    {"blockfail", "exec /lagprobe blockfail\r", "LAGPROBE", {{NULL, NULL}}},
    {"pool",
     "exec /lagprobe pool\r",
     "LAGPROBE",
     {{"pool-wait", POOL_WAIT}, {"flush-answered", "lag: flush answered"}, {NULL, NULL}}},
    {"dead",
     "exec /lagprobe dead\r",
     "LAGPROBE",
     {{"pool-wait", POOL_WAIT},
      {"quarantined", "quarantined"},
      {"stall-released", "device stalled: waiter released with EIO"},
      {NULL, NULL}}},
    {"queue", "exec /lagprobe queue\r", "LAGPROBE", {{"pool-wait", POOL_WAIT}, {NULL, NULL}}},
    {"dirty",
     "exec /lagprobe dirty\r",
     "LAGPROBE",
     {{"reclaim", "memory: reclaim:"}, {NULL, NULL}}},
    {"latefail", "exec /lagprobe latefail\r", "LAGPROBE", {{NULL, NULL}}},
};

/* The console being judged, carriage returns dropped. */
static char console[CONSOLE_BYTES];

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Read the evidence `name`, a console that libetude-case keeps, into
 * `console`; the empty string when it cannot be read.
 */
static void read_console(struct etude_case *c, const char *name)
{
    const char *path = etude_evidence(c, name);
    FILE *f;
    size_t n = 0;
    int ch;

    if (path != NULL && (f = fopen(path, "rb")) != NULL) {
        while (n + 1 < sizeof(console) && (ch = getc(f)) != EOF)
            if (ch != '\r')
                console[n++] = (char)ch;
        fclose(f);
    }
    console[n] = '\0';
}

/**
 * Copy into `buf` the first line of the file of artifact `name`, without its
 * line ending; the empty string when it cannot be read.
 */
static char *first_line(struct etude_case *c, const char *name, char *buf, size_t size)
{
    const char *path = etude_artifact(c, name);
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

/** Copy line `p` of `console` into `buf` without its trailing spaces. */
static void take_line(const char *p, char *buf, size_t size)
{
    size_t n = strcspn(p, "\n");

    if (n >= size)
        n = size - 1;
    memcpy(buf, p, n);
    while (n > 0 && buf[n - 1] == ' ')
        n--;
    buf[n] = '\0';
}

/**
 * Report assertion `id` from a search of the ended machine's console for
 * `text` after byte `from`.
 *
 * @return the offset just past the text, or `from` when it is not there.
 */
static uint64_t mark(struct lc *l, struct etude_case_machine *m, const char *id, const char *text,
                     uint64_t from)
{
    enum etude_status st;
    uint64_t at = from;
    char detail[160];

    if (!lc_ok(l))
        return from;
    st = etude_console_find(m, text, from, &at);
    snprintf(detail, sizeof(detail), "the console does not say '%s' where the case needs it", text);
    lc_assert(l, id, st == ETUDE_OK, detail);
    return st == ETUDE_OK ? at : from;
}

/**
 * Report assertion `probe-ok` from the ended machine's console, whose
 * evidence is `name`: the probe's line says TOKEN OK after byte `after`, and
 * no line after byte `from` says TOKEN FAIL.
 */
static void probe_ok(struct lc *l, struct etude_case_machine *m, const char *name,
                     const char *token, uint64_t from, uint64_t after)
{
    enum etude_status ok, bad;
    char pass[32], fail[32], why[160], detail[240];
    uint64_t at;

    if (!lc_ok(l))
        return;
    snprintf(pass, sizeof(pass), "%s OK", token);
    snprintf(fail, sizeof(fail), "%s FAIL", token);
    ok = etude_console_find(m, pass, after, &at);
    bad = etude_console_find(m, fail, from, &at);
    if (bad == ETUDE_OK) {
        lc_text(l, name, fail, from, why, sizeof(why));
        snprintf(detail, sizeof(detail), "the probe says %s%s", fail, why);
    } else if (bad != ETUDE_FAILED) {
        snprintf(detail, sizeof(detail), "the console was not kept whole");
    } else {
        snprintf(detail, sizeof(detail), "the probe never says %s after the records before it",
                 pass);
    }
    lc_assert(l, "probe-ok", ok == ETUDE_OK && bad == ETUDE_FAILED, detail);
}

/**
 * Report assertion `unmount-records` from the console: every record of an
 * unmount under /mnt counts as many opens as releases, and there is one.
 */
static void unmount_records(struct lc *l)
{
    static const char key[] = "vfs: unmounted /mnt";
    const char *p = console, *colon, *eol;
    long opens, releases;
    int n = 0, bad = 0;
    char detail[200];

    detail[0] = '\0';
    while ((p = strstr(p, key)) != NULL) {
        p += sizeof(key) - 1;
        n++;
        colon = strchr(p, ':');
        eol = strchr(p, '\n');
        if (colon == NULL || (eol != NULL && colon > eol) ||
            sscanf(colon + 1, " %ld opens, %ld releases", &opens, &releases) != 2) {
            bad++;
            snprintf(detail, sizeof(detail), "unmount record %d does not give its counts", n);
        } else if (opens != releases) {
            bad++;
            snprintf(detail, sizeof(detail), "unmount record %d counts %ld opens and %ld releases",
                     n, opens, releases);
        }
    }
    if (n == 0)
        snprintf(detail, sizeof(detail), "the kernel recorded no unmount under /mnt");
    lc_assert(l, "unmount-records", n > 0 && bad == 0, detail);
}

/**
 * Report assertion `written-back` from the console: a record of the memory
 * reclaim says it wrote back at least one dirty buffer.
 */
static void written_back(struct lc *l)
{
    static const char key[] = "memory: reclaim: ";
    const char *p = console;
    long frames, written;
    int seen = 0;

    while ((p = strstr(p, key)) != NULL) {
        p += sizeof(key) - 1;
        if (sscanf(p, "%ld frames from the caches, %ld written back", &frames, &written) == 2 &&
            written > 0)
            seen = 1;
    }
    lc_assert(l, "written-back", seen, "no reclaim record says it wrote back a dirty buffer");
}

/**
 * Report assertion `dirty-flag` from the private copy of disk `disk`: bit 0
 * of byte 65 of the FAT32 boot sector, the volume's dirty flag, is set.
 */
static void dirty_flag(struct lc *l, const char *disk)
{
    const char *copy = etude_private_copy(l->c, disk);
    FILE *f = copy != NULL ? fopen(copy, "rb") : NULL;
    char detail[120];
    int byte = EOF;

    if (!lc_ok(l))
        return;
    if (f != NULL) {
        if (fseek(f, FAT_OFFSET + FAT_DIRTY_BYTE, SEEK_SET) == 0)
            byte = getc(f);
        fclose(f);
    }
    if (byte == EOF)
        snprintf(detail, sizeof(detail), "the boot sector of the copy cannot be read");
    else
        snprintf(detail, sizeof(detail), "byte %d of the FAT32 boot sector is %d", FAT_DIRTY_BYTE,
                 byte);
    lc_assert(l, "dirty-flag", byte != EOF && (byte & 1) != 0, detail);
}

/**
 * Run probe case `p`: boot the machine to the kernel's debug shell, run the
 * probe and wait for it to end, end the machine with QMP's quit, and judge
 * the console.
 */
static void probe(struct lc *l, const struct probe_case *p)
{
    struct etude_case_machine *m;
    uint64_t at = 0, from, after;
    size_t i;

    if (!lc_step(l, "probe") || (m = lc_boot(l, "shell")) == NULL ||
        !lc_wait(l, m, "Type 'help'", &at, NULL) || !lc_wait(l, m, "\n> ", &at, NULL) ||
        !lc_type(l, m, p->command))
        return;
    from = at;
    if (!lc_wait(l, m, "returning to kernel shell", &at, NULL) || !lc_quit(l, m, NULL)) {
        if (!l->stopped)
            lc_assert(l, "probe-ok", 0, l->why);
        return;
    }
    after = from;
    for (i = 0; p->marks[i].id != NULL; i++)
        after = mark(l, m, p->marks[i].id, p->marks[i].text, after);
    probe_ok(l, m, "shell.1.console", p->token, from, after);
    if (strcmp(p->name, "blockfail") == 0 || strcmp(p->name, "latefail") == 0)
        lc_find(l, m, "read-only-record", "vfs: read-only: /lb", from, 1);
    if (strcmp(p->name, "queue") == 0)
        lc_find(l, m, "none-quarantined", "quarantined", 0, 0);
    if (!lc_ok(l))
        return;
    read_console(l->c, "shell.1.console");
    if (strcmp(p->name, "mountprobe") == 0)
        unmount_records(l);
    if (strcmp(p->name, "dirty") == 0)
        written_back(l);
    if (strcmp(p->name, "latefail") == 0)
        dirty_flag(l, "disk");
}

/**
 * Log in at os-real's console as `user`, whose password is the first line
 * of artifact `password`, and wait for `prompt`.
 *
 * @return 1 when the prompt came.
 */
static int login(struct lc *l, struct etude_case_machine *m, const char *user, const char *password,
                 const char *prompt, uint64_t *at)
{
    char pass[128], line[160];

    first_line(l->c, password, pass, sizeof(pass));
    snprintf(line, sizeof(line), "%s\r", user);
    if (!lc_wait(l, m, "rach2 login:", at, NULL) || !lc_type(l, m, line) ||
        !lc_wait(l, m, "Password:", at, NULL))
        return 0;
    snprintf(line, sizeof(line), "%s\r", pass);
    return lc_type(l, m, line) && lc_wait(l, m, prompt, at, NULL);
}

/** Type `line` at the console and wait for `text`, reported as `id`. */
static int say(struct lc *l, struct etude_case_machine *m, const char *line, const char *text,
               uint64_t *at, const char *id)
{
    return lc_type(l, m, line) && lc_wait(l, m, text, at, id);
}

/**
 * Report assertions `listing` and `bytes` from the console: the line the
 * shell printed of the volume's names is the artifact `list`, which mdir
 * read off the volume, and the line cksum printed for each file is the one
 * of artifact `sums`, which cksum gave for what mcopy read off it.
 */
static void against_mtools(struct lc *l)
{
    const char *path = etude_artifact(l->c, "sums"), *p;
    char want[512], got[512], name[128], crc[32], size[32], line[256], detail[300];
    size_t len;
    FILE *f;
    int held = 1, n = 0;

    first_line(l->c, "list", want, sizeof(want));
    got[0] = '\0';
    for (p = console; (p = strstr(p, "again.bin")) != NULL; p++)
        if (p == console || p[-1] == '\n') {
            take_line(p, got, sizeof(got));
            break;
        }
    snprintf(detail, sizeof(detail), "the shell listed '%.120s' where mdir lists '%.120s'", got,
             want);
    lc_assert(l, "listing", want[0] != '\0' && strcmp(got, want) == 0, detail);

    snprintf(detail, sizeof(detail), "the artifact of the sums cannot be read");
    f = path != NULL ? fopen(path, "r") : NULL;
    while (f != NULL && fgets(line, sizeof(line), f) != NULL && held) {
        if (sscanf(line, "%31s %31s %127s", crc, size, name) != 3) {
            held = 0;
            break;
        }
        n++;
        snprintf(want, sizeof(want), "%s %s /mnt/%s", crc, size, name);
        len = strlen(name) + 6;
        got[0] = '\0';
        for (p = console; *p != '\0'; p += strcspn(p, "\n") + (p[strcspn(p, "\n")] != '\0')) {
            take_line(p, line, sizeof(line));
            if (strlen(line) >= len && strstr(line, "cksum") == NULL &&
                strncmp(line + strlen(line) - len, " /mnt/", 6) == 0 &&
                strcmp(line + strlen(line) - len + 6, name) == 0) {
                snprintf(got, sizeof(got), "%s", line);
                break;
            }
        }
        if (strcmp(got, want) != 0) {
            held = 0;
            snprintf(detail, sizeof(detail),
                     "the guest's cksum of %.40s is '%.100s', mcopy's '%.100s'", name, got, want);
        }
    }
    if (f != NULL)
        fclose(f);
    lc_assert(l, "bytes", f != NULL && n > 0 && held, detail);
}

/**
 * Run the case of BusyBox on os-real: mount the FAT32 partition read-only,
 * list and read it, find it busy while a forked holder keeps a file of it
 * open, unmount it once the holder lets go, and compare what the shell
 * printed with what mtools reads off the volume.
 */
static void busybox(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(l, "busybox") || (m = lc_boot(l, "login")) == NULL ||
        !login(l, m, "root", "demo-password", "# ", &at))
        return;
    if (!say(l, m, "mkdir /mnt && mount -t vfat -o ro /dev/disk0p2 /mnt && echo MOUNT\"\"ED\r",
             "MOUNTED", &at, "mounted") ||
        !say(l, m, "ls /mnt | sort | tr '\\n' ' '; echo; echo LIST\"\"DONE\r", "LISTDONE", &at,
             "listed") ||
        !say(l, m,
             "cat /mnt/hello.txt; cksum /mnt/hello /mnt/again.bin /mnt/hello.txt; "
             "echo SUMS\"\"DONE\r",
             "SUMSDONE", &at, "summed") ||
        !say(l, m,
             "mkfifo /tmp/go; exec 3< /mnt/hello.txt; (read x < /tmp/go; exec 3<&-; "
             "echo HOLDER\"\"DONE) & exec 3<&-; umount /mnt; echo UMOUNT\"\"RC=$?\r",
             "UMOUNTRC=1", &at, "umount-busy") ||
        !say(l, m, "echo > /tmp/go\r", "HOLDERDONE", &at, "holder-ended") ||
        !say(l, m, "umount /mnt && echo UN\"\"MOUNTED\r", "UNMOUNTED", &at, "unmounted") ||
        !say(l, m, "[ \"$(grep -c vfat /proc/mounts)\" = 0 ] && echo BB\"\"DONE\r", "BBDONE", &at,
             "no-vfat") ||
        !lc_quit(l, m, NULL))
        return;
    lc_find(l, m, "ebusy", "Resource busy", 0, 1);
    read_console(l->c, "login.1.console");
    against_mtools(l);
}

/**
 * Run the case of the memory reserve on os-real: the test account takes
 * every frame outside the reserve and its fork is refused, and root still
 * logs in at the console and kills it.
 */
static void hog(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(l, "hog") || (m = lc_boot(l, "login")) == NULL ||
        !login(l, m, "root", "demo-password", "# ", &at) ||
        !say(l, m, "mkdir /mnt && mount -t vfat /dev/disk0p2 /mnt && echo MOUNT\"\"ED\r", "MOUNTED",
             &at, "mounted") ||
        !lc_type(l, m, "exit\r") || !login(l, m, "test", "test-password", "$ ", &at) ||
        !say(l, m, "/mnt/memprobe hog &\r", "MEMPROBE HOG: fork refused with ENOMEM", &at,
             "fork-refused") ||
        !lc_type(l, m, "exit\r") || !login(l, m, "root", "demo-password", "# ", &at) ||
        !say(l, m,
             "kill -9 $(pidof memprobe) && grep MemFree /proc/meminfo && echo RECO\"\"VERED\r",
             "RECOVERED", &at, "recovered") ||
        !lc_quit(l, m, NULL))
        return;
    lc_find(l, m, "no-probe-fail", "MEMPROBE FAIL", 0, 0);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const busyboxes[] = {"mounted",      "listed",    "summed",  "umount-busy",
                                            "holder-ended", "unmounted", "no-vfat", "ebusy",
                                            "listing",      "bytes",     NULL};
    static const char *const hogs[] = {"mounted", "fork-refused", "recovered", "no-probe-fail",
                                       NULL};
    const char *ids[LC_ASSERTS + 1];
    const char *name = case_name(c);
    const struct probe_case *p;
    struct lc l;
    size_t i, n;

    if (strcmp(name, "busybox") == 0) {
        lc_begin(&l, c, busyboxes);
        busybox(&l);
        return lc_finish(&l);
    }
    if (strcmp(name, "hog") == 0) {
        lc_begin(&l, c, hogs);
        hog(&l);
        return lc_finish(&l);
    }
    for (i = 0; i < sizeof(probe_cases) / sizeof(probe_cases[0]); i++)
        if (strcmp(name, probe_cases[i].name) == 0)
            break;
    if (i == sizeof(probe_cases) / sizeof(probe_cases[0]))
        return 1;
    p = &probe_cases[i];
    for (n = 0; p->marks[n].id != NULL; n++)
        ids[n] = p->marks[n].id;
    ids[n++] = "probe-ok";
    if (strcmp(name, "mountprobe") == 0)
        ids[n++] = "unmount-records";
    if (strcmp(name, "blockfail") == 0 || strcmp(name, "latefail") == 0)
        ids[n++] = "read-only-record";
    if (strcmp(name, "queue") == 0)
        ids[n++] = "none-quarantined";
    if (strcmp(name, "dirty") == 0)
        ids[n++] = "written-back";
    if (strcmp(name, "latefail") == 0)
        ids[n++] = "dirty-flag";
    ids[n] = NULL;
    lc_begin(&l, c, ids);
    probe(&l, p);
    return lc_finish(&l);
}
