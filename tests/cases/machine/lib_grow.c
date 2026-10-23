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
#include <sys/stat.h>
#include <unistd.h>

#include "lib_grow.h"
#include "volume/volume.h"

/* The end of the installed machine's boot partition, which starts at
 * GROW_BOOT_FIRST. */
#define BOOT_END 133120

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

/* The files grow_made noted, the assertions reported, and the number of
 * them that failed. */
static char *made_files[32];
static size_t made_count;
static char *reported[64];
static size_t reported_count;
static int failures;

void grow_assert(struct etude_case *c, const char *id, int held, const char *detail)
{
    if (!held)
        failures++;
    if (reported_count < sizeof(reported) / sizeof(reported[0]) &&
        (reported[reported_count] = strdup(id)) != NULL)
        reported_count++;
    etude_assert(c, id, held, detail);
}

void grow_fail_rest(struct etude_case *c, const char *const ids[], const char *why)
{
    size_t i;

    for (; *ids != NULL; ids++) {
        for (i = 0; i < reported_count && strcmp(reported[i], *ids) != 0; i++)
            ;
        if (i == reported_count)
            grow_assert(c, *ids, 0, why);
    }
}

void grow_made(const char *path)
{
    size_t i;

    if (path == NULL)
        return;
    for (i = 0; i < made_count; i++)
        if (strcmp(made_files[i], path) == 0)
            return;
    if (made_count < sizeof(made_files) / sizeof(made_files[0]) &&
        (made_files[made_count] = strdup(path)) != NULL)
        made_count++;
}

void grow_clean(void)
{
    size_t i;

    for (i = 0; i < made_count; i++) {
        if (failures == 0)
            unlink(made_files[i]);
        free(made_files[i]);
    }
    made_count = 0;
}

