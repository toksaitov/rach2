/*
 * drill_scenario.c: the scenario of the cases of
 * tests/cases/machine.drill.json, the recovery drill on a small GRUB disk
 * that has the three partitions of an installed machine.
 *
 * machine.drill.repair boots one copy of the disk through SeaBIOS and GRUB
 * again and again, changing the copy between the boots as the drill does to
 * a machine: the everyday kernel refuses to write its own disk and is shown
 * to have written nothing; the root's superblock is destroyed with the
 * machine stopped, and the next boot stops without a login; the recovery
 * entry, armed in the environment block, comes up from partition 2, takes a
 * lease and answers SSH with nothing typed at its console; the whole root
 * arrives through one ssh pipe and the copy then has the image's bytes;
 * recovery saved as the default comes back after a power cut; the machine
 * boots from the repaired root; a recovery payload that will not load falls
 * back to a kernel on partition 1; and a destroyed partition 1 still reaches
 * recovery through the core image.
 *
 * machine.drill.publish reads the everyday kernel's authority off the
 * artifact with the tree's check_recovery.sh, and boots the recovery kernel
 * directly to show that it publishes /dev/disk0 with no rach2.diskdiag.
 * machine.drill.no-dhcp boots the recovery kernel directly with its card's
 * link down, a network on which no DHCP server answers, and requires the
 * recovery image to take its fixed address and stop its DHCP client.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "netcase.h"

/* What init in the recovery image says first, whether the image has a key,
 * and what it says each time it starts the SSH server. */
#define RECOVERY_UP  "recovery image"
#define SSHD_STARTED "init: sshd: started as pid "

/* What the kernel says when the root named on its boot line is not ext2,
 * when it mounts an ext2 root, and when it halts a boot that mounted none. */
#define ROOT_GONE "has no ext2 file system this kernel mounts"
#define ROOT_UP   "ext2: root: "
#define NO_ROOT   "refused: no file system mounted and no rach2.rescue"

/* The recovery shell's prompt, which is root's in its home directory. */
#define RECOVERY_PROMPT "~ #"

/* The recovery image's fixed address, the sealed disk's (SEALED_ADDRESS in
 * the root Makefile), and the line rescue-static logs once it has it. */
#define FIXED_ADDRESS "192.168.0.105"
#define FIXED_LOGGED  "rescue: eth0: " FIXED_ADDRESS

/* GRUB's environment block: its first line, and its size. */
#define ENV_HEADER "# GRUB Environment Block"
#define ENV_BYTES  1024

/* The bytes destroyed at the start of partition 1 and of partition 2. */
#define P1_WIPE (2L * 1024 * 1024)
#define P2_WIPE (1L * 1024 * 1024)

/* Where each partition the drill touches is on the disk, in bytes. */
struct layout {
    long p1;   /* the boot partition */
    long p2;   /* the recovery partition */
    long root; /* the ext2 root, the one entry of type 0x83 */
    long root_bytes;
    long env; /* the environment block */
};

/* What the steps of a case share. */
struct drill {
    struct net_case u;
    struct layout at;
    const char *disk; /* the artifact, read-only */
    const char *copy; /* the attempt's private copy of it */
    char port[16];
    char key[4096];     /* the private copy of the demo key */
    char password[128]; /* root's password on the everyday root */
    unsigned boots[2];  /* the boots of `everyday` and `networked` so far */
};

/** Say whether `text` has `part`; no text has nothing. */
static int has(const char *text, const char *part)
{
    return text != NULL && strstr(text, part) != NULL;
}

/** Say whether the run `r` printed `part` on its standard output or error. */
static int printed(const struct net_run *r, const char *part)
{
    return has(r->out, part) || has(r->err, part);
}

/** Give the first line of `r`'s standard error, or of its output when that
 * is empty, in `buf` of `size` bytes, to show in a failure. */
static const char *said(const struct net_run *r, char *buf, size_t size)
{
    const char *text = r->err != NULL && r->err[0] != '\0' ? r->err : r->out;

    return net_copy_line(text != NULL ? text + strspn(text, "\r\n") : NULL, buf, size);
}

/** Give the little-endian 32-bit number at `p`. */
static unsigned long le32(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 |
           (unsigned long)p[3] << 24;
}

