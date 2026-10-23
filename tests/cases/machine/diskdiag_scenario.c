/*
 * diskdiag_scenario.c: the scenario of machine.diskdiag.read
 * (tests/cases/machine.diskdiag.json). The read-only kernel's diagnostic,
 * reached over SSH with nothing typed at the console, reports a drive and
 * hands out its sectors without writing a byte, and reports a missing drive.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "netcase.h"
#include "sha256.h"

/* The most of an SSH command's output that is read back. */
#define OUTPUT_BYTES 8192

/* The private copy of the demo key, which ssh(1) takes only when no one
 * else may read it, and the case's port that QEMU forwards to the guest's
 * SSH server. */
static char key[4096];
static char port[16];
static struct net_case network;

/**
 * Say whether files `a` and `b` have the same bytes: 1 when they do, 0 when
 * they differ, and -1 when either cannot be read.
 */
static int same_bytes(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char ba[65536], bb[65536];
    size_t na, nb;
    int same = -1;

    if (fa != NULL && fb != NULL) {
        do {
            na = fread(ba, 1, sizeof(ba), fa);
            nb = fread(bb, 1, sizeof(bb), fb);
        } while (na == nb && na > 0 && memcmp(ba, bb, na) == 0);
        if (ferror(fa) == 0 && ferror(fb) == 0)
            same = na == 0 && nb == 0;
    }
    if (fa != NULL)
        fclose(fa);
    if (fb != NULL)
        fclose(fb);
    return same;
}

/** Say whether the console of `m`, which has ended, has `text`. */
static int said(struct etude_case_machine *m, const char *text)
{
    uint64_t at;

    return etude_console_wait(m, text, 0, &at) == ETUDE_OK;
}

/** End machine `m` with QMP's `quit`, as a power cut would, and wait for it. */
static void cut(struct etude_case_machine *m)
{
    struct etude_termination end;
    char *reply = NULL;

    etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    etude_wait_end(m, &end);
}

/**
 * Read at most `size` - 1 bytes of file `path` into `buf` as a string, with
 * carriage returns dropped; the empty string when it cannot be read.
 */
static char *read_text(const char *path, char *buf, size_t size)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    size_t n = 0;
    int ch;

    if (f != NULL) {
        while (n + 1 < size && (ch = getc(f)) != EOF)
            if (ch != '\r')
                buf[n++] = (char)ch;
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

/** Say whether `text` has a line that is `line` whole. */
static int has_line(const char *text, const char *line)
{
    size_t len = strlen(line);
    const char *p = text;

    while ((p = strstr(p, line)) != NULL) {
        if ((p == text || p[-1] == '\n') && (p[len] == '\n' || p[len] == '\0'))
            return 1;
        p += len;
    }
    return 0;
}

/**
 * Copy artifact `demo-key` to evidence `ssh.key`, readable by its owner
 * alone, and make it the key; 0, or -1 when it could not.
 */
static int private_key(struct etude_case *c)
{
    (void)c;
    return net_key(&network, "demo-key", key, sizeof(key)) != NULL ? 0 : -1;
}

/**
 * Run `command` as root on the diagnostic machine over SSH by the demo key,
 * its standard output kept as evidence `output` and read into `buf`.
 *
 * @return ETUDE_OK when ssh(1) exited 0, ETUDE_FAILED when it ended
 *         otherwise, and the status of etude_host when it did not run.
 */
static enum etude_status ssh_run(struct etude_case *c, const char *command, const char *output,
                                 char *buf, size_t size)
{
    char *argv[] = {"ssh",
                    "-i",
                    key,
                    "-p",
                    port,
                    "-o",
                    "StrictHostKeyChecking=no",
                    "-o",
                    "UserKnownHostsFile=/dev/null",
                    "-o",
                    "BatchMode=yes",
                    "-o",
                    "LogLevel=ERROR",
                    "root@127.0.0.1",
                    (char *)command,
                    NULL};
    struct net_run run;
    enum etude_status st = net_host(&network, argv, output, &run);
    int ok = net_exited0(&run);

    read_text(etude_evidence(c, output), buf, size);
    net_run_free(&run);
    if (st != ETUDE_OK)
        return st;
    return ok ? ETUDE_OK : ETUDE_FAILED;
}

/**
 * Give in `hex` the SHA-256 of sector `lba` of file `path`; 0, or -1 when it
 * cannot be read whole.
 */
static int host_sector(const char *path, unsigned long lba, char hex[65])
{
    FILE *f = fopen(path, "rb");
    unsigned char sector[512], digest[32];
    struct sha256 s;
    int rc = -1;

    if (f == NULL)
        return -1;
    if (fseek(f, (long)(lba * 512), SEEK_SET) == 0 && fread(sector, 1, 512, f) == 512) {
        sha256_init(&s);
        sha256_update(&s, sector, 512);
        sha256_final(&s, digest);
        sha256_hex(digest, hex);
        rc = 0;
    }
    fclose(f);
    return rc;
}

/**
 * Report assertion `id` on sector `lba`: hashed on the machine through
 * /dev/disk0 and on the host out of the disk, the two must agree, which
 * shows that the machine returned the sector named and not another.
 */
static void check_sector(struct etude_case *c, const char *id, const char *disk, unsigned long lba)
{
    char command[160], output[32], text[OUTPUT_BYTES], want[65], detail[200];

    snprintf(command, sizeof(command),
             "dd if=/dev/disk0 bs=512 skip=%lu count=1 2>/dev/null | sha256sum", lba);
    snprintf(output, sizeof(output), "sector-%lu", lba);
    if (host_sector(disk, lba, want) != 0) {
        etude_assert(c, id, 0, "the disk's sector cannot be read on the host");
        return;
    }
    if (ssh_run(c, command, output, text, sizeof(text)) != ETUDE_OK) {
        etude_assert(c, id, 0, "the sector's hash could not be read over SSH");
        return;
    }
    snprintf(detail, sizeof(detail), "sector %lu reads back as %.64s, and the disk has %s", lba,
             text, want);
    etude_assert(c, id, strncmp(text, want, 64) == 0, detail);
}

/**
 * Step `control`: the read-write kernel mounts its copy of the disk, which
 * must change it, since the mount sets the dirty flag. Without it the
 * comparison of the diagnostic's copy could not find a write.
 */
static int control(struct etude_case *c)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at;

    if (etude_step(c, "control") != ETUDE_OK ||
        etude_boot(c, "control", NULL, &m, &end) != ETUDE_OK)
        return -1;
    if (etude_console_wait(m, "Type 'help'", 0, &at) != ETUDE_OK)
        return -1;
    cut(m);
    etude_assert(c, "control-mounted", said(m, "krffs: mounted:"),
                 "the read-write kernel did not mount the disk");
    etude_assert(c, "control-changed",
                 same_bytes(etude_artifact(c, "rw-disk"), etude_private_copy(c, "rw-disk")) == 0,
                 "the read-write boot left its copy as the disk is");
    return 0;
}

