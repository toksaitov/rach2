/*
 * lib_grow.c: what the scenarios of the grow suite share: commands typed at
 * a recovery shell and judged by their status and output, the host's reading
 * of disk files, and the rehearsal of the installed machine's grow, which
 * runs once for each kind of partition table.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lib_grow.h"

/* The installed machine's boot partition, sectors 2048 to 133119, whose
 * environment the grow writes beside the seed. */
#define BOOT_FIRST 2048
#define BOOT_END   133120

int grow_boot(struct drive *d, struct etude_case *c, const char *machine, const char *const disks[],
              const char *prompt)
{
    drive_boot(d, c, machine, disks);
    drive_expect(d, "recovery image");
    return drive_expect(d, prompt);
}

/** Fail `d` with the text `fmt` gives, unless a call failed before. */
static void grow_fail(struct drive *d, const char *fmt, ...)
{
    va_list ap;

    if (d->failed)
        return;
    d->failed = 1;
    va_start(ap, fmt);
    vsnprintf(d->why, sizeof(d->why), fmt, ap);
    va_end(ap);
}

/**
 * Read bytes `from` to `to` of the console of the boot of `d`, leaving out
 * every carriage return.
 *
 * @return the text, which the caller frees, or NULL.
 */
static char *console_span(const struct drive *d, uint64_t from, uint64_t to)
{
    char name[128], *buf;
    const char *path;
    size_t i, o, len;
    ssize_t r;
    int fd;

    if (to < from)
        return NULL;
    snprintf(name, sizeof(name), "%s.%u.console", d->machine, d->boot);
    path = etude_evidence(d->c, name);
    if (path == NULL || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return NULL;
    len = (size_t)(to - from);
    if ((buf = malloc(len + 1)) == NULL) {
        close(fd);
        return NULL;
    }
    for (i = 0; i < len; i += (size_t)r) {
        r = pread(fd, buf + i, len - i, (off_t)(from + i));
        if (r < 0 && errno == EINTR) {
            r = 0;
            continue;
        }
        if (r <= 0) {
            free(buf);
            close(fd);
            return NULL;
        }
    }
    close(fd);
    for (i = o = 0; i < len; i++)
        if (buf[i] != '\r')
            buf[o++] = buf[i];
    buf[o] = '\0';
    return buf;
}

/** Type command `cmd` as grow_shell says. */
static int shell_line(struct drive *d, const char *tag, char **out, const char *cmd)
{
    char token[64], mark[64], *text, *at, *next;
    uint64_t from = d->at;
    int status = -1;

    if (out != NULL)
        *out = NULL;
    snprintf(token, sizeof(token), "%sEND", tag);
    snprintf(mark, sizeof(mark), "%s=", tag);
    if (drive_send(d, "%s; echo %s\"\"=$? %s\"\"END", cmd, tag, tag) != 0 ||
        drive_expect(d, token) != 0)
        return -1;
    if ((text = console_span(d, from, d->at)) == NULL) {
        grow_fail(d, "the console of \"%.80s\" cannot be read", cmd);
        return -1;
    }
    for (at = NULL, next = text; (next = strstr(next, mark)) != NULL; next++)
        at = next;
    if (at != NULL)
        status = atoi(at + strlen(mark));
    else
        grow_fail(d, "\"%.80s\" printed no status", cmd);
    if (out != NULL)
        *out = text;
    else
        free(text);
    return at != NULL ? status : -1;
}

int grow_shell(struct drive *d, const char *tag, char **out, const char *fmt, ...)
{
    char cmd[3072];
    va_list ap;
    int n;

    if (out != NULL)
        *out = NULL;
    if (d->failed)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        grow_fail(d, "a command longer than %zu bytes", sizeof(cmd) - 1);
        return -1;
    }
    return shell_line(d, tag, out, cmd);
}