/**
 * Read the partition table and the environment block's place off the disk
 * at `path` into `at`. The root is the table's one entry of type 0x83, and
 * partitions 1 and 2 are its first two entries, as mkmbr writes them.
 *
 * @return 0, or -1 when the disk cannot be read or lacks one of them.
 */
static int read_layout(const char *path, struct layout *at)
{
    static const char header[] = ENV_HEADER;
    unsigned char sector[512], *bytes = NULL;
    const unsigned char *e;
    long size = -1, i;
    size_t n = sizeof(header) - 1;
    FILE *f = fopen(path, "rb");
    int k, roots = 0;

    memset(at, 0, sizeof(*at));
    at->env = -1;
    if (f == NULL)
        return -1;
    if (fread(sector, 1, sizeof(sector), f) == sizeof(sector) && fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size > 0 && (bytes = malloc((size_t)size)) != NULL && fseek(f, 0, SEEK_SET) == 0 &&
        fread(bytes, 1, (size_t)size, f) == (size_t)size) {
        for (i = 0; at->env < 0 && i + (long)n <= size; i++)
            if (memcmp(bytes + i, header, n) == 0)
                at->env = i;
    }
    free(bytes);
    fclose(f);
    if (size <= 0 || sector[510] != 0x55 || sector[511] != 0xaa)
        return -1;
    for (k = 0; k < 4; k++) {
        e = sector + 446 + 16 * k;
        if (k == 0)
            at->p1 = (long)le32(e + 8) * 512;
        if (k == 1)
            at->p2 = (long)le32(e + 8) * 512;
        if (e[4] == 0x83 && le32(e + 12) > 0) {
            at->root = (long)le32(e + 8) * 512;
            at->root_bytes = (long)le32(e + 12) * 512;
            roots++;
        }
    }
    return at->p1 > 0 && at->p2 > 0 && roots == 1 && at->env >= 0 ? 0 : -1;
}

/**
 * Write `n` bytes of `data`, or zeros when it is NULL, at byte `offset` of
 * the file at `path`, which must have them already.
 *
 * @return 0, or -1 when they cannot be written.
 */
static int put_bytes(const char *path, long offset, const void *data, long n)
{
    static const char zeros[65536];
    FILE *f = fopen(path, "r+b");
    long done = 0, part;
    int rc = -1;

    if (f != NULL && fseek(f, offset, SEEK_SET) == 0) {
        for (; done < n; done += part) {
            part = n - done < (long)sizeof(zeros) ? n - done : (long)sizeof(zeros);
            if (fwrite(data != NULL ? (const char *)data + done : zeros, 1, (size_t)part, f) !=
                (size_t)part)
                break;
        }
        rc = done == n ? 0 : -1;
    }
    if (f != NULL && fclose(f) != 0)
        rc = -1;
    return rc;
}

/**
 * Copy `n` bytes from byte `from` of the file at `src` to a new file at
 * `dst`, or the whole of `src` when `n` is negative.
 *
 * @return 0, or -1 when they cannot be copied whole.
 */
static int copy_bytes(const char *src, long from, long n, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    char buf[65536];
    long done = 0;
    size_t want, got;
    int rc = -1;

    if (in != NULL && out != NULL && fseek(in, from, SEEK_SET) == 0) {
        for (;;) {
            want = n < 0 || n - done > (long)sizeof(buf) ? sizeof(buf) : (size_t)(n - done);
            if (want == 0 || (got = fread(buf, 1, want, in)) == 0)
                break;
            if (fwrite(buf, 1, got, out) != got)
                break;
            done += (long)got;
        }
        rc = ferror(in) == 0 && ferror(out) == 0 && (n < 0 || done == n) ? 0 : -1;
    }
    if (in != NULL)
        fclose(in);
    if (out != NULL && fclose(out) != 0)
        rc = -1;
    return rc;
}

/**
 * Say whether `n` bytes at byte `a_at` of the file at `a` are those at
 * `b_at` of `b`: 1 when they are, 0 when they differ, and -1 when either
 * cannot be read.
 */
