/*
 * ext2_scenario.c: the scenario of machine.ext2, the ext2 driver booted.
 * Cases `probe-NAME` run a probe as init on an ext2 root staged from the test
 * manifest; `links` and `cost` run ext2probe's rows on the fixture volume the
 * host's mke2fs wrote; `rewrite`, `append`, `setid`, `lchown`, `dirent` and
 * `zombie` each boot os-real on a fresh copy of its ext2 root; and `repair`
 * takes one copy of that root through a kill, the mount's check, a lost
 * block, a fault, recovery's e2fsck and a clean boot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The first partition of the probe and root disks, at sector 2048, and the
 * root disk's second, at sector 198656, as debugfs and e2fsck open them. */
#define PART1 "?offset=1048576"
#define PART2 "?offset=101711872"

/* The byte of the root disk that has the state field of the first
 * partition's superblock: 1 when unmounted cleanly, 2 with an error. */
#define STATE_AT (1048576L + 1024 + 58)

/* The line /proc/mounts gives the ext2 root mounted writable. */
static const char rw_line[] = "/dev/disk0p1 / ext2 rw,relatime 0 0";

/* The probes of the `probe-NAME` cases, by NAME and their path on the root. */
struct probe {
    const char *name;
    const char *path;
};

static const struct probe probes[] = {
    {"shelltest", "/shelltest"},     {"limitprobe", "/limitprobe"},
    {"dirprobe", "/dirprobe"},       {"unlinkdirprobe", "/unlinkdirprobe"},
    {"mvtest", "/mvtest"},           {"vfsprobe", "/vfsprobe"},
    {"dnstest", "/dnstest"},         {"devprobe", "/devprobe"},
    {"orphanprobe", "/orphanprobe"}, {"modeprobe", "/modeprobe"},
    {"mapmusl", "/musl/mapmusl"},
};

/*
 * The cases of 50 writes of 4 KiB to a file on the root, counted by the
 * disk's commands in /proc/disk. The shell's `ok` stays 1 only while every
 * write and sync succeeds and the file has its length, and the last line
 * prints `KEY=N`, the commands, only then.
 */
struct count {
    const char *name;
    const char *lines[4];
    const char *ends[4];
    const char *key;
    long most;
};

static const struct count counts[] = {
    {"rewrite",
     {"ok=1; dd if=/dev/zero of=/root/cost.bin bs=4096 count=1 2>/dev/null && sync || ok=0; "
      "echo MADE\"\"END\r",
      "a=$(grep ^disk-commands /proc/disk | cut -d' ' -f2); i=0; echo COST\"\"START\r",
      "while [ $i -lt 50 ]; do dd if=/dev/zero of=/root/cost.bin bs=4096 count=1 conv=notrunc "
      "2>/dev/null || ok=0; i=$((i+1)); done; sync || ok=0; echo COST\"\"WROTE\r",
      "[ $(wc -c < /root/cost.bin) -eq 4096 ] || ok=0; b=$(grep ^disk-commands /proc/disk | "
      "cut -d' ' -f2); [ $ok -eq 1 ] && echo COST=$((b-a)); echo COST\"\"DONE\r"},
     {"MADEEND", "COSTSTART", "COSTWROTE", "COSTDONE"},
     "COST=",
     250},
    {"append",
     {"ok=1; rm -f /root/grow.bin; sync; echo READY\"\"NOW\r",
      "a=$(grep ^disk-commands /proc/disk | cut -d' ' -f2); i=0; echo APPEND\"\"START\r",
      "while [ $i -lt 50 ]; do dd if=/dev/zero bs=4096 count=1 2>/dev/null >> /root/grow.bin || "
      "ok=0; i=$((i+1)); done; sync || ok=0; echo APPEND\"\"WROTE\r",
      "[ $(wc -c < /root/grow.bin) -eq 204800 ] || ok=0; b=$(grep ^disk-commands /proc/disk | "
      "cut -d' ' -f2); [ $ok -eq 1 ] && echo APPEND=$((b-a)); echo APPEND\"\"DONE\r"},
     {"READYNOW", "APPENDSTART", "APPENDWROTE", "APPENDDONE"},
     "APPEND=",
     550},
};

/* The cases of a test program run on the root by its full path, which must
 * print `ok` and never `fail` before the shell prints `end`. */
struct row {
    const char *name;
    const char *line;
    const char *end;
    const char *ok;
    const char *fail;
};

static const struct row rows[] = {
    {"setid", "/modeprobe /tmp; echo MODE\"\"END\r", "MODEEND", "MODEPROBE OK", "MODEPROBE FAIL"},
    {"lchown", "/lchownmusl /tmp; echo LCHOWN\"\"END\r", "LCHOWNEND", "LCHOWNMUSL OK (links)",
     "LCHOWNMUSL FAIL"},
    {"dirent", "/direntmusl /tmp; echo DIRENT\"\"END\r", "DIRENTEND", "DIRENTMUSL OK (links)",
     "DIRENTMUSL FAIL"},
    {"zombie", "cd /tmp && /zombieprobe; echo ZOMBIE\"\"END\r", "ZOMBIEEND", "ZOMBIEPROBE OK",
     "ZOMBIEPROBE FAIL"},
};

/* What the mkfs step types: mke2fs and mkfs.vfat make a volume each in a
 * file on the second partition, which a loop device mounts writable. */
