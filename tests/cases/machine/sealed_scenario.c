/*
 * sealed_scenario.c: the scenario of tests/cases/machine.sealed.json, the
 * disk that `make rach2-sealed-disk` writes, operated the way it is operated
 * once it is in the laptop: booted by SeaBIOS on AHCI, with nothing typed at
 * the console, and reached over SSH alone.
 *
 * machine.sealed.fault walks the whole life of the machine on one private
 * copy of the disk. It comes up on the ext2 root the candidate entries carry,
 * answers a key login and makes a volume in a file that a loop device
 * mounts; a kernel sent down the connection is installed as a one-boot
 * trial, shows its marker, and is committed; a candidate destroyed on the
 * host is caught by the fallback, which boots the same root on the previous
 * kernel, and is repaired from there; the root made missing, faulty and
 * dirty under an armed trial lands each time in the recovery system, which
 * repairs it; a menu is staged and then torn; the boot loader is replaced in
 * the BIOS boot partition through /dev/grubcore; and the root, destroyed
 * with the machine stopped, is rewritten from recovery through an ssh pipe.
 *
 * QEMU runs with -no-reboot, so a `reboot -f` ends a boot with a reset and
 * the next boot of the machine is a new QEMU on the same copy, which keeps
 * what the boots before it wrote. What the old suite did to the disk from
 * the host, this scenario does to the copy between boots.
 *
 * machine.sealed.control boots another copy with nothing injected and finds
 * no fallback and no recovery anywhere on its console; a run that entered
 * recovery on purpose could not show that.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "netcase.h"

/* What init prints each time it starts the SSH server, installed or in
 * recovery. */
#define SSHD_STARTED "init: sshd: started as pid "

/* The disk GUID of the sealed disk's table, by which its menu names the
 * root, and the lines that follow from it. */
#define DISK_ID      "GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001"
#define ROOT_CMDLINE "rach2.root=" DISK_ID ",3 rach2.rootfstype=ext2"
#define ROOT_MISSING "root: " DISK_ID ",3: /dev/disk0p3 has no ext2 file system this kernel mounts"

/* The kernel's first boot record, and the mark of the same length that the
 * kernel sent to the machine prints in its place. */
#define BANNER "Rach2 Booting"
#define MARK   "Rach2 SEALED!"

/* The staged menu's entry, and the header of GRUB's environment block. */
#define MENU_ENTRY "menuentry sealedone --id sealedone"
#define ENV_HEADER "# GRUB Environment Block"

/* What GRUB prints when an entry's kernel does not load, and what the
 * recovery entry's boot prints when GRUB gave it its RAM image. */
#define FALLING_BACK "Falling back to"
#define RECOVERY_RAM "modules: found"

/* The bytes of the environment block and of a menu that GRUB reads. */
#define ENV_BYTES  1024
#define MENU_BYTES 4096

/* The BIOS boot partition's sectors that hold the core GRUB booted from:
 * the tail from LBA 35, which `bootctl grub` leaves for the one at 1041. */
#define OLD_CORE_LBA     35
#define OLD_CORE_SECTORS 1006

/* One attempt of either case. */
struct sealed {
    struct net_case u;
    struct net_boot b;
    unsigned boots; /* the boots of the machine so far */
    char port[16];
    char key[4096];               /* the private copy of the demo key */
    char marked[4096];            /* the kernel with MARK for BANNER */
    const char *disk;             /* the private copy of the disk */
    const char *artifact;         /* the disk as it was built */
    unsigned long long boot_lba;  /* the first partition's first sector */
    unsigned long long root_lba;  /* the ext2 root's first sector */
    unsigned long long root_size; /* and its sectors */
};

/** Say whether `text` has `part`; no text has nothing. */
static int has(const char *text, const char *part)
{
    return text != NULL && strstr(text, part) != NULL;
}

/** Say whether `text` has `part` in any mix of upper and lower case. */
static int has_any_case(const char *text, const char *part)
{
    size_t i, n = strlen(part);
    const char *p;

    for (p = text; p != NULL && *p != '\0'; p++) {
        for (i = 0; i < n; i++) {
            char a = p[i], b = part[i];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (a != b)
                break;
        }
        if (i == n)
            return 1;
    }
    return 0;
}

/** Say whether `r`'s standard output or error has a line with `pattern`,
 * as net_find matches it. */
static int printed(const struct net_run *r, const char *pattern)
{
    return net_find(r->out, pattern) != NULL || net_find(r->err, pattern) != NULL;
}

/** Give the first line of `r`'s standard error, or of its output when that
 * is empty, in `buf` of `size` bytes, to show in a failure. */
static const char *said(const struct net_run *r, char *buf, size_t size)
{
    const char *text = r->err != NULL && r->err[0] != '\0' ? r->err : r->out;

    return net_copy_line(text != NULL ? text + strspn(text, "\r\n") : NULL, buf, size);
}

/** Give the line of `r`'s output that has `pattern`, or its first line, in
 * `buf` of `size` bytes, to show in a failure. */
static const char *line_of(const struct net_run *r, const char *pattern, char *buf, size_t size)
{
    const char *line = net_find(r->out, pattern);

    return line != NULL ? net_copy_line(line, buf, size) : said(r, buf, size);
}

/**
 * Run `command` on the machine as root by the demo key, keeping what it
 * prints as evidence `output`. ssh reads no file of the account that runs
 * it, offers that key alone, and keeps no record of the host's key.
 */