static int same_bytes(const char *a, long a_at, const char *b, long b_at, long n)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char ba[65536], bb[65536];
    size_t want;
    int same = -1;

    if (fa != NULL && fb != NULL && fseek(fa, a_at, SEEK_SET) == 0 &&
        fseek(fb, b_at, SEEK_SET) == 0) {
        same = 1;
        for (; n > 0 && same == 1; n -= (long)want) {
            want = n > (long)sizeof(ba) ? sizeof(ba) : (size_t)n;
            if (fread(ba, 1, want, fa) != want || fread(bb, 1, want, fb) != want)
                same = -1;
            else if (memcmp(ba, bb, want) != 0)
                same = 0;
        }
    }
    if (fa != NULL)
        fclose(fa);
    if (fb != NULL)
        fclose(fb);
    return same;
}

/**
 * Arm the recovery entry for the next boot in the environment block of the
 * copy, as an operator arms it from the console: the 1024 bytes of the
 * block, its two comment lines, `next_entry=recovery`, the saved default
 * `candidate`, and a fill of `#`, the way grub-editenv writes one.
 */
static void arm_recovery(struct drill *s)
{
    char env[ENV_BYTES];
    int n;

    if (!net_ok(&s->u))
        return;
    memset(env, '#', sizeof(env));
    n = snprintf(env, sizeof(env),
                 ENV_HEADER "\n"
                            "# WARNING: Do not edit this file by tools other than grub-editenv!!!\n"
                            "next_entry=recovery\n"
                            "saved_entry=candidate\n");
    env[n] = '#';
    if (put_bytes(s->copy, s->at.env, env, sizeof(env)) != 0)
        net_fail(&s->u, "the environment block could not be written");
}

/** Zero `n` bytes at byte `offset` of the copy, naming `what` in a failure. */
static void destroy(struct drill *s, long offset, long n, const char *what)
{
    if (net_ok(&s->u) && put_bytes(s->copy, offset, NULL, n) != 0)
        net_fail(&s->u, "%s could not be destroyed", what);
}

/** Boot `machine`, `everyday` or `networked`, on the copy of the disk. */
static void boot(struct drill *s, struct net_boot *b, const char *machine)
{
    unsigned *n = &s->boots[strcmp(machine, "everyday") == 0 ? 0 : 1];

    net_boot(b, &s->u, machine, ++*n, NULL);
}

/** End the machine of `b` with QMP's `quit`, as a power cut would, unless it
 * has ended, and wait for its end. */
static void cut(struct net_boot *b)
{
    struct etude_termination end;
    char *reply = NULL;

    if (!net_ok(b->u))
        return;
    etude_qmp(b->m, "quit", NULL, &reply);
    free(reply);
    if (etude_wait_end(b->m, &end) != ETUDE_OK)
        net_fail(b->u, "the machine did not end after QMP's quit");
}

/** Wait for the machine of `b` to end by itself, as it does once the guest
 * has powered it off. */
static void ended(struct net_boot *b)
{
    struct etude_termination end;

    if (net_ok(b->u) && etude_wait_end(b->m, &end) != ETUDE_OK)
        net_fail(b->u, "the machine did not end after it was powered off");
}

/** Say whether the console of `b`, whose machine has ended, has `text`. */
static int found(struct net_boot *b, const char *text)
{
    uint64_t at;

    return net_ok(b->u) && etude_console_find(b->m, text, 0, &at) == ETUDE_OK;
}

/**
 * Wait for `text` on the console of `b` after the offset read so far, then
 * for the end of its line, and give the rest of that line in `buf` of
 * `size` bytes, or an empty text when the waits fail.
 */
static const char *line_after(struct net_boot *b, const char *text, char *buf, size_t size)
{
    uint64_t from;
    char *rest;

    buf[0] = '\0';
    if (net_wait(b, text) != ETUDE_OK)
        return buf;
    from = b->at;
    if (net_wait(b, "\n") != ETUDE_OK)
        return buf;
    rest = net_text(b, from, b->at);
    net_copy_line(rest, buf, size);
    free(rest);
    return buf;
}

/** Run `command` as root on the networked machine by the demo key, keeping
 * what it prints as evidence `output`. */
static enum etude_status as_root(struct drill *s, const char *command, const char *output,
                                 struct net_run *r)
{
    char *argv[] = {NET_SSH,          NET_SSH_KEY(s->key), "-T", "-p", s->port,
                    "root@127.0.0.1", (char *)command,     NULL};

    return net_host(&s->u, argv, output, r);
}

/**
 * Once init has said that it started the SSH server, run `cat
 * /var/log/services` as root until ssh exits 0, since a connection made
 * while the server starts is reset at the key exchange; report `id` with
 * the count of tries, and leave the last run in `r`.
 */