char *grow_evidence(struct etude_case *c, const char *name, char *buf, size_t size)
{
    const char *path = etude_evidence(c, name);

    if (path == NULL || strlen(path) >= size)
        return NULL;
    memcpy(buf, path, strlen(path) + 1);
    return buf;
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

int grow_copy_file(const char *from, const char *to)
{
    struct stat st;

    if (from == NULL || stat(from, &st) != 0)
        return -1;
    return grow_copy_range(from, 0, (uint64_t)st.st_size, to);
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

int grow_raw_same(struct etude_case *c, const char *name, uint64_t env, char *why, size_t size)
{
    const char *disk = etude_private_copy(c, "target"), *base = etude_artifact(c, "disk");
    char raw_name[64], out_name[64], raw[1024];
    int same;

    snprintf(raw_name, sizeof(raw_name), "%s.raw", name);
    snprintf(out_name, sizeof(out_name), "%s.convert", name);
    if (grow_evidence(c, raw_name, raw, sizeof(raw)) == NULL) {
        snprintf(why, size, "no evidence %s", raw_name);
        return 0;
    }
    {
        char *const argv[] = {"qemu-img", "convert",    "-f", "qcow2", "-O",
                              "raw",      (char *)disk, raw,  NULL};

        if (drive_host(c, argv, out_name) != 0) {
            snprintf(why, size, "qemu-img could not convert the target to %s", raw_name);
            return 0;
        }
    }
    same = grow_same(base, raw, env, env != 0 ? 1024 : 0, why, size);
    if (same)
        unlink(raw);
    return same;
}

int grow_writes_inside(struct etude_case *c, const char *name, const char *disk, const char *seed,
                       const char *ranges, char *why, size_t size)
{
    unsigned long long first[256], end[256], a, n;
    char map_name[64], cover_name[64], map[1024], *cover = NULL, *list = NULL, *line, *eol;
    int count = 0, extents = -1, inside = 1, st, k, covered;

    snprintf(map_name, sizeof(map_name), "%s.map", name);
    snprintf(cover_name, sizeof(cover_name), "%s.cover", name);
    if (grow_evidence(c, map_name, map, sizeof(map)) == NULL) {
        snprintf(why, size, "no evidence %s", map_name);
        return 0;
    }
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
        covered = a >= GROW_BOOT_FIRST && a + n <= BOOT_END;
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
static const char *const saved_files[] = {"backup.bin", "boot.img",    "grow.plan", "ranges.txt",
                                          "SHA256SUMS", "boot.status", NULL};

void grow_recovery_boot(struct drive *d, struct etude_case *c, const char *mount)
{
    grow_boot(d, c, "recovery", NULL, "# ");
    grow_run(d,
             "test ! -e /tmp/grow-plan && mkdir -p /mnt/grow /tmp/grow-plan && "
             "mount %s -t exfat /dev/disk1p1 /mnt/grow && "
             "tar -C /tmp/grow-plan -xf /mnt/grow/grow-tools.tar && "
             "tar -C /tmp/grow-plan -xf /mnt/grow/grow-tests.tar",
             mount);
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
 * name of a file `save` writes, and nothing else, an empty line included.
 *
 * @return 1 when every line is such, 0 otherwise.
 */
static int sums_form(const char *sums, char *why, size_t wsize)
{
    char text[256], hex[80], file[64], extra[2];
    const char *line, *eol;
    size_t n;
    int lines = 0, i;

    for (line = sums; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        n = eol != NULL ? (size_t)(eol - line) : strlen(line);
        if (n >= sizeof(text))
            n = 0;
        memcpy(text, line, n);
        text[n] = '\0';
        if (sscanf(text, "%79s %63s %1s", hex, file, extra) != 2 || strlen(hex) != 64 ||
            strspn(hex, "0123456789abcdef") != 64) {
            snprintf(why, wsize, "a saved checksum is not 64 hexadecimal digits and a name: %.80s",
                     line);
            return 0;
        }
        for (i = 0; i < 4 && strcmp(file, saved_files[i]) != 0; i++)
            ;
        if (i == 4) {
            snprintf(why, wsize, "the saved checksums name %s", file);
            return 0;
        }
        lines++;
    }
    if (lines == 0)
        snprintf(why, wsize, "the saved checksums are empty");
    return lines > 0;
}

/* The checksums are typed a line for each, since the shell takes typed lines
 * of at most 255 bytes. */
void grow_type_sums(struct drive *d, const char *sums)
{
    char hex[80], file[64];
    const char *line, *eol;

    grow_run(d, ": > /mnt/grow/saved/host.sha256");
    for (line = sums; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        if (sscanf(line, "%79s %63s", hex, file) == 2)
            grow_run(d, "printf '%%s\\n' '%s  %s' >> /mnt/grow/saved/host.sha256", hex, file);
    }
}

int grow_sums_hold(struct etude_case *c, const char *sums, const char *prefix, char *why,
                   size_t size)
{
    char hex[80], file[64], ev[96], got[65];
    const char *line, *eol;

    if (!sums_form(sums, why, size))
        return 0;
    for (line = sums; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        if (sscanf(line, "%79s %63s", hex, file) != 2)
            continue;
        snprintf(ev, sizeof(ev), "%s%s", prefix, file);
        if (file_digest(etude_evidence(c, ev), 0, UINT64_MAX, got) != 0 || strcmp(got, hex) != 0) {
            snprintf(why, size, "the host's copy of saved/%s does not have its saved checksum",
                     file);
            return 0;
        }
    }
    return 1;
}

int grow_read_saved(struct etude_case *c, const char *store, const char *prefix,
                    const char *const names[])
{
    char ev[96], file[96];
    size_t i;

    if (names == NULL)
        names = saved_files;
    for (i = 0; names[i] != NULL && i < 8; i++) {
        char *argv[] = {"mkexfatfixture", "--check", (char *)store,
                        "2048", "--read", file, NULL};
        snprintf(file, sizeof(file), "saved/%s", names[i]);
        snprintf(ev, sizeof(ev), "%s%s", prefix, names[i]);
        grow_made(etude_evidence(c, ev));
        if (drive_host(c, argv, ev) != 0)
            return -1;
    }
    return names[i] == NULL ? 0 : -1;
}

long grow_env_sector(const char *status)
{
    char f[16][64], *p;
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
        for (n = 0, p = f[5]; *p != '\0'; p++)
            if (*p != ',')
                f[5][n++] = *p;
        f[5][n] = '\0';
        if (sector != -1 || f[5][0] == '\0' || strspn(f[5], "0123456789") != strlen(f[5]))
            return -1;
        sector = atol(f[5]);
    }
    return sector;
}

int grow_read_plan(struct etude_case *c, char *seed, size_t size)
{
    char tools[1024], ranges[1024], why[256];

    if (grow_evidence(c, "plan-seed.ext2", seed, size) == NULL ||
        grow_evidence(c, "plan-ranges.txt", ranges, sizeof(ranges)) == NULL ||
        grow_evidence(c, "plan-grow-tools.tar", tools, sizeof(tools)) == NULL)
        return -1;
    grow_made(seed);
    grow_made(tools);
    {
        char *const seed_argv[] = {"mkexfatfixture", "--check",
                                   (char *)etude_artifact(c, "store"),
                                   "2048", "--read", "seed.ext2", NULL};
        char *const tools_argv[] = {"mkexfatfixture", "--check",
                                    (char *)etude_artifact(c, "store"),
                                    "2048", "--read", "grow-tools.tar", NULL};
        grow_made(ranges);
        return drive_host(c, seed_argv, "plan-seed.ext2") == 0 &&
                       drive_host(c, tools_argv, "plan-grow-tools.tar") == 0 &&
                       etude_volume_file(tools, "ranges.txt", ranges, why, sizeof(why)) == 0
                   ? 0
                   : -1;
    }
}

int grow_target_boots(struct etude_case *c, char *why, size_t size)
{
    char *cmdline = NULL, *owner = NULL;
    struct drive d;
    int st, owner_st, ok;

    grow_boot(&d, c, "target", NULL, "# ");
    st = grow_shell(&d, "BOOTED", &cmdline,
                    "cat /proc/cmdline; bootctl --target /dev/disk0p1 status");
    owner_st = grow_shell(&d, "OWNER", &owner,
                          "mkdir -p /mnt/root && mount -t ext2 -o ro /dev/disk0p3 /mnt/root && "
                          "cat /mnt/root/owner.txt && umount /mnt/root");
    drive_stop(&d);
    if (d.failed) {
        snprintf(why, size, "%s", d.why);
        ok = 0;
    } else if (st != 0 || strstr(cmdline, "rach2.slot=recovery") == NULL ||
               strstr(cmdline, "bootctl: saved_entry: recovery") == NULL ||
               strstr(cmdline, "bootctl: next_entry: absent") == NULL) {
        snprintf(why, size,
                 "the target did not start recovery with recovery saved and no next entry");
        ok = 0;
    } else if (owner_st != 0 || strstr(owner, "preserve the installed root") == NULL) {
        snprintf(why, size, "the target's root has no owner's file");
        ok = 0;
    } else {
        ok = 1;
    }
    free(cmdline);
    free(owner);
    return ok;
}

void grow_fail_all(struct etude_case *c, const char *const ids[], const char *why)
{
    for (; *ids != NULL; ids++)
        grow_assert(c, *ids, 0, why);
}

int grow_machine(struct etude_case *c)
{
    static const char *const after_save[] = {"grown",       "small-seed", "measured",
                                             "grow-writes", "restored",   "restore-writes",
                                             "env-found",   "bytes",      NULL};
    const char *base = etude_artifact(c, "disk");
    const char *target, *store;
    char why[300], buf[200], hb[65], hs[65], *out = NULL, *sums = NULL, *status = NULL;
    char seed[1024];
    int saved_st, refused_st, grow_st, restore_st, plan_ok, read_ok, form_ok, ok;
    struct drive d;
    long env;

    if (etude_step(c, "save") != ETUDE_OK)
        return 1;
    target = etude_private_copy(c, "target");
    store = etude_private_copy(c, "store");
    grow_recovery_boot(&d, c, "");
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
    grow_assert(c, "save", !d.failed && saved_st == 0,
                boot_why(&d, "save", saved_st, buf, sizeof(buf)));
    grow_assert(c, "unverified-refused", !d.failed && ok,
                d.failed ? d.why
                         : "grow before the host's check did not end with status 1 saying the "
                           "host's verification is absent");

    if (etude_step(c, "saved") != ETUDE_OK)
        return 1;
    ok = grow_raw_same(c, "after-save", 0, why, sizeof(why));
    grow_assert(c, "save-unchanged", ok, why);
    plan_ok = grow_read_plan(c, seed, sizeof(seed)) == 0;
    read_ok = grow_read_saved(c, store, "saved-", NULL) == 0;
    sums = read_ok ? evidence_text(c, "saved-SHA256SUMS") : NULL;
    form_ok = sums != NULL && sums_form(sums, why, sizeof(why));
    grow_assert(c, "sums-form", form_ok,
                sums == NULL ? "the saved files cannot be read on the host" : why);
    ok = sums != NULL && grow_sums_hold(c, sums, "saved-", why, sizeof(why));
    grow_assert(c, "host-copy", ok,
                sums == NULL ? "the saved files cannot be read on the host" : why);
    ok = read_ok && file_digest(base, 1048576, 67108864, hb) == 0 &&
         file_digest(etude_evidence(c, "saved-boot.img"), 0, UINT64_MAX, hs) == 0 &&
         strcmp(hb, hs) == 0;
    grow_assert(c, "boot-copy", ok, "the saved boot.img is not the target's 64 MiB boot partition");
    if (!form_ok) {
        free(sums);
        grow_fail_all(c, after_save,
                      "the saved checksums are not valid, so the grow did not start");
        grow_clean();
        return 0;
    }

    if (etude_step(c, "grow") != ETUDE_OK)
        return 1;
    grow_recovery_boot(&d, c, "");
    grow_type_sums(&d, sums);
    free(sums);
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
    grow_assert(c, "grown", ok,
                d.failed ? d.why
                         : "the grow did not end with status 0 saying the measured grow and "
                           "read-only check passed, or said machine-grow: failed:");
    grow_assert(c, "small-seed",
                grow_console_has(&d, "growroot: entry: /dev/disk0p3 from 16384 to 131072 sectors"),
                "growroot's entry record is not of the 8 MiB seed grown to 131072 sectors");
    grow_assert(c, "measured", grow_console_has(&d, "growroot: measure: resize "),
                "growroot printed no measure of its resize");

    if (etude_step(c, "grown") != ETUDE_OK)
        return 1;
    if (plan_ok)
        ok = grow_writes_inside(c, "grown", target, seed, "plan-ranges.txt", why, sizeof(why));
    grow_assert(c, "grow-writes", plan_ok && ok,
                plan_ok ? why : "the plan cannot be read from the store");

    if (etude_step(c, "restore") != ETUDE_OK)
        return 1;
    grow_recovery_boot(&d, c, "");
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
    grow_assert(c, "restored", ok,
                d.failed ? d.why
                         : "the restore did not end with status 0 saying every saved range "
                           "matches, or said machine-grow: failed:");

    if (etude_step(c, "restored") != ETUDE_OK)
        return 1;
    if (plan_ok)
        ok = grow_writes_inside(c, "restored", target, seed, "plan-ranges.txt", why, sizeof(why));
    grow_assert(c, "restore-writes", plan_ok && ok,
                plan_ok ? why : "the plan cannot be read from the store");
    status = read_ok ? evidence_text(c, "saved-boot.status") : NULL;
    env = status != NULL ? grow_env_sector(status) : -1;
    free(status);
    grow_assert(c, "env-found", env >= 0,
                "the saved boot status puts the environment in no two consecutive sectors");
    if (env < 0) {
        snprintf(why, sizeof(why), "the environment's sector is not known");
        ok = 0;
    } else {
        ok =
            grow_raw_same(c, "restored", (uint64_t)(GROW_BOOT_FIRST + env) * 512, why, sizeof(why));
    }
    grow_assert(c, "bytes", ok, why);
    grow_clean();
    return 0;
}
