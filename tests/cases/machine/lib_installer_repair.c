/*
 * lib_installer_repair.c: the code the scenarios of the installer-repair cases
 * share: their boots, their own files on the host, and the readings of disk
 * tables and consoles that they judge by.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib_installer_repair.h"
#include "sha256.h"

/* The assertions reported failed in this attempt. */
static int repair_failures;

/* The attempt's own directory, once repair_path has made it. */
static char repair_work[4096];

int repair_assert(struct etude_case *c, const char *id, int held, const char *detail)
{
    held = held != 0;
    if (!held)
        repair_failures++;
    etude_assert(c, id, held, detail != NULL && detail[0] != '\0' ? detail : "failed");
    return held;
}

int repair_path(struct etude_case *c, const char *name, char *buf, size_t size)
{
    const char *probe;
    char *cut;

    if (repair_work[0] == '\0') {
        probe = etude_evidence(c, "work");
        if (probe == NULL || strlen(probe) >= sizeof(repair_work))
            return -1;
        strcpy(repair_work, probe);
        cut = strstr(repair_work, "/evidence/work");
        if (cut == NULL) {
            repair_work[0] = '\0';
            return -1;
        }
        strcpy(cut, "/work");
        if (mkdir(repair_work, 0700) != 0 && errno != EEXIST) {
            repair_work[0] = '\0';
            return -1;
        }
    }
    if ((size_t)snprintf(buf, size, "%s%s%s", repair_work, name[0] != '\0' ? "/" : "", name) >=
        size)
        return -1;
    return 0;
}

/** Remove the file or the directory tree `path`; -1 when any part stays. */
static int remove_tree(const char *path)
{
    char child[4096];
    struct dirent *e;
    struct stat st;
    DIR *dir;
    int rc = 0;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    if ((dir = opendir(path)) == NULL)
        return -1;
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if ((size_t)snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= sizeof(child) ||
            remove_tree(child) != 0)
            rc = -1;
    }
    closedir(dir);
    return rmdir(path) != 0 ? -1 : rc;
}

void repair_finish(struct etude_case *c)
{
    (void)c;
    if (repair_work[0] == '\0')
        return;
    if (repair_failures != 0) {
        fprintf(stderr, "installer-repair: kept: %s\n", repair_work);
        return;
    }
    if (remove_tree(repair_work) != 0)
        fprintf(stderr, "installer-repair: not removed: %s\n", repair_work);
}

int repair_run(struct etude_case *c, char *const argv[], const char *output, char **out)
{
    char err_name[160];
    char *text, *err, *both;
    size_t n;
    int rc;

    rc = drive_host(c, argv, output);
    if (out == NULL)
        return rc;
    snprintf(err_name, sizeof(err_name), "%s.err", output);
    text = evidence_text(c, output);
    err = evidence_text(c, err_name);
    n = (text != NULL ? strlen(text) : 0) + (err != NULL ? strlen(err) : 0);
    both = malloc(n + 1);
    if (both != NULL)
        snprintf(both, n + 1, "%s%s", text != NULL ? text : "", err != NULL ? err : "");
    free(text);
    free(err);
    *out = both;
    return rc;
}

int repair_volume(struct etude_case *c, const char *tool, const char *disk, uint64_t at,
                  const char *const args[], const char *output, char **out)
{
    char spec[4200];
    char *argv[16];
    size_t i, n = 0;

    if (disk == NULL || (size_t)snprintf(spec, sizeof(spec), "%s@@%llu", disk,
                                         (unsigned long long)at) >= sizeof(spec))
        return -1;
    argv[n++] = (char *)tool;
    argv[n++] = "-i";
    argv[n++] = spec;
    for (i = 0; args[i] != NULL && n < sizeof(argv) / sizeof(argv[0]) - 1; i++)
        argv[n++] = (char *)args[i];
    argv[n] = NULL;
    return repair_run(c, argv, output, out);
}

int repair_entry(struct etude_case *c, const char *tool, const char *disk, int n, uint64_t *start,
                 uint64_t *len)
{
    static unsigned listings;
    char output[64], *text = NULL, *line, *eol;
    unsigned long long s, l;
    int k, found = -1;

    snprintf(output, sizeof(output), "%s.list.%u", tool, ++listings);
    if (disk == NULL || repair_run(c, (char *const[]){(char *)tool, "--list", (char *)disk, NULL},
                                   output, &text) != 0) {
        free(text);
        return -1;
    }
    for (line = text; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        if (sscanf(line, "%d %*s %llu %llu", &k, &s, &l) == 3 && k == n) {
            *start = s;
            *len = l;
            found = 0;
            break;
        }
    }
    free(text);
    return found;
}