static const char mkfs_ext2[] =
    "mkdir -p /mnt/p2 /mnt/v && mount -t ext2 /dev/disk0p2 /mnt/p2 && mke2fs -q -F -L made "
    "/mnt/p2/v.ext2 8M && loopctl attach 0 /mnt/p2/v.ext2 && mount -t ext2 /dev/loop0 /mnt/v && "
    "grep loop0 /proc/mounts; echo EXT\"\"END\r";
static const char mkfs_read[] =
    "echo made > /mnt/v/f && umount /mnt/v && mount -t ext2 /dev/loop0 /mnt/v && "
    "echo GOT=$(cat /mnt/v/f) && umount /mnt/v && loopctl detach 0; echo READ\"\"END\r";
static const char mkfs_vfat[] =
    ": > /mnt/p2/v.fat && mkfs.vfat -n RACH2 /mnt/p2/v.fat 14336 && loopctl attach 0 "
    "/mnt/p2/v.fat && mount -t vfat /dev/loop0 /mnt/v && grep loop0 /proc/mounts; "
    "echo FAT\"\"END\r";
static const char mkfs_done[] =
    "echo made > /mnt/v/f && umount /mnt/v && loopctl detach 0 && umount /mnt/p2 && sync && "
    "echo MKFS\"\"DONE; echo MKFS\"\"END\r";

/* What the repair step types at recovery's shell: a sector of the second
 * partition saved, written through its node, read back and put back. The
 * text is printed by printf with an empty `%s` in it, so that the echo of
 * the line typed is not taken for the sector read back. */
static const char save_line[] =
    "dd if=/dev/disk0p2 of=/tmp/s bs=512 count=1 skip=16383 && printf 'rach2 wrote this "
    "sector%s' '' | dd of=/tmp/w bs=512 count=1 conv=sync 2>/dev/null && echo SAVED\"\"IT; "
    "echo SAVE\"\"END\r";
static const char back_line[] =
    "dd if=/tmp/w of=/dev/disk0p2 bs=512 count=1 seek=16383 && dd if=/dev/disk0p2 bs=512 count=1 "
    "skip=16383 | head -c 23 && dd if=/tmp/s of=/dev/disk0p2 bs=512 count=1 seek=16383 && "
    "echo BACK\"\"OK; echo BACK\"\"END\r";

/* One boot of a machine, as the scenario drives it. */
struct boot {
    struct etude_case_machine *m;
    char console[48]; /* the evidence of its console, MACHINE.N.console */
    uint64_t at;      /* the offset of the console read so far */
};

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Read the first line of the artifact `demo-password` into `buf` of `size`
 * bytes, followed by a carriage return, as it is typed. @return 1 when it
 * was read.
 */
static int password(struct lc *l, char *buf, size_t size)
{
    const char *path = etude_artifact(l->c, "demo-password");
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    size_t n;

    if (f == NULL || fgets(buf, (int)size - 1, f) == NULL || (n = strcspn(buf, "\r\n")) == 0) {
        if (f != NULL)
            fclose(f);
        lc_halt(l, ETUDE_FAILED, "the read of the demo password");
        return 0;
    }
    fclose(f);
    buf[n] = '\r';
    buf[n + 1] = '\0';
    return 1;
}

/**
 * Read bytes `from` to `to` of the console that the evidence `console`
 * keeps.
 *
 * @return the bytes, NUL-ended, which the caller frees; or NULL, after
 *         which the scenario cannot go on.
 */
static char *window(struct lc *l, const char *console, uint64_t from, uint64_t to)
{
    const char *path = etude_evidence(l->c, console);
    FILE *f = path != NULL && from <= to ? fopen(path, "rb") : NULL;
    size_t n = (size_t)(to - from);
    char *buf = NULL;

    if (f != NULL && fseek(f, (long)from, SEEK_SET) == 0 && (buf = malloc(n + 1)) != NULL &&
        fread(buf, 1, n, f) == n) {
        buf[n] = '\0';
    } else {
        free(buf);
        buf = NULL;
        lc_halt(l, ETUDE_FAILED, "the read of the console's evidence");
    }
    if (f != NULL)
        fclose(f);
    return buf;
}

/**
 * Boot machine `machine` of the step for its `n`th time and wait for a
 * shell: os-real's machine `root` is logged in to as root at the console,
 * and the recovery machines, which need no login, are waited on for their
 * prompt.
 *
 * @return 1 when the shell's prompt came.
 */
static int start(struct lc *l, const char *machine, int n, struct boot *b)
{
    char secret[256];

    b->at = 0;
    snprintf(b->console, sizeof(b->console), "%s.%d.console", machine, n);
    if ((b->m = lc_boot(l, machine)) == NULL)
        return 0;
    if (strcmp(machine, "root") != 0)
        return lc_wait(l, b->m, "recovery image", &b->at, NULL) &&
               lc_wait(l, b->m, "~ #", &b->at, NULL);
    return lc_wait(l, b->m, "rach2 login:", &b->at, NULL) && lc_type(l, b->m, "root\r") &&
           lc_wait(l, b->m, "Password:", &b->at, NULL) && password(l, secret, sizeof(secret)) &&
           lc_type(l, b->m, secret) && lc_wait(l, b->m, "~ #", &b->at, NULL);
}

