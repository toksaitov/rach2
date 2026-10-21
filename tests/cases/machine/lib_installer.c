/*
 * lib_installer.c: the code the scenarios of the installer suite share, as
 * lib_installer.h declares it.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lib_installer.h"

/* The assertions that failed, and the files the scenario made. */
static int failures;
static char *made_files[64];
static size_t made_count;

void inst_assert(struct etude_case *c, const char *id, int held, const char *detail)
{
    if (!held)
        failures++;
    etude_assert(c, id, held, detail);
}

void inst_fail(struct etude_case *c, const char *id, const char *why)
{
    inst_assert(c, id, 0, why != NULL && why[0] != '\0' ? why : "an earlier step did not complete");
}

void inst_made(const char *path)
{
    size_t i;

    if (path == NULL)
        return;
    for (i = 0; i < made_count; i++)
        if (strcmp(made_files[i], path) == 0)
            return;
    if (made_count < sizeof(made_files) / sizeof(made_files[0]))
        made_files[made_count++] = strdup(path);
}

void inst_finish(void)
{
    size_t i;

    for (i = 0; i < made_count; i++) {
        if (failures == 0 && made_files[i] != NULL)
            unlink(made_files[i]);
        free(made_files[i]);
    }
    made_count = 0;
}

void inst_refuse(struct drive *d, const char *why)
{
    if (d->failed)
        return;
    d->failed = 1;
    snprintf(d->why, sizeof(d->why), "%s", why);
}

int inst_exchange(struct drive *d, const struct inst_line *lines)
{
    for (; lines->text != NULL; lines++) {
        if (lines->send)
            drive_send(d, "%s", lines->text);
        else
            drive_expect(d, lines->text);
    }
    return d->failed ? -1 : 0;
}

char *inst_host_text(struct etude_case *c, char *const argv[], const char *output, int *status)
{
    char err[160];
    char *out, *text, *joined;
    size_t n;

    *status = drive_host(c, argv, output);
    snprintf(err, sizeof(err), "%s.err", output);
    out = evidence_text(c, output);
    text = evidence_text(c, err);
    if (out == NULL || text == NULL) {
        free(out);
        free(text);
        return NULL;
    }
    n = strlen(out);
    joined = realloc(out, n + strlen(text) + 1);
    if (joined == NULL) {
        free(out);
        free(text);
        return NULL;
    }
    strcpy(joined + n, text);
    free(text);
    return joined;
}

int inst_gpt_entry(struct etude_case *c, const char *image, int n, uint64_t *first, uint64_t *bytes)
{
    char *argv[] = {"mkgpt", "--list", (char *)image, NULL};
    char want[16], *text, *line, *eol;
    int status, found = -1;

    if (image == NULL)
        return -1;
    text = inst_host_text(c, argv, "mkgpt.list", &status);
    if (text == NULL || status != 0) {
        free(text);
        return -1;
    }
    snprintf(want, sizeof(want), "%d ", n);
    for (line = text; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        if (strncmp(line, want, strlen(want)) == 0 &&
            sscanf(line, "%*d %*s %" SCNu64 " %" SCNu64, first, bytes) == 2) {
            found = 0;
            break;
        }
    }
    free(text);
    return found;
}

int inst_pattern_disk(struct etude_case *c, const char *name, uint64_t bytes, char hex[65])
{
    const char *path = etude_private_copy(c, name);

    if (path == NULL || fill_pattern(path, bytes) != 0)
        return -1;
    return file_digest(path, 0, UINT64_MAX, hex);
}