int grow_run(struct drive *d, const char *fmt, ...)
{
    char cmd[3072];
    va_list ap;
    int n, status;

    if (d->failed)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        grow_fail(d, "a command longer than %zu bytes", sizeof(cmd) - 1);
        return -1;
    }
    status = shell_line(d, "RUN", NULL, cmd);
    if (status != 0 && !d->failed)
        grow_fail(d, "\"%.100s\" ended with status %d", cmd, status);
    return status == 0 ? 0 : -1;
}

int grow_console_has(const struct drive *d, const char *text)
{
    char *all = drive_console(d);
    int has = all != NULL && strstr(all, text) != NULL;

    free(all);
    return has;
}

int grow_copy_range(const char *from, uint64_t off, uint64_t len, const char *to)
{
    static char buf[1 << 20];
    int in, out, ok = 0;
    ssize_t r, w;
    size_t n, i;

    if (from == NULL || to == NULL || (in = open(from, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) {
        close(in);
        return -1;
    }
    while (len > 0) {
        n = len < sizeof(buf) ? (size_t)len : sizeof(buf);
        r = pread(in, buf, n, (off_t)off);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        for (i = 0; i < (size_t)r; i += (size_t)w) {
            w = write(out, buf + i, (size_t)r - i);
            if (w < 0 && errno == EINTR) {
                w = 0;
                continue;
            }
            if (w <= 0)
                goto done;
        }
        off += (uint64_t)r;
        len -= (uint64_t)r;
    }
    ok = len == 0;
done:
    close(in);
    return close(out) == 0 && ok ? 0 : -1;
}

int grow_write_text(const char *path, const char *text)
{
    FILE *f;
    int ok;

    if (path == NULL || text == NULL || (f = fopen(path, "w")) == NULL)
        return -1;
    ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/** Read up to `n` bytes of `fd` into `buf`; give the count, or -1. */
static ssize_t read_full(int fd, char *buf, size_t n)
{
    size_t got = 0;
    ssize_t r;

    while (got < n) {
        r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

int grow_same(const char *a, const char *b, uint64_t skip_off, uint64_t skip_len, char *why,
              size_t size)
{
    static char ba[1 << 20], bb[1 << 20];
    uint64_t at = 0, i;
    ssize_t ra, rb;
    int fa, fb, same = 0;

    snprintf(why, size, "%s or %s cannot be read", a != NULL ? a : "a file",
             b != NULL ? b : "a file");
    if (a == NULL || b == NULL || (fa = open(a, O_RDONLY | O_CLOEXEC)) < 0)
        return 0;
    if ((fb = open(b, O_RDONLY | O_CLOEXEC)) < 0) {
        close(fa);
        return 0;
    }
    for (;;) {
        ra = read_full(fa, ba, sizeof(ba));
        rb = read_full(fb, bb, sizeof(bb));
        if (ra < 0 || rb < 0)
            break;
        for (i = 0; i < (uint64_t)(ra < rb ? ra : rb); i++) {
            if (ba[i] != bb[i] && !(at + i >= skip_off && at + i < skip_off + skip_len)) {
                snprintf(why, size, "they differ first at byte %" PRIu64 ", sector %" PRIu64,
                         at + i, (at + i) / 512);
                goto done;
            }
        }
        if (ra != rb) {
            snprintf(why, size, "one ends at byte %" PRIu64 " and the other does not",
                     at + (uint64_t)(ra < rb ? ra : rb));
            goto done;
        }
        if (ra == 0) {
            same = 1;
            why[0] = '\0';
            break;
        }
        at += (uint64_t)ra;
    }
done:
    close(fa);
    close(fb);
    return same;
}

int grow_writes_inside(struct etude_case *c, const char *name, const char *disk, const char *seed,
                       const char *ranges, char *why, size_t size)
{
    unsigned long long first[256], end[256], a, n;
    char map_name[64], cover_name[64], *cover = NULL, *list = NULL, *line, *eol;
    const char *map;
    int count = 0, extents = -1, inside = 1, st, k, covered;

    snprintf(map_name, sizeof(map_name), "%s.map", name);
    snprintf(cover_name, sizeof(cover_name), "%s.cover", name);
    map = etude_evidence(c, map_name);
    {
        char *const argv[] = {"qemu-img", "map", "--output=json", (char *)disk, NULL};

        if ((st = drive_host(c, argv, map_name)) != 0) {
            snprintf(why, size, "qemu-img map of the target ended with status %d", st);
            return 0;
        }
    }
    {
        char *const argv[] = {"growset",   "--lba",      "266240", "--map",
                              (char *)map, (char *)seed, "131072", NULL};

        st = drive_host(c, argv, cover_name);
    }
    cover = evidence_text(c, cover_name);
    list = ranges != NULL ? evidence_text(c, ranges) : NULL;
    if (cover == NULL || list == NULL || (st != 0 && strstr(cover, "outside ") == NULL)) {
        snprintf(why, size, "the written extents cannot be read (growset ended with status %d)",
                 st);
        free(cover);
        free(list);
        return 0;
    }
    for (line = list; line != NULL && *line != '\0' && count < 256;
         line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        if (sscanf(line, "%llu %llu", &a, &n) == 2) {
            first[count] = a;
            end[count++] = a + n;
        }
    }
    for (line = cover; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        if (sscanf(line, "coverage: %d extents written,", &extents) == 1)
            continue;
        if (sscanf(line, "outside %llu %llu", &a, &n) != 2)
            continue;
        covered = a >= BOOT_FIRST && a + n <= BOOT_END;
        for (k = 0; k < count && !covered; k++)
            covered = a >= first[k] && a + n <= end[k];
        if (!covered && inside) {
            snprintf(
                why, size,
                "sectors %llu to %llu were written outside the saved ranges and the boot partition",
                a, a + n - 1);
            inside = 0;
        }
    }
    if (inside && extents < 1) {
        snprintf(why, size, "growset checked no written extent");
        inside = 0;
    }
    if (inside && count == 0) {
        snprintf(why, size, "the plan has no ranges");
        inside = 0;
    }
    free(cover);
    free(list);
    return inside;
}

/* The files that `save` writes under the store's saved/, which the host reads
 * back. */
static const char *const saved_files[] = {"backup.bin", "boot.img",   "grow.plan",
                                          "ranges.txt", "SHA256SUMS", "boot.status"};

/**
 * Boot the recovery stick with the target and the store, mount the store and
 * unpack the plan from it, as each boot of grow_machine begins.
 */
static void machine_boot(struct drive *d, struct etude_case *c)
{
    grow_boot(d, c, "recovery", NULL, "# ");
    grow_run(d, "test ! -e /tmp/grow-plan && mkdir -p /mnt/grow /tmp/grow-plan && "
                "mount -t exfat /dev/disk1p1 /mnt/grow && "
                "tar -C /tmp/grow-plan -xf /mnt/grow/grow-tools.tar && "
                "tar -C /tmp/grow-plan -xf /mnt/grow/grow-tests.tar");
}

/** Say why the boot of `d` failed, or that the command ended with `status`. */
static const char *boot_why(const struct drive *d, const char *what, int status, char *buf,
                            size_t size)
{
    if (d->failed)
        return d->why;
    snprintf(buf, size, "%s ended with status %d", what, status);
    return buf;
}

/**
 * Check SHA256SUMS of the saved files, `sums`, as the host does before it
 * lets the grow go on: each line a digest of 64 hexadecimal digits and the
 * name of a file `save` writes. Build in `ack` the command that writes them
 * as host.sha256 for the grow.
 *
 * @return 1 when every line is such, 0 otherwise.
 */
static int sums_form(const char *sums, char *ack, size_t size, char *why, size_t wsize)
{
    char hex[80], file[64];
    const char *line, *eol;
    size_t used;
    int lines = 0, i;

    used = (size_t)snprintf(ack, size, "printf '%%s\\n'");
    for (line = sums; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        if (sscanf(line, "%79s %63s", hex, file) != 2 || strlen(hex) != 64 ||
            strspn(hex, "0123456789abcdef") != 64) {
            snprintf(why, wsize, "a saved checksum is not 64 hexadecimal digits: %.80s", line);
            return 0;
        }
        for (i = 0; i < 4 && strcmp(file, saved_files[i]) != 0; i++)
            ;
        if (i == 4) {
            snprintf(why, wsize, "the saved checksums name %s", file);
            return 0;
        }
        used += (size_t)snprintf(ack + used, used < size ? size - used : 0, " '%s  %s'", hex, file);
        lines++;
    }
    used += (size_t)snprintf(ack + used, used < size ? size - used : 0,
                             " > /mnt/grow/saved/host.sha256");
    if (lines == 0 || used >= size) {
        snprintf(why, wsize,
                 lines == 0 ? "the saved checksums are empty" : "the saved checksums are too long");
        return 0;
    }
    return 1;
}

/**
 * Check each file that `sums` lists against its digest, reading it as the
 * evidence `saved-NAME`, as `shasum -c` would.
 *
 * @return 1 when every file has its digest, 0 otherwise.
 */
static int sums_hold(struct etude_case *c, const char *sums, char *why, size_t size)
{
    char hex[80], file[64], ev[96], got[65];
    const char *line, *eol;
    int lines = 0;

    for (line = sums; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        if (sscanf(line, "%79s %63s", hex, file) != 2)
            continue;
        snprintf(ev, sizeof(ev), "saved-%s", file);
        if (file_digest(etude_evidence(c, ev), 0, UINT64_MAX, got) != 0 || strcmp(got, hex) != 0) {
            snprintf(why, size, "the host's copy of saved/%s does not have its saved checksum",
                     file);
            return 0;
        }
        lines++;
    }
    if (lines == 0)
        snprintf(why, size, "the saved checksums are empty");
    return lines > 0;
}

/**
 * Find the boot environment's sector in the boot partition from the saved
 * bootctl status, `status`: its `grubenv:` line names one cluster on an MBR
 * disk's boot partition, or two of one sector, which growboot has found
 * consecutive, on a GUID table's, of 1024 bytes.
 *
 * @return the sector, or -1.
 */
static long env_sector(const char *status)
{
    char f[16][64];
    const char *line, *eol;
    long sector = -1;
    int n;

    for (line = status; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        memset(f, 0, sizeof(f));
        n = sscanf(line, "%63s %63s %63s %63s %63s %63s %63s %63s %63s %63s %63s", f[1], f[2], f[3],
                   f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
        if (n < 11 || strcmp(f[2], "grubenv:") != 0 || strcmp(f[11], "1024") != 0)
            continue;
        if (strcmp(f[9], "1") != 0 && !(strcmp(f[9], "2") == 0 && strcmp(f[6], "2") == 0))
            continue;
        f[5][strcspn(f[5], ",")] = '\0';
        if (f[5][0] == '\0' || strspn(f[5], "0123456789") != strlen(f[5]))
            return -1;
        sector = atol(f[5]);
    }
    return sector;
}

/** Report each of the assertions `ids`, ended by NULL, failed with `why`. */
static void fail_all(struct etude_case *c, const char *const ids[], const char *why)
{
    for (; *ids != NULL; ids++)
        etude_assert(c, *ids, 0, why);
}

int grow_machine(struct etude_case *c)
{
    static const char *const after_save[] = {"grown",       "small-seed", "measured",
                                             "grow-writes", "restored",   "restore-writes",
                                             "env-found",   "bytes",      NULL};
    const char *base = etude_artifact(c, "disk"), *store_image = etude_artifact(c, "store");
    const char *target, *store, *seed = etude_evidence(c, "seed.ext2");
    char why[300], buf[200], ack[2048], ev[96], hb[65], hs[65], *out = NULL, *sums = NULL,
                                                                *status = NULL;
    int saved_st, refused_st, grow_st, restore_st, plan_ok = 1, read_ok = 1, form_ok, ok;
    struct drive d;
    long env;
    size_t i;

    if (etude_step(c, "save") != ETUDE_OK)
        return 1;
    target = etude_private_copy(c, "target");
    store = etude_private_copy(c, "store");
    machine_boot(&d, c);
    saved_st =
        grow_shell(&d, "SAVED", NULL,
                   "sh /tmp/grow-plan/machine_grow.sh save /dev/disk0 /mnt/grow /tmp/grow-plan");
    refused_st =
        grow_shell(&d, "REFUSED", &out,
                   "sh /tmp/grow-plan/machine_grow.sh grow /dev/disk0 /mnt/grow /tmp/grow-plan");
    ok = refused_st == 1 && out != NULL && strstr(out, "host verification is absent") != NULL;
    free(out);
    out = NULL;
    grow_run(&d, "umount /mnt/grow");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    etude_assert(c, "save", !d.failed && saved_st == 0,
                 boot_why(&d, "save", saved_st, buf, sizeof(buf)));
    etude_assert(c, "unverified-refused", !d.failed && ok,
                 d.failed ? d.why
                          : "grow before the host's check did not end with status 1 saying the "
                            "host's verification is absent");

    if (etude_step(c, "saved") != ETUDE_OK)
        return 1;
    {
        char *const argv[] = {
            "qemu-img", "convert", "-f",           "qcow2",
            "-O",       "raw",     (char *)target, (char *)etude_evidence(c, "after-save.raw"),
            NULL};

        if (drive_host(c, argv, "after-save.out") != 0)
            snprintf(why, sizeof(why), "qemu-img could not convert the target");
        else
            grow_same(base, etude_evidence(c, "after-save.raw"), 0, 0, why, sizeof(why));
        etude_assert(c, "save-unchanged", why[0] == '\0', why);
    }
    {
        char *const seed_argv[] = {
            "mkexfatfixture", "--check", (char *)store_image, "2048", "--read", "seed.ext2", NULL};
        char *const tools_argv[] = {
            "mkexfatfixture", "--check", (char *)store_image, "2048", "--read",
            "grow-tools.tar", NULL};
        char *const ranges_argv[] = {"tar", "-xOf", (char *)etude_evidence(c, "grow-tools.tar"),
                                     "ranges.txt", NULL};

        plan_ok = drive_host(c, seed_argv, "seed.ext2") == 0 &&
                  drive_host(c, tools_argv, "grow-tools.tar") == 0 &&
                  drive_host(c, ranges_argv, "plan-ranges.txt") == 0;
    }
    for (i = 0; i < sizeof(saved_files) / sizeof(saved_files[0]); i++) {
        char path[64];
        char *const argv[] = {"mkexfatfixture", "--check", (char *)store, "2048",
                              "--read",         path,      NULL};

        snprintf(path, sizeof(path), "saved/%s", saved_files[i]);
        snprintf(ev, sizeof(ev), "saved-%s", saved_files[i]);
        if (drive_host(c, argv, ev) != 0)
            read_ok = 0;
    }
    sums = read_ok ? evidence_text(c, "saved-SHA256SUMS") : NULL;
    form_ok = sums != NULL && sums_form(sums, ack, sizeof(ack), why, sizeof(why));
    etude_assert(c, "sums-form", form_ok,
                 sums == NULL ? "the saved files cannot be read on the host" : why);
    ok = sums != NULL && sums_hold(c, sums, why, sizeof(why));
    etude_assert(c, "host-copy", ok,
                 sums == NULL ? "the saved files cannot be read on the host" : why);
    ok = read_ok && file_digest(base, 1048576, 67108864, hb) == 0 &&
         file_digest(etude_evidence(c, "saved-boot.img"), 0, UINT64_MAX, hs) == 0 &&
         strcmp(hb, hs) == 0;
    etude_assert(c, "boot-copy", ok,
                 "the saved boot.img is not the target's 64 MiB boot partition");
    free(sums);
    if (!form_ok) {
        fail_all(c, after_save, "the saved checksums are not valid, so the grow did not start");
        return 0;
    }

    if (etude_step(c, "grow") != ETUDE_OK)
        return 1;
    machine_boot(&d, c);
    grow_run(&d, "%s", ack);
    grow_st =
        grow_shell(&d, "FINISHED", &out,
                   "sh /tmp/grow-plan/machine_grow.sh grow /dev/disk0 /mnt/grow /tmp/grow-plan");
    ok = grow_st == 0 && out != NULL &&
         strstr(out, "the measured grow and read-only check passed") != NULL;
    free(out);
    out = NULL;
    grow_run(&d, "dd if=/dev/zero of=/tmp/broken-mbr bs=512 count=1 && reflash /dev/disk0 0 512 < "
                 "/tmp/broken-mbr && "
                 "diskctl reread /dev/disk0");
    grow_run(&d, "umount /mnt/grow");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    ok = ok && !d.failed && !grow_console_has(&d, "machine-grow: failed:");
    etude_assert(c, "grown", ok,
                 d.failed ? d.why
                          : "the grow did not end with status 0 saying the measured grow and "
                            "read-only check passed, or said machine-grow: failed:");
    etude_assert(c, "small-seed",
                 grow_console_has(&d, "growroot: entry: /dev/disk0p3 from 16384 to 131072 sectors"),
                 "growroot's entry record is not of the 8 MiB seed grown to 131072 sectors");
    etude_assert(c, "measured", grow_console_has(&d, "growroot: measure: resize "),
                 "growroot printed no measure of its resize");

    if (etude_step(c, "grown") != ETUDE_OK)
        return 1;
    if (plan_ok)
        ok = grow_writes_inside(c, "grown", target, seed, "plan-ranges.txt", why, sizeof(why));
    etude_assert(c, "grow-writes", plan_ok && ok,
                 plan_ok ? why : "the plan cannot be read from the store");

    if (etude_step(c, "restore") != ETUDE_OK)
        return 1;
    machine_boot(&d, c);
    restore_st =
        grow_shell(&d, "RESTORED", &out,
                   "sh /tmp/grow-plan/machine_grow.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan");
    ok = restore_st == 0 && out != NULL &&
         strstr(out, "every saved range matches and the restored root checks clean") != NULL;
    free(out);
    out = NULL;
    grow_run(&d, "umount /mnt/grow");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    ok = ok && !d.failed && !grow_console_has(&d, "machine-grow: failed:");
    etude_assert(c, "restored", ok,
                 d.failed ? d.why
                          : "the restore did not end with status 0 saying every saved range "
                            "matches, or said machine-grow: failed:");

    if (etude_step(c, "restored") != ETUDE_OK)
        return 1;
    if (plan_ok)
        ok = grow_writes_inside(c, "restored", target, seed, "plan-ranges.txt", why, sizeof(why));
    etude_assert(c, "restore-writes", plan_ok && ok,
                 plan_ok ? why : "the plan cannot be read from the store");
    status = read_ok ? evidence_text(c, "saved-boot.status") : NULL;
    env = status != NULL ? env_sector(status) : -1;
    free(status);
    etude_assert(c, "env-found", env >= 0,
                 "the saved boot status puts the environment in no two consecutive sectors");
    {
        char *const argv[] = {
            "qemu-img", "convert", "-f",           "qcow2",
            "-O",       "raw",     (char *)target, (char *)etude_evidence(c, "restored.raw"),
            NULL};

        if (env < 0)
            snprintf(why, sizeof(why), "the environment's sector is not known");
        else if (drive_host(c, argv, "restored.out") != 0)
            snprintf(why, sizeof(why), "qemu-img could not convert the target");
        else
            grow_same(base, etude_evidence(c, "restored.raw"), (uint64_t)(BOOT_FIRST + env) * 512,
                      1024, why, sizeof(why));
        etude_assert(c, "bytes", why[0] == '\0', why);
    }
    return 0;
}