/**
 * Type `line` at the shell of boot `b` and wait for `end`, which the line
 * prints last whatever it found.
 *
 * @return the console from the typing to `end`, NUL-ended, which the caller
 *         frees; or NULL when the scenario cannot go on.
 */
static char *run(struct lc *l, struct boot *b, const char *line, const char *end)
{
    uint64_t from = b->at;

    if (!lc_ok(l) || !lc_type(l, b->m, line) || !lc_wait(l, b->m, end, &b->at, NULL))
        return NULL;
    return window(l, b->console, from, b->at);
}

/**
 * Report assertion `id` from `w`, what a command printed: it holds when
 * `text` is there. A NULL `w`, after which the scenario cannot go on,
 * reports nothing.
 */
static void seen(struct lc *l, const char *w, const char *id, const char *text)
{
    char why[160];

    if (w == NULL)
        return;
    snprintf(why, sizeof(why), "the console never says '%s'", text);
    lc_assert(l, id, strstr(w, text) != NULL, why);
}

/**
 * Type `line` at boot `b`, wait for `end` and report assertion `id` from
 * what it printed, as `seen` does.
 */
static void expect(struct lc *l, struct boot *b, const char *line, const char *end, const char *id,
                   const char *text)
{
    char *w = run(l, b, line, end);

    seen(l, w, id, text);
    free(w);
}

/**
 * Power the machine of boot `b` off from its shell, wait for the kernel's
 * goodbye and end the machine.
 *
 * @return the console from the typing to the goodbye, which the caller
 *         frees; or NULL when the scenario cannot go on.
 */
static char *poweroff(struct lc *l, struct boot *b)
{
    char *w = run(l, b, "poweroff\r", "Goodbye");

    if (w != NULL && !lc_quit(l, b->m, NULL)) {
        free(w);
        w = NULL;
    }
    return w;
}

/**
 * Find `key` in `text` at the start of a line, followed by a decimal number,
 * which is given in `*value`. @return 1 when it was found.
 */
static int numbered(const char *text, const char *key, long *value)
{
    size_t len = strlen(key);
    const char *p = text;
    char *end;

    while ((p = strstr(p, key)) != NULL) {
        if ((p == text || p[-1] == '\n') && p[len] >= '0' && p[len] <= '9') {
            *value = strtol(p + len, &end, 10);
            return 1;
        }
        p += len;
    }
    return 0;
}

/**
 * Run host tool `argv[0]`, keeping its output as evidence `output`, and give
 * its end in `end`.
 *
 * @return 1 when it ran, whatever its status; 0, after which the scenario
 *         cannot go on, when it did not.
 */
static int host(struct lc *l, char *const argv[], const char *output, struct etude_termination *end)
{
    enum etude_status st;

    if (!lc_ok(l))
        return 0;
    end->end = ETUDE_END_LOST;
    end->value = -1;
    st = etude_host(l->c, argv, output, end);
    if (st != ETUDE_OK)
        lc_halt(l, st, argv[0]);
    return st == ETUDE_OK;
}

/**
 * Give in `out` the volume at `part`, PART1 or PART2, of the private copy
 * of disk `disk`, as debugfs and e2fsck open it. @return 1 when the copy
 * was made.
 */
static int volume(struct lc *l, const char *disk, const char *part, char *out, size_t size)
{
    const char *copy = etude_private_copy(l->c, disk);
    size_t n = copy != NULL ? strlen(copy) : 0;

    if (copy == NULL || n + strlen(part) >= size) {
        lc_halt(l, ETUDE_REFUSED, "the copy of the disk");
        return 0;
    }
    memcpy(out, copy, n);
    memcpy(out + n, part, strlen(part) + 1);
    return 1;
}

/**
 * Run debugfs's `request` on the volume `vol`, writable when `writes` is
 * set, keeping its output as evidence `output`. debugfs exits 0 whether
 * the request did what it asked or not; the caller judges what it left.
 *
 * @return 1 when it ran.
 */
static int debugfs(struct lc *l, int writes, const char *request, const char *vol,
                   const char *output)
{
    char name[] = "debugfs", w[] = "-w", r[] = "-R", req[2200], at[1100];
    char *with[] = {name, w, r, req, at, NULL};
    char *without[] = {name, r, req, at, NULL};
    struct etude_termination end;

    snprintf(req, sizeof(req), "%s", request);
    snprintf(at, sizeof(at), "%s", vol);
    return host(l, writes ? with : without, output, &end);
}

/**
 * Give a copy of the path of evidence `name` in `out`. @return 1 when it
 * fits.
 */
static int evidence(struct lc *l, const char *name, char *out, size_t size)
{
    const char *path = etude_evidence(l->c, name);

    if (path == NULL || strlen(path) >= size) {
        lc_halt(l, ETUDE_REFUSED, "the path of the evidence");
        return 0;
    }
    snprintf(out, size, "%s", path);
    return 1;
}

/**
 * Read file `path` whole. @return its bytes, NUL-ended, with the count in
 * `*n` when `n` is not NULL, which the caller frees; or NULL.
 */
static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    size_t len = 0, cap = 0, got;
    char *buf = NULL, *grown;

    if (f == NULL)
        return NULL;
    do {
        if (len == cap) {
            cap = cap ? 2 * cap : 65536;
            if ((grown = realloc(buf, cap + 1)) == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = grown;
        }
        got = fread(buf + len, 1, cap - len, f);
        len += got;
    } while (got > 0);
    fclose(f);
    if (buf == NULL && (buf = malloc(1)) == NULL)
        return NULL;
    buf[len] = '\0';
    if (n != NULL)
        *n = len;
    return buf;
}