static enum etude_status as_root(struct sealed *s, const char *command, const char *output,
                                 struct net_run *r)
{
    char *argv[] = {NET_SSH,          NET_SSH_KEY(s->key), "-T", "-p", s->port,
                    "root@127.0.0.1", (char *)command,     NULL};

    return net_host(&s->u, argv, output, r);
}

/** Run `command` as root with the file at `path` as its standard input,
 * as a file is sent down the connection, keeping its output as `output`. */
static enum etude_status send_file(struct sealed *s, const char *path, const char *command,
                                   const char *output, struct net_run *r)
{
    char *argv[] = {"sh",
                    "-c",
                    "f=$1; shift; exec \"$@\" < \"$f\"",
                    "sh",
                    (char *)path,
                    NET_SSH,
                    NET_SSH_KEY(s->key),
                    "-T",
                    "-p",
                    s->port,
                    "root@127.0.0.1",
                    (char *)command,
                    NULL};

    return net_host(&s->u, argv, output, r);
}

/** Run `command` as root and give its output and error in `r`, as as_root,
 * naming the evidence after the boot and `what`. */
static void ask(struct sealed *s, const char *command, const char *what, struct net_run *r)
{
    char output[64];

    snprintf(output, sizeof(output), "boot%u.%s", s->boots, what);
    as_root(s, command, output, r);
}

/** Wait until the server answers on the case's port, as sshpeer judges it:
 * a connection that has the server's identification. Each try follows the
 * guest's refusal of the one before. */