int inst_private_file(struct etude_case *c, const char *name, const char *evidence, char *path,
                      size_t size)
{
    const char *from = etude_artifact(c, name), *to = etude_evidence(c, evidence);
    char buf[4096];
    ssize_t r;
    int in, out, bad = 0;

    if (from == NULL || to == NULL || strlen(to) >= size)
        return -1;
    snprintf(path, size, "%s", to);
    if ((in = open(from, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    unlink(path);
    if ((out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
        close(in);
        return -1;
    }
    inst_made(path);
    while ((r = read(in, buf, sizeof(buf))) != 0) {
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 || write(out, buf, (size_t)r) != r) {
            bad = 1;
            break;
        }
    }
    close(in);
    return close(out) != 0 || bad ? -1 : 0;
}

int inst_artifact_line(struct etude_case *c, const char *name, char *buf, size_t size)
{
    const char *path = etude_artifact(c, name);
    FILE *f;
    size_t n;

    if (path == NULL || (f = fopen(path, "r")) == NULL)
        return -1;
    if (fgets(buf, (int)size, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    n = strcspn(buf, "\r\n");
    buf[n] = '\0';
    return n > 0 ? 0 : -1;
}

/** Give in `buf` the option that names the evidence `known` as the file of
 * known host keys, and in `port` the case's first port. */
static int ssh_names(struct etude_case *c, const char *known, char *buf, size_t size, char *port,
                     size_t port_size)
{
    const char *path = etude_evidence(c, known);

    if (path == NULL || etude_port(c, 0) == 0)
        return -1;
    inst_made(path);
    snprintf(buf, size, "UserKnownHostsFile=%s", path);
    snprintf(port, port_size, "%u", etude_port(c, 0));
    return 0;
}

char *inst_ssh_key(struct etude_case *c, const char *known, const char *key, const char *user,
                   const char *command, const char *output)
{
    char hosts[4200], port[16], to[96];
    int status;
    char *argv[] = {"ssh",
                    "-o",
                    "StrictHostKeyChecking=no",
                    "-o",
                    hosts,
                    "-o",
                    "BatchMode=yes",
                    "-o",
                    "IdentitiesOnly=yes",
                    "-o",
                    "PreferredAuthentications=publickey",
                    "-o",
                    "PasswordAuthentication=no",
                    "-i",
                    (char *)key,
                    "-p",
                    port,
                    to,
                    (char *)command,
                    NULL};

    if (ssh_names(c, known, hosts, sizeof(hosts), port, sizeof(port)) != 0)
        return NULL;
    snprintf(to, sizeof(to), "%s@127.0.0.1", user);
    return inst_host_text(c, argv, output, &status);
}

char *inst_ssh_password(struct etude_case *c, const char *known, const char *password,
                        const char *command, const char *output)
{
    char hosts[4200], port[16];
    char *text;
    int status;
    char *argv[] = {"ptydrive",
                    "--rows",
                    "40",
                    "--cols",
                    "100",
                    "--",
                    "ssh",
                    "-o",
                    "StrictHostKeyChecking=no",
                    "-o",
                    hosts,
                    "-o",
                    "PubkeyAuthentication=no",
                    "-o",
                    "PreferredAuthentications=password",
                    "-o",
                    "NumberOfPasswordPrompts=3",
                    "-tt",
                    "-p",
                    port,
                    "root@127.0.0.1",
                    (char *)command,
                    NULL};

    if (ssh_names(c, known, hosts, sizeof(hosts), port, sizeof(port)) != 0 ||
        setenv("RACH2_PASSWORD", password, 1) != 0)
        return NULL;
    text = inst_host_text(c, argv, output, &status);
    unsetenv("RACH2_PASSWORD");
    return text;
}

/** Find the first `n` bytes `needle` in file `path`, and give their offset
 * in `*at`; return -1 when the file does not have them. */
static int find_bytes(const char *path, const char *needle, size_t n, uint64_t *at)
{
    static char buf[(1 << 20) + 256];
    uint64_t base = 0;
    size_t have = 0, i;
    ssize_t r;
    int fd;

    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    for (;;) {
        r = read(fd, buf + have, sizeof(buf) - have);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        have += (size_t)r;
        for (i = 0; i + n <= have; i++) {
            if (memcmp(buf + i, needle, n) == 0) {
                *at = base + i;
                close(fd);
                return 0;
            }
        }
        if (have >= n) {
            memmove(buf, buf + have - (n - 1), n - 1);
            base += have - (n - 1);
            have = n - 1;
        }
    }
    close(fd);
    return -1;
}

/**
 * Write into `out` the GRUB environment block `block`, 1024 bytes, with
 * next_entry=recovery: its two header lines, every variable of the block but
 * next_entry, then next_entry, and `#` to fill 1024 bytes, as grub-editenv
 * writes one.
 */
static int armed_block(const char *block, char out[1024])
{
    static const char head[] = "# GRUB Environment Block\n"
                               "# WARNING: Do not edit this file by tools other than "
                               "grub-editenv!!!\n";
    size_t n = 0, len, i;
    const char *line = block, *end = block + 1024, *eol;

    memcpy(out, head, sizeof(head) - 1);
    n = sizeof(head) - 1;
    while (line < end) {
        eol = memchr(line, '\n', (size_t)(end - line));
        len = eol != NULL ? (size_t)(eol - line) : (size_t)(end - line);
        while (len > 0 && line[len - 1] == '#')
            len--;
        if (len > 0 && line[0] != '#' && strncmp(line, "next_entry=", 11) != 0) {
            if (n + len + 1 > 1024)
                return -1;
            memcpy(out + n, line, len);
            n += len;
            out[n++] = '\n';
        }
        if (eol == NULL)
            break;
        line = eol + 1;
    }
    len = strlen("next_entry=recovery\n");
    if (n + len > 1024)
        return -1;
    memcpy(out + n, "next_entry=recovery\n", len);
    n += len;
    for (i = n; i < 1024; i++)
        out[i] = '#';
    return 0;
}

int inst_recovery_boot(struct etude_case *c, const char *disk, char *why, size_t size)
{
    static const char mark[] = "# GRUB Environment Block";
    const char *keyless = etude_artifact(c, "keyless");
    char at[4200], block[1024], armed[1024];
    uint64_t first, bytes, env;
    int fd, status;
    char *text;
    char *del[] = {"mdel", "-i", at, "::/boot/recovery.krffs", NULL};
    char *copy[] = {"mcopy", "-i", at, (char *)keyless, "::/boot/recovery.krffs", NULL};

    if (disk == NULL || keyless == NULL) {
        snprintf(why, size, "the disk or the keyless recovery image is missing");
        return -1;
    }
    if (inst_gpt_entry(c, disk, 2, &first, &bytes) != 0 || first == 0) {
        snprintf(why, size, "the disk has no recovery partition in its GUID table");
        return -1;
    }
    snprintf(at, sizeof(at), "%s@@%" PRIu64, disk, first);
    text = inst_host_text(c, del, "mdel.recovery", &status);
    free(text);
    if (status != 0) {
        snprintf(why, size, "mdel of the recovery image exited %d", status);
        return -1;
    }
    text = inst_host_text(c, copy, "mcopy.recovery", &status);
    free(text);
    if (status != 0) {
        snprintf(why, size, "mcopy of the keyless image exited %d", status);
        return -1;
    }
    if (find_bytes(disk, mark, sizeof(mark) - 1, &env) != 0) {
        snprintf(why, size, "the disk has no GRUB environment block");
        return -1;
    }
    if ((fd = open(disk, O_RDWR | O_CLOEXEC)) < 0 ||
        pread(fd, block, sizeof(block), (off_t)env) != (ssize_t)sizeof(block) ||
        armed_block(block, armed) != 0 ||
        pwrite(fd, armed, sizeof(armed), (off_t)env) != (ssize_t)sizeof(armed)) {
        if (fd >= 0)
            close(fd);
        snprintf(why, size, "the GRUB environment block at %" PRIu64 " could not be armed", env);
        return -1;
    }
    if (close(fd) != 0) {
        snprintf(why, size, "the disk could not be written");
        return -1;
    }
    return 0;
}

int inst_has(const char *text, size_t from, const char *needle)
{
    return text != NULL && strlen(text) >= from && strstr(text + from, needle) != NULL;
}