/**
 * Say whether a line of `text` begins with `key` and has, after the spaces
 * that follow it, `value` and nothing else.
 */
static int field(const char *text, const char *key, const char *value)
{
    size_t klen = strlen(key), vlen = strlen(value);
    const char *p = text, *v;

    while ((p = strstr(p, key)) != NULL) {
        if (p == text || p[-1] == '\n') {
            for (v = p + klen; *v == ' '; v++) {
            }
            if (strncmp(v, value, vlen) == 0 && (v[vlen] == '\n' || v[vlen] == '\0'))
                return 1;
        }
        p += klen;
    }
    return 0;
}

/**
 * Report assertion `id`: the state field of the first partition's
 * superblock on the root disk's copy is `want`.
 */
static void state(struct lc *l, const char *id, int want)
{
    const char *copy = etude_private_copy(l->c, "root-disk");
    FILE *f = copy != NULL ? fopen(copy, "rb") : NULL;
    unsigned char b[2];
    char why[96];
    int have = -1;

    if (!lc_ok(l))
        return;
    if (f != NULL && fseek(f, STATE_AT, SEEK_SET) == 0 && fread(b, 1, 2, f) == 2)
        have = b[0] | b[1] << 8;
    if (f != NULL)
        fclose(f);
    snprintf(why, sizeof(why), "the superblock's state is %d, not %d", have, want);
    lc_assert(l, id, have == want, why);
}

/**
 * Run the host's `e2fsck -fn` on the root disk's first partition, keeping
 * its output as evidence `output`, and set `*said` when `text`, if not
 * NULL, is in what it wrote.
 *
 * @return its exit status; -1 when it did not exit; -2 when it did not run,
 *         after which the scenario cannot go on.
 */
static int e2fsck(struct lc *l, const char *output, const char *text, int *said)
{
    char name[] = "e2fsck", flags[] = "-fn", vol[1100], err[64];
    char *argv[] = {name, flags, vol, NULL};
    char *out, *errs;
    struct etude_termination end;

    if (!lc_ok(l) || !volume(l, "root-disk", PART1, vol, sizeof(vol)) ||
        !host(l, argv, output, &end))
        return -2;
    if (text != NULL) {
        snprintf(err, sizeof(err), "%s.err", output);
        out = slurp(etude_evidence(l->c, output), NULL);
        errs = slurp(etude_evidence(l->c, err), NULL);
        *said = (out != NULL && strstr(out, text) != NULL) ||
                (errs != NULL && strstr(errs, text) != NULL);
        free(out);
        free(errs);
    }
    return end.end == ETUDE_END_EXITED ? end.value : -1;
}

/**
 * Report assertion `id`: `status`, what e2fsck gave, is `want`. A status of
 * -2, e2fsck not run, reports nothing.
 */
static void fsck_status(struct lc *l, const char *id, int status, int want)
{
    char why[96];

    if (status == -2)
        return;
    snprintf(why, sizeof(why), "the host's e2fsck -fn exits %d, not %d", status, want);
    lc_assert(l, id, status == want, why);
}

/**
 * Give in `*h` the FNV-1a hash of every byte of the root disk's copy, by
 * which the scenario tells whether a boot wrote it. @return 1 when it was
 * read.
 */
static int digest(struct lc *l, unsigned long long *h)
{
    const char *copy = etude_private_copy(l->c, "root-disk");
    FILE *f = copy != NULL ? fopen(copy, "rb") : NULL;
    static unsigned char buf[1 << 20];
    size_t n, i;

    *h = 14695981039346656037ULL;
    if (f == NULL) {
        lc_halt(l, ETUDE_FAILED, "the read of the root disk's copy");
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        for (i = 0; i < n; i++)
            *h = (*h ^ buf[i]) * 1099511628211ULL;
    fclose(f);
    return 1;
}

/**
 * Copy the root disk's copy, byte for byte, into the empty disk `root-copy`
 * of the same size, leaving its blocks of zeros unwritten, so that the
 * repair boot has a second disk as damaged as the first. @return 1 when it
 * was copied.
 */
static int copy_disk(struct lc *l)
{
    const char *from = etude_private_copy(l->c, "root-disk");
    const char *to = etude_private_copy(l->c, "root-copy");
    FILE *in = from != NULL ? fopen(from, "rb") : NULL;
    FILE *out = to != NULL ? fopen(to, "r+b") : NULL;
    static unsigned char buf[1 << 20];
    size_t n, i;
    int ok = in != NULL && out != NULL;

    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
        for (i = 0; i < n && buf[i] == 0; i++) {
        }
        if (i == n)
            ok = fseek(out, (long)n, SEEK_CUR) == 0;
        else
            ok = fwrite(buf, 1, n, out) == n;
    }
    if (in != NULL && ferror(in))
        ok = 0;
    if (in != NULL)
        fclose(in);
    if (out != NULL && fclose(out) != 0)
        ok = 0;
    if (!ok)
        lc_halt(l, ETUDE_FAILED, "the copy of the root disk into the second disk");
    return ok;
}