static void reach(struct drill *s, struct net_boot *b, const char *id, struct net_run *r)
{
    unsigned tries = 0;

    memset(r, 0, sizeof(*r));
    if (net_wait(b, SSHD_STARTED) == ETUDE_OK) {
        do {
            net_run_free(r);
            tries++;
            as_root(s, "cat /var/log/services", "services", r);
        } while (net_ok(&s->u) && !net_exited0(r));
    }
    net_assert(&s->u, id, net_exited0(r), "no key login answered in %u tries", tries);
    if (net_ok(&s->u))
        printf("drill: the machine answered SSH on try %u\n", tries);
}

/** Step `refuse`: the machine as it shipped refuses to write its own disk,
 * and the refusals write nothing. */
static void refuse(struct drill *s)
{
    struct net_boot b;
    char line[200];

    net_step(&s->u, "refuse");
    boot(s, &b, "everyday");
    net_login(&b, "root", s->password, "The ISH shell");
    net_line(&b, "reflash /dev/disk0 0 512 < /dev/zero");
    line_after(&b, "reflash: ", line, sizeof(line));
    net_assert(&s->u, "refuse-reflash", has(line, "Resource busy"), "reflash said: %s", line);
    net_line(&b, "cat /etc/motd > /dev/disk0");
    net_line(&b, "halt");
    ended(&b);
    net_assert(&s->u, "refuse-redirect", found(&b, "cannot open a redirection"),
               "the shell did not refuse its redirect into /dev/disk0");
    net_assert(&s->u, "sector0-unchanged", same_bytes(s->disk, 0, s->copy, 0, 512) == 1,
               "the refused writes changed the first sector of the disk");
}

/** Step `broken`: the root's superblock destroyed with the machine stopped,
 * the next boot stops without a login. */
static void broken(struct drill *s)
{
    struct net_boot b;

    net_step(&s->u, "broken");
    destroy(s, s->at.root + 1024, 1024, "the root's superblock");
    boot(s, &b, "everyday");
    net_wait(&b, NO_ROOT);
    cut(&b);
    net_assert(&s->u, "root-refused", found(&b, ROOT_GONE),
               "the kernel did not say that the root has no ext2 file system");
    net_assert(&s->u, "no-login", !found(&b, "login:"),
               "a machine whose root is destroyed still reached a login");
}

/** Run `command` as root and require the line that `marker` begins, which
 * the command prints, to be followed by `want`; report `id`, with `what` in
 * a failure, and say whether it held. */
static int check(struct drill *s, const char *id, const char *command, const char *marker,
                 const char *want, const char *what)
{
    char line[200] = "", text[64];
    struct net_run r;
    const char *at;
    int held;

    as_root(s, command, id, &r);
    at = r.out != NULL ? strstr(r.out, marker) : NULL;
    if (at != NULL)
        net_copy_line(at + strlen(marker), text, sizeof(text));
    held = net_ok(&s->u) && at != NULL && strcmp(text, want) == 0;
    net_assert(&s->u, id, held, "%s: %s", what, at != NULL ? text : said(&r, line, sizeof(line)));
    net_run_free(&r);
    return held;
}

/**
 * Step `recovery`: the recovery entry, armed in the environment block, comes
 * up on partition 2, takes a lease and answers SSH; it has the whole-disk
 * device, refuses a write past its end, and admits one raw writer at a time.
 * The machine stays up for the steps after it.
 */