int repair_byte(const char *path, uint64_t off)
{
    unsigned char b;
    int fd, rc = -1;

    if (path == NULL || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    if (pread(fd, &b, 1, (off_t)off) == 1)
        rc = b;
    close(fd);
    return rc;
}

int repair_last_field(const char *text, const char *prefix, char *buf, size_t size)
{
    int n = 0;

    while (text != NULL && text_field(text, prefix, n + 1, buf, size) == 0)
        n++;
    return n > 0 ? text_field(text, prefix, n, buf, size) : -1;
}

int repair_line_has(const char *text, const char *const parts[])
{
    const char *line = text, *eol, *at;
    size_t i;
    char *one;

    while (line != NULL && *line != '\0') {
        eol = strchr(line, '\n');
        one = eol != NULL ? strndup(line, (size_t)(eol - line)) : strdup(line);
        if (one == NULL)
            return 0;
        at = one;
        for (i = 0; parts[i] != NULL && at != NULL; i++)
            if ((at = strstr(at, parts[i])) != NULL)
                at += strlen(parts[i]);
        free(one);
        if (at != NULL)
            return 1;
        line = eol != NULL ? eol + 1 : NULL;
    }
    return 0;
}

/** Feed `len` bytes of file `path` from `off` to `h`; -1 when they cannot be
 * read. */
static int digest_range(struct sha256 *h, const char *path, uint64_t off, uint64_t len)
{
    static unsigned char buf[1 << 20];
    ssize_t r;
    int fd;

    if (path == NULL || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    while (len > 0) {
        r = pread(fd, buf, len < sizeof(buf) ? (size_t)len : sizeof(buf), (off_t)off);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            close(fd);
            return -1;
        }
        sha256_update(h, buf, (size_t)r);
        off += (uint64_t)r;
        len -= (uint64_t)r;
    }
    close(fd);
    return 0;
}

int repair_digest_join(const char *a, uint64_t aoff, uint64_t alen, const char *b, uint64_t boff,
                       uint64_t blen, char hex[65])
{
    unsigned char sum[32];
    struct sha256 h;

    sha256_init(&h);
    if (digest_range(&h, a, aoff, alen) != 0 || digest_range(&h, b, boff, blen) != 0)
        return -1;
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
    return 0;
}

int repair_copy(const char *from, const char *to, uint64_t at, int mode)
{
    static unsigned char buf[1 << 20];
    ssize_t r, w;
    int in, out;

    if (from == NULL || to == NULL || (in = open(from, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    out = mode != 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode)
                    : open(to, O_WRONLY | O_CLOEXEC);
    if (out < 0) {
        close(in);
        return -1;
    }
    for (;;) {
        r = read(in, buf, sizeof(buf));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        w = pwrite(out, buf, (size_t)r, (off_t)at);
        if (w != r) {
            r = -1;
            break;
        }
        at += (uint64_t)r;
    }
    close(in);
    if (r < 0 || (mode != 0 && fchmod(out, (mode_t)mode) != 0)) {
        close(out);
        return -1;
    }
    return close(out);
}

int repair_read_line(const char *path, char *buf, size_t size)
{
    ssize_t n;
    int fd;

    if (path == NULL || size == 0 || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    n = read(fd, buf, size);
    close(fd);
    if (n < 0 || (size_t)n >= size)
        return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return 0;
}

int repair_recovery(struct drive *d, struct etude_case *c, const char *machine)
{
    drive_boot(d, c, machine, NULL);
    drive_expect(d, "recovery image");
    drive_expect(d, "# ");
    drive_send(d, "%s", "mkdir -p /m && mount -t exfat /dev/disk1p2 /m && echo MOUNT\"\"ED");
    return drive_expect(d, "MOUNTED");
}

int repair_login(struct drive *d, struct etude_case *c, const char *machine, const char *password)
{
    drive_boot(d, c, machine, NULL);
    drive_expect(d, "rach2 login:");
    drive_send(d, "%s", "root");
    drive_expect(d, "Password:");
    drive_send(d, "%s", password);
    return drive_expect(d, "# ");
}

int repair_password(struct etude_case *c, char *buf, size_t size)
{
    return repair_read_line(etude_artifact(c, "password"), buf, size);
}