/**
 * Step `diagnostic`: the read-only kernel on its copy of the disk on AHCI,
 * reached over SSH once init has started the server, with nothing typed at
 * its console.
 */
static int diagnostic(struct etude_case *c)
{
    static const char *const lines[] = {"state present", "driver ahci", "policy read-only",
                                        "mounted 0", "mbr-signature ok"};
    const char *disk = etude_artifact(c, "ro-disk");
    struct etude_case_machine *m;
    struct etude_termination end;
    char text[OUTPUT_BYTES], want[64], detail[200];
    unsigned long sectors, tries = 0;
    enum etude_status st;
    struct stat sb;
    uint64_t at;
    size_t i;
    int ok;

    if (etude_step(c, "diagnostic") != ETUDE_OK || disk == NULL || stat(disk, &sb) != 0 ||
        etude_boot(c, "diagnostic", NULL, &m, &end) != ETUDE_OK)
        return -1;
    sectors = (unsigned long)(sb.st_size / 512);
    snprintf(port, sizeof(port), "%u", etude_port(c, 0));
    if (etude_console_wait(m, "rach2 login", 0, &at) != ETUDE_OK ||
        etude_console_wait(m, "init: sshd: started as pid", 0, &at) != ETUDE_OK) {
        fprintf(stderr, "diskdiag: init did not report starting the SSH server\n");
        return -1;
    }
    do {
        tries++;
        st = ssh_run(c, "echo READY", "ready", text, sizeof(text));
    } while (st == ETUDE_FAILED);
    snprintf(detail, sizeof(detail), "no answer over SSH in %lu tries", tries);
    etude_assert(c, "answers", st == ETUDE_OK && has_line(text, "READY"), detail);
    if (st != ETUDE_OK)
        return -1;
    printf("diskdiag: the machine answered SSH on try %lu\n", tries);

    ssh_run(c, "cat /proc/disk", "proc-disk", text, sizeof(text));
    for (ok = 1, i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        if (!has_line(text, lines[i])) {
            snprintf(detail, sizeof(detail), "/proc/disk does not say %s", lines[i]);
            ok = 0;
            break;
        }
    etude_assert(c, "proc-disk", ok, detail);
    snprintf(want, sizeof(want), "sectors %lu", sectors);
    snprintf(detail, sizeof(detail), "/proc/disk does not give the disk's %lu sectors", sectors);
    etude_assert(c, "sectors", has_line(text, want), detail);
    etude_assert(c, "model", strncmp(text, "model ", 6) == 0 || strstr(text, "\nmodel ") != NULL,
                 "/proc/disk names no model");

    check_sector(c, "sector-first", disk, 0);
    check_sector(c, "sector-partition", disk, 2048);
    check_sector(c, "sector-last", disk, sectors - 1);

    ssh_run(c, "dd if=/dev/zero of=/dev/disk0 bs=512 count=1 2>&1 | head -1", "write", text,
            sizeof(text));
    cut(m);
    etude_assert(c, "no-refusal", !said(m, "refused: command"),
                 "the diagnostic issued a command the allowlist refused");
    etude_assert(c, "unchanged", same_bytes(disk, etude_private_copy(c, "ro-disk")) == 1,
                 "the diagnostic boot changed its copy of the disk");
    return 0;
}

/** Step `absent`: the same kernel with no drive says that none answered. */
static int absent(struct etude_case *c)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at;

    if (etude_step(c, "absent") != ETUDE_OK || etude_boot(c, "absent", NULL, &m, &end) != ETUDE_OK)
        return -1;
    if (etude_console_wait(m, "Type 'help'", 0, &at) != ETUDE_OK)
        return -1;
    cut(m);
    etude_assert(c, "absent", said(m, "diskdiag: absent: no disk answered"),
                 "with no drive the diagnostic did not report an absent disk");
    return 0;
}

int etude_scenario(struct etude_case *c)
{
    int failed = 1;

    net_case(&network, c);
    if (private_key(c) != 0)
        fprintf(stderr, "diskdiag: the demo key cannot be copied\n");
    else
        failed = control(c) != 0 || diagnostic(c) != 0 || absent(c) != 0;
    return net_end(&network) != 0 || failed;
}