static void recovery(struct drill *s, struct net_boot *b)
{
    char line[200];
    struct net_run r;
    int refused;

    net_step(&s->u, "recovery");
    arm_recovery(s);
    boot(s, b, "networked");
    net_wait(b, RECOVERY_UP);
    net_assert(&s->u, "recovery-up", 1, "");
    reach(s, b, "answers", &r);
    net_assert(&s->u, "lease", has(r.out, "lease of"), "the service log has no lease");
    net_run_free(&r);
    check(s, "disk-node", "test -b /dev/disk0; echo DISK\"\"NODE=$?", "DISKNODE=", "0",
          "recovery has no whole-disk device");
    snprintf(line, sizeof(line), "reflash /dev/disk0 %ld 999999999488 < /dev/zero", s->at.root);
    as_root(s, line, "past-end", &r);
    net_assert(&s->u, "past-end", printed(&r, "past the end"),
               "a write past the end of the device was not refused: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    check(s, "grubcore-alone", "sh -c \"exec 3>/dev/grubcore\" 2>/dev/null; echo ALONE=$?",
          "ALONE=", "0", "the loader's gap could not be opened for writing on its own");
    as_root(s, "sh -c \"exec 3>/dev/disk0; exec 4>/dev/grubcore\" 2>/dev/null; echo BOTH=$?",
            "second-writer", &r);
    refused = has(r.out, "BOTH=") && !has(r.out, "BOTH=0");
    net_assert(&s->u, "second-writer", refused,
               "a second raw writer was admitted while the whole disk was held: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/** Step `repair`: the root as built arrives through one ssh pipe, and the
 * copy of the disk then has its bytes. */
static void repair(struct drill *s)
{
    char image[4096] = "", command[128], want[64], line[200];
    char *argv[] = {"sh",
                    "-c",
                    "f=$1; shift; gzip -c \"$f\" | \"$@\"",
                    "sh",
                    image,
                    NET_SSH,
                    NET_SSH_KEY(s->key),
                    "-T",
                    "-p",
                    s->port,
                    "root@127.0.0.1",
                    command,
                    NULL};
    struct net_run r;

    net_step(&s->u, "repair");
    snprintf(command, sizeof(command), "gunzip | reflash /dev/disk0 %ld %ld", s->at.root,
             s->at.root_bytes);
    snprintf(want, sizeof(want), "wrote %ld bytes", s->at.root_bytes);
    if (net_path(&s->u, "root.img", image, sizeof(image)) != NULL &&
        copy_bytes(s->disk, s->at.root, s->at.root_bytes, image) != 0)
        net_fail(&s->u, "the root could not be read off the disk");
    net_host(&s->u, argv, "repair", &r);
    net_assert(&s->u, "repair-length", has(r.out, want),
               "the repair did not report the whole length written: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    as_root(s, "sync", "sync", &r);
    net_run_free(&r);
    net_assert(&s->u, "repair-bytes",
               same_bytes(image, 0, s->copy, s->at.root, s->at.root_bytes) == 1,
               "the root partition does not have the bytes of the image sent");
}

/**
 * Step `saved`: the saved default read back before the repair is left as
 * it was, recovery saved as the default, and the machine powered off as a
 * power cut would stop it. The default read back is left in `prev`.
 */
static void saved(struct drill *s, struct net_boot *b, char *prev, size_t size)
{
    struct net_run r;
    const char *at;

    net_step(&s->u, "saved");
    as_root(s, "bootctl env saved_entry candidate", "saved.set", &r);
    net_run_free(&r);
    as_root(s, "bootctl env saved_entry", "saved.read", &r);
    at = r.out != NULL ? strstr(r.out, "bootctl: saved_entry: ") : NULL;
    net_copy_line(at != NULL ? at + strlen("bootctl: saved_entry: ") : NULL, prev, size);
    net_run_free(&r);
    net_assert(&s->u, "saved-read", strcmp(prev, "candidate") == 0 || strcmp(prev, "fallback") == 0,
               "the saved default could not be read back before the repair: '%s'", prev);
    if (!check(s, "saved-recovery", "bootctl env saved_entry recovery && bootctl env saved_entry",
               "bootctl: saved_entry: ", "recovery",
               "recovery could not be left as the saved default") &&
        net_ok(&s->u)) {
        net_fail(&s->u, "recovery is not the saved default, so the next boot would not test it");
        s->u.told = 1;
    }
    as_root(s, "sync; poweroff -f", "poweroff", &r);
    net_run_free(&r);
    ended(b);
}

/** Step `latched`: with nothing armed for one boot, the saved default
 * brings recovery back, and the default read before is put back. */
static void latched(struct drill *s, const char *prev)
{
    char command[128];
    struct net_boot b;
    struct net_run r;

    net_step(&s->u, "latched");
    boot(s, &b, "networked");
    net_wait(&b, RECOVERY_UP);
    net_assert(&s->u, "latched-recovery", 1, "");
    reach(s, &b, "latched-answers", &r);
    net_run_free(&r);
    check(s, "latched-still", "test -b /dev/disk0; echo STILL\"\"RECOVERY=$?",
          "STILLRECOVERY=", "0", "the machine that came back was not recovery");
    snprintf(command, sizeof(command), "bootctl env saved_entry %s && bootctl env saved_entry",
             prev);
    check(s, "saved-restored", command, "bootctl: saved_entry: ", prev,
          "the saved default could not be put back to what it was");
    as_root(s, "sync; poweroff -f", "poweroff.again", &r);
    net_run_free(&r);
    ended(&b);
}

/** Step `repaired`: the ordinary entry mounts the rewritten root and offers
 * a login. */
static void repaired(struct drill *s)
{
    struct net_boot b;

    net_step(&s->u, "repaired");
    boot(s, &b, "everyday");
    net_wait(&b, "rach2 login:");
    net_assert(&s->u, "repaired-login", 1, "");
    cut(&b);
    net_assert(&s->u, "repaired-root", found(&b, ROOT_UP),
               "the repaired machine did not mount its root");
}

/**
 * Step `fallback`: partition 2 destroyed and the recovery entry armed, the
 * machine falls back to a kernel on partition 1 that mounts the root on
 * partition 3. The copy is put back as it was before the step afterwards.
 */
static void fallback(struct drill *s)
{
    char save[4096] = "";
    struct net_boot b;

    net_step(&s->u, "fallback");
    if (net_path(&s->u, "drill.save", save, sizeof(save)) != NULL &&
        copy_bytes(s->copy, 0, -1, save) != 0)
        net_fail(&s->u, "the copy of the disk could not be saved");
    destroy(s, s->at.p2, P2_WIPE, "partition 2");
    arm_recovery(s);
    boot(s, &b, "everyday");
    net_wait(&b, "rach2 login:");
    net_assert(&s->u, "fallback-login", 1, "");
    cut(&b);
    net_assert(&s->u, "fallback-root", found(&b, ROOT_UP),
               "a recovery entry that could not load did not fall back to a kernel on partition 1");
    net_assert(&s->u, "fallback-kernel", !found(&b, "modules: found"),
               "the fallback loaded a RAM image, so it was recovery and not a kernel on "
               "partition 1");
    if (net_ok(&s->u) && copy_bytes(save, 0, -1, s->copy) != 0)
        net_fail(&s->u, "the copy of the disk could not be put back");
}

/** Step `core`: partition 1 destroyed, the core image's second line still
 * reaches recovery, whose shell answers. */
static void core(struct drill *s)
{
    struct net_boot b;
    char *text = NULL;

    net_step(&s->u, "core");
    destroy(s, s->at.p1, P1_WIPE, "partition 1");
    boot(s, &b, "everyday");
    net_wait(&b, RECOVERY_PROMPT);
    net_command(&b, "echo P1\"\"DEAD", "P1DEAD", &text);
    free(text);
    net_assert(&s->u, "core-shell", 1, "");
    cut(&b);
    net_assert(&s->u, "core-recovery", found(&b, "modules: found 1"),
               "the boot with partition 1 destroyed did not load a RAM image, so it was not "
               "recovery");
}

/** The case machine.drill.repair. */
static int drill_repair(struct etude_case *c)
{
    struct drill s;
    struct net_boot b;
    char prev[32] = "";
    unsigned port;
    int laid;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    port = etude_port(c, 0);
    snprintf(s.port, sizeof(s.port), "%u", port);
    if (port == 0)
        net_fail(&s.u, "the case has no port");
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    net_key(&s.u, "demo-key", s.key, sizeof(s.key));
    s.disk = etude_artifact(c, "drill-disk");
    s.copy = etude_private_copy(c, "drill-disk");
    if (net_ok(&s.u) && (s.disk == NULL || s.copy == NULL))
        net_fail(&s.u, "the drill disk has no private copy");
    laid = net_ok(&s.u) && read_layout(s.disk, &s.at) == 0;
    net_assert(&s.u, "layout", laid,
               "the disk's table lacks partition 1, partition 2 or one ext2 root, or it has no "
               "environment block");
    if (!laid)
        net_fail(&s.u, "the disk's layout could not be read");
    refuse(&s);
    broken(&s);
    recovery(&s, &b);
    repair(&s);
    saved(&s, &b, prev, sizeof(prev));
    latched(&s, prev);
    repaired(&s);
    fallback(&s);
    core(&s);
    return net_end(&s.u);
}

/** Give in `buf` of `size` bytes the absolute path of `path`, a file of the
 * tree that ETUDE_TREE names, or of the working directory without it, and
 * say whether it is there. */
static int in_tree(const char *path, char *buf, size_t size)
{
    char dir[4096];
    const char *tree = getenv("ETUDE_TREE");

    if (tree == NULL || tree[0] != '/')
        tree = getcwd(dir, sizeof(dir));
    return tree != NULL && (size_t)snprintf(buf, size, "%s/%s", tree, path) < size &&
           access(buf, R_OK) == 0;
}

/** The case machine.drill.publish. */
static int drill_publish(struct etude_case *c)
{
    char script[4096] = "", line[200];
    const char *kernel = etude_artifact(c, "kernel");
    char *argv[] = {"sh", script, (char *)kernel, "recovery", NULL};
    struct net_case u;
    struct net_boot b;
    struct net_run r;

    net_case(&u, c);
    if (!in_tree("tools/inspect/check_recovery.sh", script, sizeof(script)) || kernel == NULL)
        net_fail(&u, "tools/inspect/check_recovery.sh is not in the tree: set ETUDE_TREE to its "
                     "root");
    net_step(&u, "artifact");
    net_host(&u, argv, "check-recovery", &r);
    net_assert(&u, "artifact-refused",
               r.end.end == ETUDE_END_EXITED && r.end.value == 1 &&
                   printed(&r, "does not carry the recovery banner"),
               "the everyday kernel passed the recovery artifact check: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    net_step(&u, "publish");
    net_boot(&b, &u, "ramboot", 1, NULL);
    net_wait(&b, RECOVERY_UP);
    net_wait(&b, RECOVERY_PROMPT);
    net_line(&b, "test -b /dev/disk0; echo DISK\"\"NODE=$?");
    line_after(&b, "DISKNODE=", line, sizeof(line));
    net_assert(&u, "disk-node", strcmp(line, "0") == 0,
               "the recovery kernel did not publish /dev/disk0 without rach2.diskdiag: %s", line);
    cut(&b);
    return net_end(&u);
}

/** The case machine.drill.no-dhcp. */
static int drill_no_dhcp(struct etude_case *c)
{
    char line[200], *reply = NULL;
    struct net_case u;
    struct net_boot b;

    net_case(&u, c);
    net_step(&u, "boot");
    net_boot(&b, &u, "isolated", 1, NULL);
    if (net_ok(&u) &&
        etude_qmp(b.m, "set_link", "{\"name\": \"net0\", \"up\": false}", &reply) != ETUDE_OK)
        net_fail(&u, "the card's link could not be taken down: %s", reply != NULL ? reply : "");
    free(reply);
    net_wait(&b, RECOVERY_UP);
    net_wait(&b, RECOVERY_PROMPT);
    net_step(&u, "fixed");
    line_after(&b, "eth0: IPv4 ", line, sizeof(line));
    net_assert(&u, "fixed-address", strcmp(line, FIXED_ADDRESS "/24") == 0,
               "the first address eth0 took was %s, not the fixed " FIXED_ADDRESS, line);
    net_line(&b, "while ! grep -q '" FIXED_LOGGED "' /var/log/services; do :; done; "
                 "ifconfig eth0 | grep -q 'addr:" FIXED_ADDRESS "'; echo ADDR\"\"=$?; "
                 "pidof udhcpc > /dev/null; echo CLIENT\"\"=$?");
    line_after(&b, "ADDR=", line, sizeof(line));
    net_assert(&u, "fixed-kept", strcmp(line, "0") == 0,
               "ifconfig does not give eth0 the fixed address once rescue-static has logged it");
    line_after(&b, "CLIENT=", line, sizeof(line));
    net_assert(&u, "client-stopped", strcmp(line, "1") == 0,
               "a DHCP client still runs beside the fixed address");
    cut(&b);
    return net_end(&u);
}

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c);

    if (strcmp(id, "machine.drill.repair") == 0)
        return drill_repair(c);
    if (strcmp(id, "machine.drill.publish") == 0)
        return drill_publish(c);
    if (strcmp(id, "machine.drill.no-dhcp") == 0)
        return drill_no_dhcp(c);
    fprintf(stderr, "drill_scenario: no case %s\n", id);
    return 1;
}