/**
 * Report assertion `staged`: the probe `path` that debugfs reads from the
 * ext2 volume of the probe disk's copy is the built one, the artifact
 * `probe`, byte for byte.
 */
static void staged(struct lc *l, const char *path)
{
    const char *built = etude_artifact(l->c, "probe");
    char vol[1100], out[1024], request[1200];
    char *have = NULL, *want = NULL;
    size_t hn = 0, wn = 0;

    if (built == NULL) {
        lc_halt(l, ETUDE_REFUSED, "the artifact probe");
        return;
    }
    if (!volume(l, "probes-disk", PART1, vol, sizeof(vol)) ||
        !evidence(l, "probe.staged", out, sizeof(out)))
        return;
    snprintf(request, sizeof(request), "dump \"%s\" \"%s\"", path, out);
    if (!debugfs(l, 0, request, vol, "dump"))
        return;
    have = slurp(out, &hn);
    want = slurp(built, &wn);
    lc_assert(l, "staged", have != NULL && want != NULL && hn == wn && memcmp(have, want, hn) == 0,
              have == NULL ? "debugfs did not read the probe from the volume"
                           : "the probe on the volume is not the built one");
    free(have);
    free(want);
}

/**
 * Run case `probe-NAME`: the probe at `path` on the volume, then the probe
 * run as init from the debug shell of a boot on that volume as its root.
 * Assertions `staged` and `exit`.
 */
static void probe(struct lc *l, const char *path)
{
    struct etude_case_machine *m;
    char command[64];
    uint64_t at;

    if (lc_step(l, "staged"))
        staged(l, path);
    snprintf(command, sizeof(command), "exec %s\r", path);
    if ((m = lc_shell(l, "probe", "probe", &at)) == NULL)
        return;
    free(lc_run(l, m, "probe", command, &at, "exit"));
    lc_quit(l, m, NULL);
}

/**
 * Print each line of `w` that has `key`, for a person, without its line
 * ending.
 */
static void print_lines(const char *w, const char *key)
{
    const char *p = w, *eol;
    int len;

    while ((p = strstr(p, key)) != NULL) {
        eol = p + strcspn(p, "\r\n");
        len = (int)(eol - p);
        printf("%.*s\n", len, p);
        p = eol;
    }
}

/**
 * Run case `links` or `cost`: ext2probe's rows of `mode` on the fixture
 * volume, mounted from the IDE disk beside the test image. Assertions
 * `exit`, `said-ok` and, for `links`, `mounted`.
 */
static void fixture(struct lc *l, const char *mode)
{
    struct etude_case_machine *m;
    char command[48], ok[48], why[96];
    uint64_t at;
    char *w;

    snprintf(command, sizeof(command), "exec /ext2probe %s\r", mode);
    snprintf(ok, sizeof(ok), "EXT2PROBE OK (%s)", mode);
    if ((m = lc_shell(l, "probe", "fixture", &at)) == NULL)
        return;
    w = lc_run(l, m, "fixture", command, &at, "exit");
    if (w != NULL) {
        snprintf(why, sizeof(why), "ext2probe did not say %s alone", ok);
        lc_assert(l, "said-ok", strstr(w, ok) != NULL && strstr(w, "EXT2PROBE FAIL") == NULL, why);
        if (strcmp(mode, "links") == 0)
            seen(l, w, "mounted", "ext2: mounted: ");
        else
            print_lines(w, "ext2-cost: result: ");
        free(w);
    }
    lc_quit(l, m, NULL);
}

/**
 * Run case `rewrite` or `append`: fifty writes of 4 KiB on the root, each
 * counted by the disk's commands. Assertions `writes`, that the writes,
 * their sync and the file's length were as asked, and `commands`, that they
 * took at most `k->most` disk commands, which /proc/disk counts.
 */
static void count(struct lc *l, const struct count *k)
{
    struct boot b;
    char *w = NULL, why[128];
    long n = -1;
    size_t i;
    int found;

    if (!lc_step(l, "row") || !start(l, "root", 1, &b))
        return;
    for (i = 0; i < 4 && lc_ok(l); i++) {
        free(w);
        w = run(l, &b, k->lines[i], k->ends[i]);
    }
    if (w != NULL) {
        found = numbered(w, k->key, &n);
        snprintf(why, sizeof(why), "a write, the sync or the file's length failed, so no %s",
                 k->key);
        lc_assert(l, "writes", found, why);
        snprintf(why, sizeof(why), "50 writes of 4 KiB took %ld disk commands, over %ld", n,
                 k->most);
        lc_assert(l, "commands", found && n >= 0 && n <= k->most, why);
        if (found)
            printf("ext2: %s: 50 writes of 4 KiB took %ld disk commands\n", k->name, n);
        free(w);
    }
    lc_quit(l, b.m, NULL);
}

/**
 * Run case `setid`, `lchown`, `dirent` or `zombie`: test program `r` run on
 * the root, which must say OK. Assertion `said-ok`, and for `zombie`,
 * `unmounted`: the poweroff after it reaches the kernel's goodbye with the
 * root unmounted.
 */