static void answered(struct sealed *s)
{
    char *argv[] = {"sshpeer", "wait", s->port, NULL};
    char output[64], line[160];
    struct net_run r;

    snprintf(output, sizeof(output), "boot%u.answered", s->boots);
    net_host(&s->u, argv, output, &r);
    if (net_ok(&s->u) && !(net_exited0(&r) && has(r.out, "sshpeer: answered")))
        net_fail(&s->u, "sshpeer: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/** Boot the machine again on its copy of the disk, and read nothing. */
static void power_on(struct sealed *s)
{
    s->boots++;
    net_boot(&s->b, &s->u, "sealed", s->boots, NULL);
}

/**
 * Boot the machine and wait until it answers SSH: init's record of starting
 * the server, which the installed system and recovery both print once the
 * network is up, and then the server's own answer.
 */
static void up(struct sealed *s)
{
    power_on(s);
    net_wait(&s->b, SSHD_STARTED);
    answered(s);
}

/** Give the console of the boot that runs, as far as it has been read, or
 * all of it once the machine has ended, from malloc. */
static char *console(struct sealed *s)
{
    return net_file(s->b.console);
}

/** Say whether the console of the boot has `text`. */
static int console_has(struct sealed *s, const char *text)
{
    char *t = console(s);
    int found = has(t, text);

    free(t);
    return found;
}

/** Wait for the machine to end, after `why`, which ended it. */
static void ended(struct sealed *s, const char *why)
{
    struct etude_termination end;
    enum etude_status st;

    if (!net_ok(&s->u))
        return;
    st = etude_wait_end(s->b.m, &end);
    if (st != ETUDE_OK)
        net_fail(&s->u, "the machine did not end after %s", why);
}

/** Run `command`, a `poweroff -f` or a `reboot -f`, as root, and wait for
 * the machine to end. The connection dies with the machine, so what ssh
 * says is not read. */
static void stop_with(struct sealed *s, const char *command)
{
    struct net_run r;

    ask(s, command, "stop", &r);
    net_run_free(&r);
    ended(s, command);
}

/** Power the machine off from inside, after a sync. */
static void stop(struct sealed *s)
{
    stop_with(s, "sync; poweroff -f");
}

/** Reset the machine from inside, after a sync, which ends the boot. */
static void restart(struct sealed *s)
{
    stop_with(s, "sync; reboot -f");
}

/** End the machine with QMP's `quit`, as a power cut would. */
static void cut(struct sealed *s)
{
    char *reply = NULL;

    if (!net_ok(&s->u))
        return;
    if (etude_qmp(s->b.m, "quit", NULL, &reply) != ETUDE_OK)
        net_fail(&s->u, "QMP's quit was not taken");
    free(reply);
    ended(s, "QMP's quit");
}

/** Read or write `n` bytes at byte `at` of the file at `path`; 0, or -1
 * after failing the attempt. */
static int disk_io(struct sealed *s, const char *path, int write, unsigned long long at, void *buf,
                   size_t n)
{
    int fd;
    ssize_t done;

    if (!net_ok(&s->u))
        return -1;
    fd = open(path, write ? O_WRONLY : O_RDONLY);
    if (fd < 0) {
        net_fail(&s->u, "%s cannot be opened", path);
        return -1;
    }
    if (write)
        done = pwrite(fd, buf, n, (off_t)at);
    else
        done = pread(fd, buf, n, (off_t)at);
    if (close(fd) != 0 || done != (ssize_t)n) {
        net_fail(&s->u, "%s: %lu bytes at %llu could not be %s", path, (unsigned long)n, at,
                 write ? "written" : "read");
        return -1;
    }
    return 0;
}

/** Write zeros over `count` sectors of the copy from sector `lba`. */
static void zero_sectors(struct sealed *s, unsigned long long lba, unsigned long count)
{
    unsigned char *zero = calloc(count, 512);

    if (zero == NULL)
        net_fail(&s->u, "no memory for %lu sectors of zeros", count);
    else
        disk_io(s, s->disk, 1, lba * 512, zero, (size_t)count * 512);
    free(zero);
}

/** Give the little-endian number of `n` bytes at `p`. */
static unsigned long long le(const unsigned char *p, int n)
{
    unsigned long long v = 0;

    while (n-- > 0)
        v = v << 8 | p[n];
    return v;
}

/**
 * Read where the partitions are off the copy's own GUID table, so that what
 * the case writes or destroys is placed by the disk as it is and not by the
 * sizes its build was given: the first entry is the boot partition, and the
 * root is the first whose superblock has ext2's magic, 0xEF53 at byte 56,
 * as the kernel finds it.
 */
static void read_table(struct sealed *s)
{
    unsigned char header[512], entry[128], super[1024];
    unsigned long long table, first, last;
    unsigned long count, size, i;
    static const unsigned char unused[16];

    if (disk_io(s, s->disk, 0, 512, header, sizeof(header)) != 0)
        return;
    if (memcmp(header, "EFI PART", 8) != 0) {
        net_fail(&s->u, "the disk has no GUID table");
        return;
    }
    table = le(header + 72, 8);
    count = (unsigned long)le(header + 80, 4);
    size = (unsigned long)le(header + 84, 4);
    if (size < sizeof(entry) || count > 1024) {
        net_fail(&s->u, "the GUID table's entries are %lu of %lu bytes", count, size);
        return;
    }
    for (i = 0; i < count && s->root_lba == 0; i++) {
        if (disk_io(s, s->disk, 0, table * 512 + i * size, entry, sizeof(entry)) != 0)
            return;
        if (memcmp(entry, unused, sizeof(unused)) == 0)
            continue;
        first = le(entry + 32, 8);
        last = le(entry + 40, 8);
        if (s->boot_lba == 0)
            s->boot_lba = first;
        if (disk_io(s, s->disk, 0, first * 512 + 1024, super, sizeof(super)) != 0)
            return;
        if (super[56] == 0x53 && super[57] == 0xEF) {
            s->root_lba = first;
            s->root_size = last - first + 1;
        }
    }
    if (s->root_lba == 0)
        net_fail(&s->u, "no partition of the disk's GUID table has an ext2 root");
}

/** Find `text` in the copy with binfind, and give its first byte's offset,
 * or -1 when the copy has none or binfind failed. */
static long long find_in_disk(struct sealed *s, const char *text, const char *output)
{
    char *argv[] = {"binfind", "--offset", (char *)text, (char *)s->disk, NULL};
    struct net_run r;
    long long at = -1;

    net_host(&s->u, argv, output, &r);
    if (net_exited0(&r) && r.out != NULL && r.out[0] >= '0' && r.out[0] <= '9')
        at = strtoll(r.out, NULL, 10);
    net_run_free(&r);
    return at;
}

/** Copy the file at `from` to `to`, or fail the attempt. */
static void copy_file(struct sealed *s, const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out = in != NULL ? fopen(to, "wb") : NULL;
    char buf[65536];
    size_t n;
    int rc = -1;

    if (out != NULL) {
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0 && fwrite(buf, 1, n, out) == n)
            ;
        rc = ferror(in) == 0 && feof(in) ? 0 : -1;
        if (fclose(out) != 0)
            rc = -1;
    }
    if (in != NULL)
        fclose(in);
    if (rc != 0)
        net_fail(&s->u, "%s could not be copied", from);
}

/**
 * Make the kernel sent to the machine: the kernel on the disk with the text
 * of its first boot record rewritten in place, the smallest edit that makes
 * a boot say which kernel booted it. The mark is as long as what it
 * replaces, so nothing moves, and the structural checker must still take it.
 */
static void mark_kernel(struct sealed *s)
{
    char *replace[] = {"binfind", "--replace", BANNER, MARK, s->marked, NULL};
    char *check[] = {"elfcheck", "--kernel", s->marked, NULL};
    const char *kernel = etude_artifact(s->u.c, "kernel");
    struct net_run r;
    int marked, checked;

    net_path(&s->u, "marked.kernel", s->marked, sizeof(s->marked));
    if (net_ok(&s->u) && kernel == NULL)
        net_fail(&s->u, "the case has no kernel");
    if (net_ok(&s->u))
        copy_file(s, kernel, s->marked);
    net_host(&s->u, replace, "mark", &r);
    marked = net_exited0(&r);
    net_run_free(&r);
    net_host(&s->u, check, "elfcheck", &r);
    checked = net_exited0(&r);
    net_run_free(&r);
    net_assert(&s->u, "marked-kernel", marked && checked,
               marked ? "the marked kernel does not pass the structural checker"
                      : "the kernel has no banner to mark");
}

/** The first boot of the disk as it was built, and what it says of itself. */
static void first_boot(struct sealed *s, long *cand)
{
    struct net_run r;
    char line[160], *text;

    net_step(&s->u, "first-boot");
    read_table(s);
    net_assert(&s->u, "layout", s->boot_lba != 0 && s->root_lba != 0,
               "no partition of the disk's GUID table has an ext2 root");
    mark_kernel(s);
    up(s);
    ask(s, "ps", "ps", &r);
    net_assert(&s->u, "update", has(r.out, "update"), "the installed root runs no update process");
    net_run_free(&r);
    ask(s,
        "mkdir -p /tmp/v && mke2fs -q /root/v.ext2 8M && loopctl attach 0 /root/v.ext2 && "
        "mount -t ext2 /dev/loop0 /tmp/v && echo made > /tmp/v/f && umount /tmp/v && "
        "mount -t ext2 /dev/loop0 /tmp/v && printf GOT= && cat /tmp/v/f && umount /tmp/v && "
        "loopctl detach 0 && rm /root/v.ext2",
        "mke2fs", &r);
    net_assert(&s->u, "mke2fs", has(r.out, "GOT=made"),
               "mke2fs on the installed root made no volume a loop device mounts: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl status", "status", &r);
    net_assert(&s->u, "first-candidate", printed(&r, "booted: candidate"),
               "the first boot did not come from the candidate slot: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    *cand = net_number_after(r.out, "bootctl: candidate: first sector ");
    net_assert(&s->u, "candidate-extent", *cand >= 0,
               "the machine never reported the candidate extent");
    net_assert(&s->u, "first-unarmed", printed(&r, "next_entry: absent"),
               "a freshly built disk has a trial armed: %s",
               line_of(&r, "next_entry:", line, sizeof(line)));
    net_run_free(&r);
    ask(s, "cat /proc/cmdline; cat /proc/mounts", "roots", &r);
    net_assert(&s->u, "first-cmdline", has(r.out, ROOT_CMDLINE),
               "the candidate's command line does not name the ext2 root: %s",
               said(&r, line, sizeof(line)));
    net_assert(&s->u, "first-mounted", has(r.out, "/dev/disk0p3 / ext2 rw,relatime"),
               "the candidate did not mount the ext2 root writable");
    net_run_free(&r);
    text = console(s);
    net_assert(&s->u, "first-unchecked", !has(text, "ext2: check"),
               "a freshly built ext2 root was checked at its first boot");
    net_assert(&s->u, "first-no-fallback", !has(text, FALLING_BACK),
               "the first boot of a good disk took the fallback");
    free(text);
    ask(s, "cat /var/log/services", "services", &r);
    net_assert(&s->u, "first-lease", has(r.out, "lease of"),
               "the machine did not record taking a lease");
    net_run_free(&r);
}

/** A kernel sent down the connection, tried once and committed, and then
 * written as the fallback and tried once through the fallback entry. */
static void update(struct sealed *s)
{
    struct net_run r;
    char line[160];

    net_step(&s->u, "update");
    send_file(s, s->marked, "cat > /tmp/kernel", "kernel.sent", &r);
    net_assert(&s->u, "kernel-sent", net_exited0(&r), "the kernel did not arrive: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    ask(s, "test -s /tmp/kernel && bootctl kernel /tmp/kernel", "install", &r);
    net_assert(&s->u, "kernel-installed", printed(&r, "installed:*candidate slot"),
               "the install did not report success: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl commit", "early-commit", &r);
    net_assert(&s->u, "commit-refused", !net_exited0(&r) && printed(&r, "has not booted"),
               "a commit while the trial was armed was not refused with its reason: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl status", "refused-status", &r);
    net_assert(&s->u, "refused-saved", printed(&r, "saved_entry: fallback"),
               "the refused commit moved the saved default: %s",
               line_of(&r, "saved_entry:", line, sizeof(line)));
    net_assert(&s->u, "refused-armed", printed(&r, "next_entry: candidate"),
               "the refused commit disarmed the trial: %s",
               line_of(&r, "next_entry:", line, sizeof(line)));
    net_run_free(&r);
    restart(s);

    up(s);
    net_assert(&s->u, "trial-marked", console_has(s, MARK),
               "the trial boot did not run the installed kernel");
    ask(s, "bootctl status", "status", &r);
    net_assert(&s->u, "trial-candidate", printed(&r, "booted: candidate"),
               "the trial boot did not come from the candidate slot: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl commit", "commit", &r);
    net_assert(&s->u, "commit", printed(&r, "committed: the candidate is the saved default"),
               "the commit after a trial boot did not report success: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl fallback", "fallback", &r);
    net_assert(&s->u, "fallback-written",
               printed(&r, "fallback: *written from the committed candidate and read back"),
               "the committed candidate was not written as the fallback: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl trial fallback", "trial", &r);
    net_assert(&s->u, "fallback-armed", printed(&r, "armed: the next boot tries fallback"),
               "the fallback trial was not armed: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    restart(s);

    up(s);
    ask(s, "bootctl status", "status", &r);
    net_assert(&s->u, "fallback-booted", printed(&r, "booted: fallback"),
               "the trial boot did not come from the fallback: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    net_run_free(&r);
    net_assert(&s->u, "fallback-marked", console_has(s, MARK),
               "the fallback file does not carry the committed kernel");
    stop(s);
}

/**
 * The candidate slot's first sector destroyed with the machine stopped, at
 * the extent the machine reported. GRUB loads no kernel from it, so its
 * fallback takes the other slot, which boots the same root; the repair is
 * the same install, from the machine that fell back.
 */
static void destroyed(struct sealed *s, long cand)
{
    struct net_run r;
    char line[160];

    net_step(&s->u, "fallback");
    if (cand < 0)
        net_fail(&s->u, "the candidate slot's extent is not known");
    zero_sectors(s, s->boot_lba + (unsigned long long)cand, 1);
    up(s);
    net_assert(&s->u, "destroyed-fallback", console_has(s, FALLING_BACK),
               "a destroyed candidate did not take the fallback");
    ask(s, "bootctl status", "status", &r);
    net_assert(&s->u, "destroyed-booted", printed(&r, "booted: fallback"),
               "the machine did not come up on the fallback: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    net_run_free(&r);
    ask(s, "cat /proc/cmdline; cat /proc/mounts", "roots", &r);
    net_assert(&s->u, "destroyed-cmdline", has(r.out, ROOT_CMDLINE),
               "the fallback's command line does not name the installed root: %s",
               said(&r, line, sizeof(line)));
    net_assert(&s->u, "destroyed-mounted", has(r.out, "/dev/disk0p3 / ext2"),
               "the fallback did not mount the installed root");
    net_run_free(&r);
    send_file(s, s->marked, "cat > /tmp/kernel", "repair.sent", &r);
    net_assert(&s->u, "repair-sent", net_exited0(&r), "the repair kernel did not arrive: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl kernel /tmp/kernel", "install", &r);
    net_assert(&s->u, "repair-installed", printed(&r, "installed:*candidate slot"),
               "the repair install did not report success: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    restart(s);

    up(s);
    ask(s, "bootctl status", "status", &r);
    net_assert(&s->u, "repaired-candidate", printed(&r, "booted: candidate"),
               "the repaired candidate did not boot: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl commit", "commit", &r);
    net_assert(&s->u, "repaired-commit",
               printed(&r, "committed: the candidate is the saved default"),
               "the repaired candidate could not be committed: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    stop(s);
}

/**
 * Boot as the disk stands, make recovery the saved default, arm the
 * candidate for one boot on top of it, and stop: what `rach2-install
 * --update` leaves for the whole of a root write. Report it as `id`.
 */
static void arm_trial(struct sealed *s, const char *id)
{
    struct net_run r;
    char line[160];

    up(s);
    ask(s, "bootctl env saved_entry recovery && bootctl trial candidate && bootctl status", "arm",
        &r);
    net_assert(&s->u, id, printed(&r, "next_entry: candidate"),
               "the candidate trial could not be armed: %s",
               line_of(&r, "next_entry:", line, sizeof(line)));
    net_run_free(&r);
    stop(s);
}

/**
 * Boot once more after a damaged root's trial, and report as `booted_id`
 * that the boot is recovery, which runs from RAM whatever the root's state,
 * and as `spent_id` that the trial was spent.
 */
static void landed(struct sealed *s, const char *booted_id, const char *spent_id,
                   const char *damage)
{
    struct net_run r;
    char line[160];

    up(s);
    ask(s, "bootctl status; cat /proc/mounts", "landed", &r);
    net_assert(&s->u, booted_id, printed(&r, "booted: recovery"),
               "the boot after a %s root was not recovery: %s", damage,
               line_of(&r, "booted:", line, sizeof(line)));
    net_assert(&s->u, spent_id,
               printed(&r, "next_entry: absent") || printed(&r, "next_entry: empty"),
               "the trial was still armed after a %s root: %s", damage,
               line_of(&r, "next_entry:", line, sizeof(line)));
    net_run_free(&r);
    stop(s);
}

/** Run e2fsprogs' `tool` with `args` on the ext2 root of the copy, at its
 * offset in the disk, keeping its output as `output`; give its run in `r`. */
static void e2fs(struct sealed *s, const char *tool, const char *const args[], const char *output,
                 struct net_run *r)
{
    char *argv[8], at[4200];
    int n = 0;

    snprintf(at, sizeof(at), "%s?offset=%llu", s->disk, s->root_lba * 512);
    argv[n++] = (char *)tool;
    while (*args != NULL && n < 6)
        argv[n++] = (char *)*args++;
    argv[n++] = at;
    argv[n] = NULL;
    net_host(&s->u, argv, output, r);
}

/** Run debugfs's request `request` on the root of the copy, writing; a
 * failure fails the attempt. */
static void debugfs(struct sealed *s, const char *request, const char *output)
{
    const char *const args[] = {"-w", "-R", request, NULL};
    struct net_run r;
    char line[160];

    e2fs(s, "debugfs", args, output, &r);
    if (net_ok(&s->u) && !net_exited0(&r))
        net_fail(&s->u, "debugfs %s: %s", request, said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/**
 * The root made missing under an armed trial: both kernel entries boot the
 * same root, so GRUB cannot save the machine, and the trial exists for this.
 * The armed boot must stop with no root and reach no login, and the boot
 * after it must be recovery with nothing armed. The superblock is put back
 * afterwards.
 */
static void missing(struct sealed *s)
{
    unsigned char super[1024];
    unsigned long long at = (s->root_lba + 2) * 512;

    net_step(&s->u, "missing");
    arm_trial(s, "arm-missing");
    disk_io(s, s->disk, 0, at, super, sizeof(super));
    zero_sectors(s, s->root_lba + 2, 2);
    power_on(s);
    net_wait(&s->b, ROOT_MISSING);
    net_assert(&s->u, "missing-refused", 1, "");
    cut(s);
    net_assert(&s->u, "missing-no-login", !console_has(s, "rach2 login:"),
               "a candidate whose ext2 root is missing still reached a login");
    landed(s, "missing-recovery", "missing-spent", "missing");
    disk_io(s, s->disk, 1, at, super, sizeof(super));
}

/** The root made faulty under an armed trial: a reached inode's bit in the
 * bitmap cleared and the valid bit clear, so the check runs, finds the
 * fault and mounts the root read-only; nothing commits it. */
static void faulty(struct sealed *s)
{
    char *text;

    net_step(&s->u, "faulty");
    arm_trial(s, "arm-faulty");
    debugfs(s, "freei /etc/hostname", "freei");
    debugfs(s, "ssv state 0", "faulty.state");
    power_on(s);
    net_wait(&s->b, "ext2: mounted read-only: the check found a fault");
    net_assert(&s->u, "faulty-readonly", 1, "");
    cut(s);
    text = console(s);
    net_assert(&s->u, "faulty-named",
               net_find(text, "ext2: check: inode #: a reached inode the bitmap calls free") !=
                   NULL,
               "the faulty root's fault was not named");
    free(text);
    landed(s, "faulty-recovery", "faulty-spent", "faulty");
}

/** The root made dirty under an armed trial: the fault repaired by e2fsck
 * on the host and the valid bit alone clear, so the check passes and the
 * root mounts writable; still nothing commits it. */
static void dirty(struct sealed *s)
{
    const char *const args[] = {"-fp", NULL};
    struct net_run r;
    char line[160];

    net_step(&s->u, "dirty");
    arm_trial(s, "arm-dirty");
    e2fs(s, "e2fsck", args, "e2fsck", &r);
    net_assert(&s->u, "host-repair",
               r.end.end == ETUDE_END_EXITED && r.end.value >= 0 && r.end.value <= 2,
               "e2fsck -fp on the host exited %d: %s", r.end.value, said(&r, line, sizeof(line)));
    net_run_free(&r);
    debugfs(s, "ssv state 0", "dirty.state");
    power_on(s);
    net_wait(&s->b, "rach2 login:");
    net_assert(&s->u, "dirty-login", 1, "");
    cut(s);
    net_assert(&s->u, "dirty-clean", console_has(s, "ext2: check: clean"),
               "the dirty root was not checked clean");
    landed(s, "dirty-recovery", "dirty-spent", "dirty");
}

/**
 * The repair and the return: recovery armed, the check run there by hand,
 * and the candidate, tried once more, up clean and committed, which is the
 * state the case goes on in. No boot entry runs the check: e2fsck is a
 * program a person in recovery runs. Its status is read whole, `FSCK=1.`,
 * since `FSCK=1` is a prefix of `FSCK=16`, e2fsck's usage error.
 */
static void repair(struct sealed *s)
{
    struct net_run r;
    char line[160], *text;

    net_step(&s->u, "repair");
    up(s);
    ask(s, "bootctl trial recovery && bootctl status", "arm", &r);
    net_assert(&s->u, "recovery-armed", printed(&r, "next_entry: recovery"),
               "recovery could not be armed for the repair: %s",
               line_of(&r, "next_entry:", line, sizeof(line)));
    net_run_free(&r);
    stop(s);

    up(s);
    ask(s, "e2fsck -y /dev/disk0p3; echo FSCK\"\"=$?.", "e2fsck", &r);
    net_assert(&s->u, "recovery-e2fsck", has(r.out, "FSCK=0.") || has(r.out, "FSCK=1."),
               "the check of the ext2 root did not repair it: %s",
               line_of(&r, "FSCK=", line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl trial candidate && bootctl status", "arm", &r);
    net_assert(&s->u, "candidate-rearmed", printed(&r, "next_entry: candidate"),
               "the candidate could not be armed after the repair: %s",
               line_of(&r, "next_entry:", line, sizeof(line)));
    net_run_free(&r);
    stop(s);

    up(s);
    text = console(s);
    net_assert(&s->u, "repaired-unchecked", !has(text, "ext2: check"),
               "the repaired ext2 root was checked again");
    free(text);
    ask(s, "bootctl status; cat /proc/mounts", "status", &r);
    net_assert(&s->u, "repaired-root-candidate", printed(&r, "booted: candidate"),
               "the repaired candidate did not boot: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    net_assert(&s->u, "repaired-root-writable", has(r.out, "/dev/disk0p3 / ext2 rw,relatime"),
               "the repaired candidate did not mount the ext2 root writable");
    net_run_free(&r);
    ask(s, "bootctl commit", "commit", &r);
    net_assert(&s->u, "repaired-root-commit",
               printed(&r, "committed: the candidate is the saved default"),
               "the repaired candidate could not be committed: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    stop(s);
}

/** A menu staged and shown, and then truncated mid-entry on the host: the
 * file keeps its size, and the stub must survive a menu it cannot parse. */
static void menu(struct sealed *s)
{
    unsigned char torn[MENU_BYTES];
    struct net_run r;
    char line[160], *text;
    long long at;
    size_t n;

    net_step(&s->u, "menu");
    up(s);
    ask(s,
        "printf 'menuentry sealedone --id sealedone {\\nmultiboot /boot/fallback "
        "rach2.root=disk rach2.menu=sealedone\\nboot\\n}\\n' > /tmp/m.cfg && "
        "bootctl config /tmp/m.cfg",
        "config", &r);
    net_assert(&s->u, "menu-staged", printed(&r, "staged:*menu in a"), "the menu did not stage: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    restart(s);
    up(s);
    net_assert(&s->u, "menu-shown", console_has(s, "sealedone"),
               "the staged entry never reached the menu");
    stop(s);

    at = find_in_disk(s, MENU_ENTRY, "menu.offset");
    net_assert(&s->u, "menu-on-disk", at >= 0, "the staged menu is not on the disk");
    if (at >= 0) {
        n = (size_t)snprintf((char *)torn, sizeof(torn), "%s {\n", MENU_ENTRY);
        memset(torn + n, '#', sizeof(torn) - n);
        disk_io(s, s->disk, 1, (unsigned long long)at, torn, sizeof(torn));
    }
    up(s);
    text = console(s);
    net_assert(&s->u, "torn-error", has_any_case(text, "error"),
               "a truncated menu produced no loader error");
    free(text);
    ask(s, "bootctl status", "status", &r);
    net_assert(&s->u, "torn-candidate", printed(&r, "booted: candidate"),
               "a truncated menu stopped the machine booting its candidate: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    net_run_free(&r);
    stop(s);
}

/**
 * The boot loader replaced: the core is in the BIOS boot partition, which
 * the kernel publishes as /dev/grubcore on this GUID table, and `bootctl
 * grub` writes GRUB's pinned GPT core into the tail not in use and selects
 * it. With the old tail zeroed, the machine comes back only through the
 * new core.
 */
static void core(struct sealed *s)
{
    const char *image = etude_artifact(s->u.c, "gpt-core");
    struct net_run r;
    char line[160];

    net_step(&s->u, "core");
    if (net_ok(&s->u) && image == NULL)
        net_fail(&s->u, "the case has no GPT core");
    up(s);
    net_assert(
        &s->u, "grubcore-published",
        console_has(s, "devfs: /dev/grubcore: BIOS boot partition 4, 2014 sectors at LBA 34"),
        "the kernel did not publish the BIOS boot partition as /dev/grubcore");
    send_file(s, image, "cat > /tmp/core.img", "core.sent", &r);
    net_assert(&s->u, "core-sent", net_exited0(&r), "the core image did not arrive: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    ask(s, "bootctl grub /tmp/core.img", "grub", &r);
    net_assert(&s->u, "core-installed",
               printed(&r, "in the tail at LBA 1041; the entry sector at LBA 34 now points there"),
               "the boot loader install in the BIOS boot partition: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    stop(s);
    zero_sectors(s, OLD_CORE_LBA, OLD_CORE_SECTORS);
    up(s);
    ask(s, "bootctl status", "status", &r);
    net_assert(&s->u, "core-booted", printed(&r, "booted: candidate"),
               "the machine did not come back on its candidate through the new core: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    net_run_free(&r);
    stop(s);
}

/**
 * Set next_entry to `entry` in GRUB's environment block on the copy at
 * byte `at`, as grub-editenv arms it: that key set and every other line
 * kept, so that the saved default stays.
 */
static void arm_next(struct sealed *s, long long at, const char *entry)
{
    char block[ENV_BYTES + 1], out[ENV_BYTES + 1], *line, *next;
    size_t n, len;

    if (at < 0 || disk_io(s, s->disk, 0, (unsigned long long)at, block, ENV_BYTES) != 0)
        return;
    block[ENV_BYTES] = '\0';
    n = (size_t)snprintf(out, sizeof(out),
                         "%s\n# WARNING: Do not edit this file by tools other than "
                         "grub-editenv!!!\n",
                         ENV_HEADER);
    for (line = block; line != NULL && *line != '\0'; line = next) {
        next = strchr(line, '\n');
        len = next != NULL ? (size_t)(next - line) : strlen(line);
        if (next != NULL)
            next++;
        while (len > 0 && line[len - 1] == '#')
            len--;
        if (len == 0 || line[0] == '#' || strncmp(line, "next_entry=", 11) == 0)
            continue;
        if (n + len + 1 >= sizeof(out))
            break;
        memcpy(out + n, line, len);
        n += len;
        out[n++] = '\n';
    }
    n += (size_t)snprintf(out + n, sizeof(out) - n, "next_entry=%s\n", entry);
    if (n >= ENV_BYTES) {
        net_fail(&s->u, "the environment block has no room for next_entry");
        return;
    }
    memset(out + n, '#', ENV_BYTES - n);
    disk_io(s, s->disk, 1, (unsigned long long)at, out, ENV_BYTES);
}

/** Say whether the copy's ext2 root has the bytes the disk was built with. */
static int root_as_built(struct sealed *s)
{
    static unsigned char a[1 << 20], b[1 << 20];
    unsigned long long at = s->root_lba * 512, left = s->root_size * 512;
    size_t n;

    while (left > 0 && net_ok(&s->u)) {
        n = left < sizeof(a) ? (size_t)left : sizeof(a);
        if (disk_io(s, s->artifact, 0, at, a, n) != 0 || disk_io(s, s->disk, 0, at, b, n) != 0)
            return 0;
        if (memcmp(a, b, n) != 0)
            return 0;
        at += n;
        left -= n;
    }
    return net_ok(&s->u);
}

/**
 * The root's superblock destroyed with the machine stopped, the damage no
 * kernel entry survives. It is shown first: the fallback entry, armed for
 * one boot, must stop with no root. Recovery, armed next, rewrites the root
 * from the disk as built through one ssh pipe, and the copy must then have
 * those bytes; they are compared on the host, since hashing a gigabyte on
 * an emulated processor takes longer than the answer is worth.
 */
static void rewrite(struct sealed *s)
{
    char command[128], wrote[64], bs[24], skip[24], count[24], bytes[24], line[160];
    unsigned long long block;
    struct net_run r;
    long long env;
    char *argv[] = {"sh",
                    "-c",
                    "f=$1 bs=$2 skip=$3 count=$4; shift 4; "
                    "dd if=\"$f\" bs=\"$bs\" skip=\"$skip\" count=\"$count\" 2>/dev/null | "
                    "gzip -c | \"$@\"",
                    "sh",
                    (char *)s->artifact,
                    bs,
                    skip,
                    count,
                    NET_SSH,
                    NET_SSH_KEY(s->key),
                    "-T",
                    "-p",
                    s->port,
                    "root@127.0.0.1",
                    command,
                    NULL};

    net_step(&s->u, "rewrite");
    zero_sectors(s, s->root_lba + 2, 2);
    env = find_in_disk(s, ENV_HEADER, "env.offset");
    net_assert(&s->u, "env-block", env >= 0, "the disk has no environment block");
    arm_next(s, env, "fallback");
    power_on(s);
    net_wait(&s->b, ROOT_MISSING);
    net_assert(&s->u, "broken-refused", 1, "");
    cut(s);
    net_assert(&s->u, "broken-no-login", !console_has(s, "rach2 login:"),
               "a machine whose root is destroyed still reached a login");
    arm_next(s, env, "recovery");

    up(s);
    net_assert(&s->u, "recovery-image", console_has(s, RECOVERY_RAM),
               "the armed entry did not load the recovery image");
    ask(s, "test -b /dev/disk0 && echo RECOVERY\"\"HERE", "disk", &r);
    net_assert(&s->u, "recovery-disk", has(r.out, "RECOVERYHERE"),
               "the recovery entry did not come up with the whole-disk device: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    block = s->root_lba % 2048 == 0 && s->root_size % 2048 == 0 ? 2048 : 1;
    snprintf(bs, sizeof(bs), "%llu", block * 512);
    snprintf(skip, sizeof(skip), "%llu", s->root_lba / block);
    snprintf(count, sizeof(count), "%llu", s->root_size / block);
    snprintf(bytes, sizeof(bytes), "%llu", s->root_size * 512);
    snprintf(command, sizeof(command), "gunzip | reflash /dev/disk0 %llu %s", s->root_lba * 512,
             bytes);
    net_host(&s->u, argv, "reflash", &r);
    snprintf(wrote, sizeof(wrote), "wrote %s bytes", bytes);
    net_assert(&s->u, "rewrite-written", printed(&r, wrote),
               "the repair did not report the whole root written: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    ask(s, "sync", "sync", &r);
    net_run_free(&r);
    stop(s);
    net_assert(&s->u, "rewrite-same", root_as_built(s),
               "the root written does not have the bytes of the root sent");
}

/** The last cold boot, on the root that arrived through the pipe. */
static void last_boot(struct sealed *s)
{
    struct net_run r;
    char line[160];

    net_step(&s->u, "last-boot");
    up(s);
    net_assert(&s->u, "final-no-fallback", !console_has(s, FALLING_BACK),
               "the machine ended the case taking a fallback");
    ask(s, "bootctl status", "status", &r);
    net_assert(&s->u, "final-candidate", printed(&r, "booted: candidate"),
               "the repaired machine did not boot its candidate: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    net_run_free(&r);
    ask(s, "cat /var/log/services", "services", &r);
    net_assert(&s->u, "final-lease", has(r.out, "lease of"),
               "the repaired machine did not record taking a lease");
    net_run_free(&r);
    stop(s);
}

/** Begin an attempt of either case: the port, the key and the disks. */
static void begin(struct sealed *s, struct etude_case *c)
{
    unsigned port;

    memset(s, 0, sizeof(*s));
    net_case(&s->u, c);
    port = etude_port(c, 0);
    snprintf(s->port, sizeof(s->port), "%u", port);
    if (port == 0)
        net_fail(&s->u, "the case has no port");
    net_key(&s->u, "demo-key", s->key, sizeof(s->key));
    if (net_ok(&s->u) && ((s->artifact = etude_artifact(c, "sealed")) == NULL ||
                          (s->disk = etude_private_copy(c, "sealed")) == NULL))
        net_fail(&s->u, "the sealed disk has no private copy");
}

/** The case machine.sealed.fault. */
static int fault(struct etude_case *c)
{
    struct sealed s;
    long cand = -1;

    begin(&s, c);
    first_boot(&s, &cand);
    update(&s);
    destroyed(&s, cand);
    missing(&s);
    faulty(&s);
    dirty(&s);
    repair(&s);
    menu(&s);
    core(&s);
    rewrite(&s);
    last_boot(&s);
    return net_end(&s.u);
}

/**
 * The case machine.sealed.control: the ordinary work with nothing injected,
 * and no fallback and no recovery on the whole console once the machine has
 * stopped. The menu draws every entry's title, recovery's among them, so a
 * boot of recovery is told by the RAM image it loads.
 */
static int control(struct etude_case *c)
{
    struct sealed s;
    struct net_run r;
    char line[160], *text;
    int candidate, unarmed;

    begin(&s, c);
    net_step(&s.u, "boot");
    up(&s);
    ask(&s, "bootctl status", "status", &r);
    candidate = printed(&r, "booted: candidate");
    unarmed = printed(&r, "next_entry: absent");
    net_assert(&s.u, "control-candidate", candidate,
               "the control run did not boot its candidate: %s",
               line_of(&r, "booted:", line, sizeof(line)));
    net_run_free(&r);
    ask(&s, "cat /var/log/services", "services", &r);
    net_assert(&s.u, "control-lease", has(r.out, "lease of"), "the control run recorded no lease");
    net_run_free(&r);
    net_assert(&s.u, "control-unarmed", unarmed, "the control run left a trial armed");
    stop(&s);
    text = console(&s);
    net_assert(&s.u, "control-no-fallback", !has(text, FALLING_BACK),
               "the control run took a fallback with nothing injected");
    net_assert(&s.u, "control-no-recovery", !has(text, RECOVERY_RAM),
               "the control run reached recovery with nothing injected");
    free(text);
    return net_end(&s.u);
}

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c);

    if (strcmp(id, "machine.sealed.fault") == 0)
        return fault(c);
    if (strcmp(id, "machine.sealed.control") == 0)
        return control(c);
    fprintf(stderr, "sealed_scenario: no case %s\n", id);
    return 1;
}