static void row(struct lc *l, const struct row *r)
{
    struct boot b;
    char why[96];
    char *w;

    if (!lc_step(l, "row") || !start(l, "root", 1, &b))
        return;
    w = run(l, &b, r->line, r->end);
    if (w != NULL) {
        snprintf(why, sizeof(why), "the program did not say %s alone", r->ok);
        lc_assert(l, "said-ok", strstr(w, r->ok) != NULL && strstr(w, r->fail) == NULL, why);
        free(w);
    }
    if (strcmp(r->name, "zombie") != 0) {
        lc_quit(l, b.m, NULL);
        return;
    }
    w = run(l, &b, "cd / && poweroff\r", "Goodbye");
    seen(l, w, "unmounted", "shutdown: root: unmounted");
    free(w);
    lc_quit(l, b.m, NULL);
}

/**
 * The first boot of case `repair`: the root mounts writable and takes a
 * write, its partition node refuses one, and the machine is ended with the
 * root unclean. Assertions `first-mounted`, `first-write`, `node-refused`,
 * `root-before-usb` and `first-unchecked`.
 */
static void first(struct lc *l)
{
    uint64_t root = 0, usb = 0;
    struct boot b;

    if (!lc_step(l, "first") || !start(l, "root", 1, &b))
        return;
    expect(l, &b, "cat /proc/mounts; echo MOUNTS\"\"END\r", "MOUNTSEND", "first-mounted", rw_line);
    expect(l, &b, "echo first > /root/first && sync && echo WRITE\"\"OK; echo WRITE\"\"END\r",
           "WRITEEND", "first-write", "WRITEOK");
    expect(l, &b, "dd if=/dev/zero of=/dev/disk0p2 bs=512 count=1; echo DD\"\"END\r", "DDEND",
           "node-refused", "Operation not permitted");
    if (!lc_quit(l, b.m, NULL))
        return;
    lc_assert(l, "root-before-usb",
              etude_console_find(b.m, "ext2: root: /dev/disk0p1", 0, &root) == ETUDE_OK &&
                  etude_console_find(b.m, "usb: started", 0, &usb) == ETUDE_OK && root < usb,
              "the root was not mounted before the USB stack started");
    lc_find(l, b.m, "first-unchecked", "ext2: check", 0, 0);
}

/**
 * The mkfs boot of case `repair`, then the host's reading of the volumes it
 * made in files on the second partition. Assertions `loop-ext2`,
 * `loop-ext2-read`, `loop-vfat`, `loop-done`, `made-e2fsck`,
 * `made-features`, `made-block-size`, `made-label` and `made-fat-file`.
 */
static void mkfs(struct lc *l)
{
    char vol[1100], ext2[1024], fat[1024], request[2200];
    char e2fsck[] = "e2fsck", fn[] = "-fn", mtype[] = "mtype", i[] = "-i", file[] = "::/f";
    char *check[] = {e2fsck, fn, ext2, NULL};
    char *type[] = {mtype, i, fat, file, NULL};
    struct etude_termination end;
    struct boot b;
    char *text;
    size_t n;

    if (!lc_step(l, "mkfs") || !start(l, "root", 2, &b))
        return;
    expect(l, &b, mkfs_ext2, "EXTEND", "loop-ext2", "/dev/loop0 /mnt/v ext2 rw");
    expect(l, &b, mkfs_read, "READEND", "loop-ext2-read", "GOT=made");
    expect(l, &b, mkfs_vfat, "FATEND", "loop-vfat", "/dev/loop0 /mnt/v vfat rw");
    expect(l, &b, mkfs_done, "MKFSEND", "loop-done", "MKFSDONE");
    if (!lc_quit(l, b.m, NULL) || !lc_step(l, "made") ||
        !volume(l, "root-disk", PART2, vol, sizeof(vol)) ||
        !evidence(l, "made.ext2", ext2, sizeof(ext2)) || !evidence(l, "made.fat", fat, sizeof(fat)))
        return;
    snprintf(request, sizeof(request), "dump /v.ext2 \"%s\"", ext2);
    if (!debugfs(l, 0, request, vol, "dump-ext2"))
        return;
    snprintf(request, sizeof(request), "dump /v.fat \"%s\"", fat);
    if (!debugfs(l, 0, request, vol, "dump-fat") || !host(l, check, "made.e2fsck", &end))
        return;
    lc_assert(l, "made-e2fsck", end.end == ETUDE_END_EXITED && end.value == 0,
              "the host's e2fsck -fn does not pass the volume mke2fs made");
    if (!debugfs(l, 0, "stats", ext2, "made.stats"))
        return;
    text = slurp(etude_evidence(l->c, "made.stats"), NULL);
    lc_assert(l, "made-features",
              text != NULL && field(text, "Filesystem features:",
                                    "resize_inode filetype sparse_super large_file"),
              "debugfs gives other features");
    lc_assert(l, "made-block-size", text != NULL && field(text, "Block size:", "4096"),
              "debugfs gives another block size");
    lc_assert(l, "made-label", text != NULL && field(text, "Filesystem volume name:", "made"),
              "debugfs gives another label");
    free(text);
    if (!host(l, type, "made.mtype", &end))
        return;
    text = slurp(etude_evidence(l->c, "made.mtype"), &n);
    while (text != NULL && n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r'))
        text[--n] = '\0';
    lc_assert(l, "made-fat-file",
              end.end == ETUDE_END_EXITED && end.value == 0 && text != NULL &&
                  strcmp(text, "made") == 0,
              "mtype does not read made from the file on the FAT volume");
    free(text);
}

/**
 * The boot after the kill in case `repair`: the dirty root is checked,
 * keeps the first boot's write, mounts writable and unmounts clean.
 * Assertions `killed-dirty`, `dirty-kept`, `dirty-mounted`,
 * `dirty-unmounted`, `dirty-checked`, `dirty-valid` and `dirty-e2fsck`.
 */
static void dirty(struct lc *l)
{
    struct boot b;
    char *w;

    if (!lc_step(l, "dirty"))
        return;
    state(l, "killed-dirty", 0);
    if (!start(l, "root", 3, &b))
        return;
    expect(l, &b, "echo GOT=$(cat /root/first); echo GOT\"\"END\r", "GOTEND", "dirty-kept",
           "GOT=first");
    expect(l, &b, "cat /proc/mounts; echo MOUNTS\"\"END\r", "MOUNTSEND", "dirty-mounted", rw_line);
    w = poweroff(l, &b);
    seen(l, w, "dirty-unmounted", "shutdown: root: unmounted");
    free(w);
    lc_find(l, b.m, "dirty-checked", "ext2: check: clean", 0, 1);
    state(l, "dirty-valid", 1);
    fsck_status(l, "dirty-e2fsck", e2fsck(l, "dirty.e2fsck", NULL, NULL), 0);
}

/**
 * The lost block of case `repair`: a block marked used that no file has,
 * with the valid bit clear, mounts writable with the error bit set and is
 * left to e2fsck. Assertions `lost-mounted`, `lost-write`, `lost-record`,
 * `lost-error-bit`, `lost-e2fsck` and `lost-bitmap`.
 */
static void lost(struct lc *l)
{
    char vol[1100];
    struct boot b;
    int status, said = 0;

    if (!lc_step(l, "lose") || !volume(l, "root-disk", PART1, vol, sizeof(vol)) ||
        !debugfs(l, 1, "setb 20000", vol, "setb") || !debugfs(l, 1, "ssv state 0", vol, "ssv") ||
        !lc_step(l, "lost") || !start(l, "root", 4, &b))
        return;
    expect(l, &b, "cat /proc/mounts; echo MOUNTS\"\"END\r", "MOUNTSEND", "lost-mounted", rw_line);
    expect(l, &b, "echo again > /root/again && sync && echo WRITE\"\"OK; echo WRITE\"\"END\r",
           "WRITEEND", "lost-write", "WRITEOK");
    free(poweroff(l, &b));
    lc_find(l, b.m, "lost-record", "ext2: check: 1 lost blocks, 0 orphan inodes, kept for e2fsck",
            0, 1);
    state(l, "lost-error-bit", 2);
    status = e2fsck(l, "lost.e2fsck", "Block bitmap differences", &said);
    fsck_status(l, "lost-e2fsck", status, 4);
    if (status != -2)
        lc_assert(l, "lost-bitmap", said, "e2fsck does not name the block bitmap's differences");
}

/**
 * Say whether `text` has the kernel's record that its check found an inode
 * that the tree reaches and the bitmap calls free, `ext2: check: inode N: a
 * reached inode the bitmap calls free`.
 */
static int free_reached(const char *text)
{
    static const char head[] = "ext2: check: inode ", tail[] = ": a reached inode the bitmap "
                                                               "calls free";
    const char *p = text, *q;

    while ((p = strstr(p, head)) != NULL) {
        for (q = p + strlen(head); *q >= '0' && *q <= '9'; q++) {
        }
        if (q > p + strlen(head) && strncmp(q, tail, strlen(tail)) == 0)
            return 1;
        p += strlen(head);
    }
    return 0;
}

/**
 * The fault of case `repair`: with an inode in use marked free, the root
 * mounts read-only, names the fault and refuses a write. Assertions
 * `fault-read-only`, `fault-refused`, `fault-record` and `fault-reason`.
 */
static void fault(struct lc *l)
{
    char vol[1100], *w;
    struct boot b;

    if (!lc_step(l, "damage") || !volume(l, "root-disk", PART1, vol, sizeof(vol)) ||
        !debugfs(l, 1, "freei /etc/hostname", vol, "freei") || !lc_step(l, "fault") ||
        !start(l, "root", 5, &b))
        return;
    expect(l, &b, "cat /proc/mounts; echo MOUNTS\"\"END\r", "MOUNTSEND", "fault-read-only",
           "/dev/disk0p1 / ext2 ro,relatime 0 0");
    expect(l, &b, "echo x > /root/x; echo X\"\"END\r", "XEND", "fault-refused",
           "Read-only file system");
    free(poweroff(l, &b));
    if ((w = window(l, b.console, 0, b.at)) == NULL)
        return;
    lc_assert(l, "fault-record", free_reached(w),
              "the kernel did not name the inode the bitmap calls free");
    seen(l, w, "fault-reason", "ext2: mounted read-only: the check found a fault");
    free(w);
}

/**
 * The recovery boots of case `repair`. Without rach2.fsck the boot reaches
 * its shell with the disk as it was; with it, init runs e2fsck -p on every
 * ext2 partition of both disks, the second a copy of the first, and the
 * partition node takes a sector's write. Assertions `look-same`,
 * `look-no-e2fsck`, `repair-root`, `repair-second`, `repair-copy`,
 * `node-writes`, `repair-e2fsck`, `repair-valid` and `repair-changed`.
 */
static void recover(struct lc *l)
{
    unsigned long long before, after;
    struct boot b;
    char *saved, *back;

    if (!lc_step(l, "look") || !digest(l, &before) || !start(l, "look", 1, &b))
        return;
    free(run(l, &b, "echo LOOK\"\"DONE\r", "LOOKDONE"));
    if (!lc_quit(l, b.m, NULL) || !digest(l, &after))
        return;
    lc_assert(l, "look-same", before == after,
              "a recovery boot without rach2.fsck changed the disk");
    lc_find(l, b.m, "look-no-e2fsck", "init: e2fsck", 0, 0);
    if (!lc_step(l, "repair") || !copy_disk(l) || !start(l, "repair", 1, &b))
        return;
    saved = run(l, &b, save_line, "SAVEEND");
    back = run(l, &b, back_line, "BACKEND");
    if (saved != NULL && back != NULL)
        lc_assert(l, "node-writes",
                  strstr(saved, "SAVEDIT") != NULL &&
                      strstr(back, "rach2 wrote this sector") != NULL &&
                      strstr(back, "BACKOK") != NULL,
                  "the sector did not go through the partition node and back");
    free(saved);
    free(back);
    if (!lc_quit(l, b.m, NULL))
        return;
    lc_find(l, b.m, "repair-root", "init: e2fsck: /dev/disk0p1: status 1", 0, 1);
    lc_find(l, b.m, "repair-second", "init: e2fsck: /dev/disk0p2: status 0", 0, 1);
    lc_find(l, b.m, "repair-copy", "init: e2fsck: /dev/disk1p1: status 1", 0, 1);
    fsck_status(l, "repair-e2fsck", e2fsck(l, "repair.e2fsck", NULL, NULL), 0);
    state(l, "repair-valid", 1);
    if (digest(l, &after))
        lc_assert(l, "repair-changed", after != before, "the repair changed nothing on the disk");
}

/**
 * The last boot of case `repair`: the repaired root mounts writable without
 * a check and keeps the write of the lost block's boot. Assertions
 * `clean-mounted`, `clean-kept` and `clean-unchecked`.
 */
static void clean(struct lc *l)
{
    struct boot b;

    if (!lc_step(l, "clean") || !start(l, "root", 6, &b))
        return;
    expect(l, &b, "cat /proc/mounts; echo MOUNTS\"\"END\r", "MOUNTSEND", "clean-mounted", rw_line);
    expect(l, &b, "echo GOT=$(cat /root/again); echo GOT\"\"END\r", "GOTEND", "clean-kept",
           "GOT=again");
    free(poweroff(l, &b));
    lc_find(l, b.m, "clean-unchecked", "ext2: check", 0, 0);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const probe_ids[] = {"staged", "exit", NULL};
    static const char *const links_ids[] = {"exit", "said-ok", "mounted", NULL};
    static const char *const cost_ids[] = {"exit", "said-ok", NULL};
    static const char *const count_ids[] = {"writes", "commands", NULL};
    static const char *const row_ids[] = {"said-ok", NULL};
    static const char *const zombie_ids[] = {"said-ok", "unmounted", NULL};
    static const char *const repair_ids[] = {
        "first-mounted",   "first-write",     "node-refused",    "root-before-usb",
        "first-unchecked", "loop-ext2",       "loop-ext2-read",  "loop-vfat",
        "loop-done",       "made-e2fsck",     "made-features",   "made-block-size",
        "made-label",      "made-fat-file",   "killed-dirty",    "dirty-kept",
        "dirty-mounted",   "dirty-unmounted", "dirty-checked",   "dirty-valid",
        "dirty-e2fsck",    "lost-mounted",    "lost-write",      "lost-record",
        "lost-error-bit",  "lost-e2fsck",     "lost-bitmap",     "fault-read-only",
        "fault-refused",   "fault-record",    "fault-reason",    "look-same",
        "look-no-e2fsck",  "repair-root",     "repair-second",   "repair-copy",
        "node-writes",     "repair-e2fsck",   "repair-valid",    "repair-changed",
        "clean-mounted",   "clean-kept",      "clean-unchecked", NULL};
    const char *name = case_name(c);
    struct lc l;
    size_t i;

    if (strncmp(name, "probe-", 6) == 0) {
        for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
            if (strcmp(name + 6, probes[i].name) == 0) {
                lc_begin(&l, c, probe_ids);
                probe(&l, probes[i].path);
                return lc_finish(&l);
            }
        return 1;
    }
    if (strcmp(name, "links") == 0 || strcmp(name, "cost") == 0) {
        lc_begin(&l, c, strcmp(name, "links") == 0 ? links_ids : cost_ids);
        fixture(&l, name);
        return lc_finish(&l);
    }
    for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
        if (strcmp(name, counts[i].name) == 0) {
            lc_begin(&l, c, count_ids);
            count(&l, &counts[i]);
            return lc_finish(&l);
        }
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        if (strcmp(name, rows[i].name) == 0) {
            lc_begin(&l, c, strcmp(name, "zombie") == 0 ? zombie_ids : row_ids);
            row(&l, &rows[i]);
            return lc_finish(&l);
        }
    if (strcmp(name, "repair") != 0)
        return 1;
    lc_begin(&l, c, repair_ids);
    first(&l);
    mkfs(&l);
    dirty(&l);
    lost(&l);
    fault(&l);
    recover(&l);
    clean(&l);
    return lc_finish(&l);
}
