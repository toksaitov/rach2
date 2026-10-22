/*
 * uefi_scenario.c: the scenario of tests/cases/machine.uefi.json, the
 * machine booted by UEFI firmware, OVMF, and the paths beside it that
 * SeaBIOS boots.
 *
 * OVMF starts GRUB's EFI image from the disk's EFI system partition, from
 * the stick's EFI boot image or from the CD's second El Torito entry, and
 * the image opens the menu the BIOS core opens. Each case boots private
 * copies of its disks, reaches the machine over SSH with the demonstration
 * key once init has started the server and the server answers, and changes
 * the copies from the host between boots where the old suite did:
 *
 * installed  the GPT disk as the disk builder makes one: the root by the
 *            disk's GUID, the framebuffer's console read off a screen dump,
 *            and Doom's picture on the framebuffer and gone again.
 * recovery   the same disk with next_entry=recovery in GRUB's environment
 *            block, as grub-editenv arms it.
 * nomenu     the same disk with the menu deleted from its EFI system
 *            partition, so that only the recovery partition's menu boots.
 * stick, cd  the install stick on USB and the recovery ISO as a CD.
 * core       the disk booted by SeaBIOS, whose BIOS core bootctl grub
 *            replaces twice, each new tail shown to be the one that runs by
 *            zeroing the other on the host; then OVMF boots it again.
 * wide       an install onto a sparse 4 TiB NVMe disk whose root grows past
 *            2 TiB, then booted alone and with the kernel given to QEMU.
 *            wide-bios is the same by SeaBIOS for i686, since OVMF gives the
 *            controller's 64-bit BAR an address the i686 kernel does not
 *            reach.
 * damaged    a GPT disk whose primary header does not check, which recovery
 *            writes again from the backup; SeaBIOS then boots it, and its
 *            kernel reads the primary. GRUB's BIOS core boots such a disk
 *            from its backup as well, so only the header's CRC-32 and the
 *            kernel's record show the repair.
 *
 * QEMU runs with -no-reboot, and every boot is a QEMU of its own on the
 * copies, which keep what the boots before it wrote.
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

/* What the console says when the boot is ready for a login. */
#define LOGIN "rach2 login:"

/* The firmware's record of the boot option it starts, on the serial line,
 * which a pattern of net_find follows with the option's device. */
#define BDS     "BdsDxe"
#define STARTED BDS ": starting Boot"

/* The header of GRUB's environment block, and the block's bytes. */
#define ENV_HEADER "# GRUB Environment Block"
#define ENV_BYTES  1024

/* The type GUID of a BIOS boot partition, 21686148-6449-6E6F-744E-
 * 656564454649, as its 16 bytes lie in an entry: "Hah!IdontNeedEFI". */
static const unsigned char bios_boot_type[16] = {0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e,
                                                 0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49};

/* The most colors a screen of the console has, and the fewest of a picture
 * of the game's. */
#define CONSOLE_COLORS 16
#define PICTURE_COLORS 64

/* The machines a case boots, at most, for the count of each one's boots. */
#define MACHINES 3

/* One attempt of any case. */
struct uefi {
    struct net_case u;
    struct net_boot b;
    const char *names[MACHINES]; /* the machines booted so far */
    unsigned boots[MACHINES];    /* and the boots of each */
    const char *machine;         /* the machine of the boot that runs */
    unsigned boot;               /* and its boot, counted from 1 */
    char port[16];
    char key[4096];   /* the private copy of the demo key */
    const char *disk; /* the private copy of the case's GPT disk */
};

/* Where a GPT disk has what the cases read or write, from its own table. */
struct table {
    unsigned long long esp;        /* partition 1's first byte */
    unsigned long long bios_first; /* the BIOS boot partition's first LBA */
    unsigned long long bios_size;  /* and its sectors */
    unsigned long long sectors;    /* the disk's sectors */
};

/** Say whether `text` has `part`; no text has nothing. */
static int has(const char *text, const char *part)
{
    return text != NULL && strstr(text, part) != NULL;
}

/** Give the first line of `r`'s standard error, or of its output when that
 * is empty, in `buf` of `size` bytes, to show in a failure. */
static const char *said(const struct net_run *r, char *buf, size_t size)
{
    const char *text = r->err != NULL && r->err[0] != '\0' ? r->err : r->out;

    return net_copy_line(text != NULL ? text + strspn(text, "\r\n") : NULL, buf, size);
}

/** Say whether a line of `text` begins with `start`. */
static int line_begins(const char *text, const char *start)
{
    const char *p;
    size_t n = strlen(start);

    for (p = text; p != NULL && *p != '\0'; p = strchr(p, '\n'), p = p != NULL ? p + 1 : NULL)
        if (strncmp(p, start, n) == 0)
            return 1;
    return 0;
}

/**
 * Run `command` on the machine as `user` by the demo key, keeping what it
 * prints as evidence `output`. ssh reads no file of the account that runs
 * it, offers that key alone, and keeps no record of the host's key.
 */
static enum etude_status as_user(struct uefi *s, const char *user, const char *command,
                                 const char *output, struct net_run *r)
{
    char login[64];
    char *argv[] = {NET_SSH, NET_SSH_KEY(s->key), "-T", "-p", s->port,
                    login,   (char *)command,     NULL};

    snprintf(login, sizeof(login), "%s@127.0.0.1", user);
    return net_host(&s->u, argv, output, r);
}

/** Run `command` as root with the file at `path` as its standard input,
 * as a file is sent down the connection, keeping its output as `output`. */
static enum etude_status send_file(struct uefi *s, const char *path, const char *command,
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

/** Give the count of the boots of `machine` after one more. */
static unsigned next_boot(struct uefi *s, const char *machine)
{
    unsigned i;

    for (i = 0; i < MACHINES && s->names[i] != NULL; i++)
        if (strcmp(s->names[i], machine) == 0)
            return ++s->boots[i];
    if (i == MACHINES)
        return 0;
    s->names[i] = machine;
    return s->boots[i] = 1;
}

/** Boot `machine` on the case's copies, and read nothing. */
static void power_on(struct uefi *s, const char *machine)
{
    s->machine = machine;
    s->boot = next_boot(s, machine);
    if (s->boot == 0)
        net_fail(&s->u, "the case boots more than %d machines", MACHINES);
    else
        net_boot(&s->b, &s->u, machine, s->boot, NULL);
}

/** Wait until the server answers on the case's port, as sshpeer judges it:
 * a connection that has the server's identification. */
static void answered(struct uefi *s)
{
    char *argv[] = {"sshpeer", "wait", s->port, NULL};
    char output[64], line[160];
    struct net_run r;

    snprintf(output, sizeof(output), "%s.%u.answered", s->machine, s->boot);
    net_host(&s->u, argv, output, &r);
    if (net_ok(&s->u) && !(net_exited0(&r) && has(r.out, "sshpeer: answered")))
        net_fail(&s->u, "sshpeer: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/**
 * Boot `machine` and wait until it answers SSH: init's record of starting
 * the server, which the installed system and recovery both print once the
 * network is up, and then the server's own answer.
 */
static void up(struct uefi *s, const char *machine)
{
    power_on(s, machine);
    net_wait(&s->b, SSHD_STARTED);
    answered(s);
}

/** Say whether the console of the boot that runs, as far as it has been
 * read, has a line with `pattern`, as net_find matches it. */
static int console_has(struct uefi *s, const char *pattern)
{
    char *text = net_file(s->b.console);
    int found = net_find(text, pattern) != NULL;

    free(text);
    return found;
}

/** End the machine with QMP's `quit` and wait for it to end. */
static void quit(struct uefi *s)
{
    struct etude_termination end;
    char *reply = NULL;

    if (!net_ok(&s->u))
        return;
    if (etude_qmp(s->b.m, "quit", NULL, &reply) != ETUDE_OK)
        net_fail(&s->u, "QMP's quit was not taken");
    free(reply);
    if (net_ok(&s->u) && etude_wait_end(s->b.m, &end) != ETUDE_OK)
        net_fail(&s->u, "the machine did not end after QMP's quit");
}

/** Read or write `n` bytes at byte `at` of the file at `path`; 0, or -1
 * after failing the attempt. */
static int disk_io(struct uefi *s, const char *path, int write, unsigned long long at, void *buf,
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

/** Give the little-endian number of `n` bytes at `p`. */
static unsigned long long le(const unsigned char *p, int n)
{
    unsigned long long v = 0;

    while (n-- > 0)
        v = v << 8 | p[n];
    return v;
}

/**
 * Read where the copy has its EFI system partition, the first entry, and
 * its BIOS boot partition, off its own GUID table, so that what the case
 * writes is placed by the disk as it is and not by the sizes its build was
 * given.
 */
static void read_table(struct uefi *s, struct table *t)
{
    unsigned char header[512], entry[128];
    unsigned long long array;
    unsigned long count, size, i;
    off_t end;
    int fd;

    memset(t, 0, sizeof(*t));
    if (disk_io(s, s->disk, 0, 512, header, sizeof(header)) != 0)
        return;
    if (memcmp(header, "EFI PART", 8) != 0) {
        net_fail(&s->u, "the disk has no GUID table");
        return;
    }
    array = le(header + 72, 8);
    count = (unsigned long)le(header + 80, 4);
    size = (unsigned long)le(header + 84, 4);
    if (size < sizeof(entry) || count > 1024) {
        net_fail(&s->u, "the GUID table's entries are %lu of %lu bytes", count, size);
        return;
    }
    for (i = 0; i < count; i++) {
        if (disk_io(s, s->disk, 0, array * 512 + i * size, entry, sizeof(entry)) != 0)
            return;
        if (i == 0)
            t->esp = le(entry + 32, 8) * 512;
        if (memcmp(entry, bios_boot_type, sizeof(bios_boot_type)) == 0) {
            t->bios_first = le(entry + 32, 8);
            t->bios_size = le(entry + 40, 8) - t->bios_first + 1;
        }
    }
    fd = open(s->disk, O_RDONLY);
    end = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (fd >= 0)
        close(fd);
    if (end <= 0)
        net_fail(&s->u, "the copy's size cannot be read");
    else
        t->sectors = (unsigned long long)end / 512;
}

/**
 * Write next_entry=`entry` into the environment block at byte `at` of the
 * copy as grub-editenv writes it: the header and its warning, every other
 * line kept, and `#` to the block's end.
 */
static void arm_next(struct uefi *s, long long at, const char *entry)
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

/**
 * Dump the machine's screen through QMP into evidence `name`, and run
 * screentext on the dump, with `--colors` when `colors`, keeping what it
 * prints as evidence `name`.txt and giving its run in `r`.
 */
static void screen(struct uefi *s, const char *name, int colors, struct net_run *r)
{
    char path[4096], args[4200], output[64];
    char *text[] = {"screentext", path, NULL};
    char *count[] = {"screentext", "--colors", path, NULL};
    char *reply = NULL;

    memset(r, 0, sizeof(*r));
    if (net_path(&s->u, name, path, sizeof(path)) == NULL)
        return;
    snprintf(args, sizeof(args), "{\"filename\": \"%s\"}", path);
    if (etude_qmp(s->b.m, "screendump", args, &reply) != ETUDE_OK)
        net_fail(&s->u, "QMP's screendump was not taken: %s", reply != NULL ? reply : "");
    free(reply);
    snprintf(output, sizeof(output), "%s.txt", name);
    net_host(&s->u, colors ? count : text, output, r);
}

/** Give the colors screentext counted in `r`, or -1 when it did not. */
static long colors_of(const struct net_run *r)
{
    return net_exited0(r) && r->out != NULL && r->out[0] >= '0' && r->out[0] <= '9'
               ? strtol(r->out, NULL, 10)
               : -1;
}

/** Begin an attempt: the port, the key and the copy of the GPT disk when
 * the case has one. */
static void begin(struct uefi *s, struct etude_case *c, int disk)
{
    unsigned port;

    memset(s, 0, sizeof(*s));
    net_case(&s->u, c);
    port = etude_port(c, 0);
    snprintf(s->port, sizeof(s->port), "%u", port);
    if (port == 0)
        net_fail(&s->u, "the case has no port");
    net_key(&s->u, "demo-key", s->key, sizeof(s->key));
    if (disk && net_ok(&s->u) && (s->disk = etude_private_copy(c, "uefi-disk")) == NULL)
        net_fail(&s->u, "the disk has no private copy");
}

/** The case machine.uefi.installed. */
static int installed(struct etude_case *c)
{
    struct uefi s;
    char *args[] = {NET_SSH,
                    NET_SSH_KEY(s.key),
                    "-T",
                    "-p",
                    s.port,
                    "root@127.0.0.1",
                    "cd /root && exec doom 2>&1 < /dev/null",
                    NULL};
    struct etude_case_service *game = NULL;
    struct etude_termination end;
    struct net_run r;
    char line[160];
    long n;

    begin(&s, c, 1);
    net_step(&s.u, "boot");
    up(&s, "uefi");
    net_assert(&s.u, "booted-disk", console_has(&s, STARTED "*HARDDISK"),
               "the firmware did not say it started the disk's boot option");
    net_assert(&s.u, "root-by-guid", console_has(&s, "root: GPTID=*,3: /dev/disk0p3, ext2"),
               "the kernel did not name its root by GPTID");
    net_assert(&s.u, "write-combining",
               console_has(&s, "cpu: page attributes: 0x0007040600070106; the framebuffer is "
                               "mapped write-combining"),
               "the kernel did not make the framebuffer write-combining");
    as_user(&s, "root",
            "id -u; bootctl status; test -f /demo/doom1.wad && echo WAD\"\"HERE;"
            " echo UEFI\"\"LOGIN",
            "login", &r);
    net_assert(&s.u, "login", has(r.out, "UEFILOGIN") && line_begins(r.out, "0\n"),
               "root's SSH login to the disk booted by UEFI: %s", said(&r, line, sizeof(line)));
    net_assert(&s.u, "candidate", has(r.out, "bootctl: booted: candidate"),
               "bootctl on the disk booted by UEFI did not name the candidate: %s",
               said(&r, line, sizeof(line)));
    net_assert(&s.u, "wad", has(r.out, "WADHERE"),
               "the disk has no /demo/doom1.wad, which its root stages from ports/doom/doom1.wad"
               " (sh ports/doom/fetch_wad.sh)");
    net_run_free(&r);

    net_step(&s.u, "screen");
    screen(&s, "screen.ppm", 0, &r);
    net_assert(&s.u, "screen-login", net_exited0(&r) && line_begins(r.out, LOGIN),
               "no line of the framebuffer console begins %s: %s", LOGIN,
               said(&r, line, sizeof(line)));
    net_run_free(&r);

    net_step(&s.u, "doom");
    /* The service's program is ssh, and it takes the words after it. */
    if (net_ok(&s.u) && etude_service(c, "doom", args + 1, &game, &end) != ETUDE_OK)
        net_fail(&s.u, "the game reported no frame drawn");
    net_assert(&s.u, "doom-drew", 1, "");
    screen(&s, "doom.ppm", 1, &r);
    n = colors_of(&r);
    net_assert(&s.u, "doom-picture", n > PICTURE_COLORS,
               "the screen had %ld colors while the game ran, not more than %d", n, PICTURE_COLORS);
    net_run_free(&r);
    if (game != NULL && etude_service_stop(game, &end) != ETUDE_OK)
        net_fail(&s.u, "the game's ssh did not stop");
    as_user(
        &s, "root",
        "killall doom; while pidof doom > /dev/null; do :; done; pidof doom || echo DOOM\"\"GONE",
        "gone", &r);
    net_assert(&s.u, "doom-gone", has(r.out, "DOOMGONE"), "the game still ran after it was killed");
    net_run_free(&r);
    screen(&s, "after.ppm", 1, &r);
    n = colors_of(&r);
    net_assert(&s.u, "console-back", n >= 0 && n <= CONSOLE_COLORS,
               "the screen had %ld colors once the game had gone, more than the console's %d", n,
               CONSOLE_COLORS);
    net_run_free(&r);
    quit(&s);
    return net_end(&s.u);
}

/** Find `text` in the copy with binfind, and give its first byte's offset,
 * or -1 when the copy has none or binfind failed. */
static long long find_in_disk(struct uefi *s, const char *text)
{
    char *argv[] = {"binfind", "--offset", (char *)text, (char *)s->disk, NULL};
    struct net_run r;
    long long at = -1;

    net_host(&s->u, argv, "env.offset", &r);
    if (net_exited0(&r) && r.out != NULL && r.out[0] >= '0' && r.out[0] <= '9')
        at = strtoll(r.out, NULL, 10);
    net_run_free(&r);
    return at;
}

/** The case machine.uefi.recovery. */
static int recovery(struct etude_case *c)
{
    struct uefi s;
    struct net_run r;
    char line[160];
    long long env;

    begin(&s, c, 1);
    net_step(&s.u, "arm");
    env = find_in_disk(&s, ENV_HEADER);
    if (env >= 0)
        arm_next(&s, env, "recovery");
    net_assert(&s.u, "env-block", env >= 0, "the disk has no environment block");

    net_step(&s.u, "boot");
    up(&s, "uefi");
    as_user(&s, "root",
            "grep -o rach2.slot=recovery /proc/cmdline; bootctl --target /dev/disk0p1 status;"
            " echo RECOVERY\"\"LOGIN",
            "recovery", &r);
    net_assert(&s.u, "recovery-up",
               has(r.out, "rach2.slot=recovery") && has(r.out, "RECOVERYLOGIN"),
               "the recovery system of the disk booted by UEFI: %s", said(&r, line, sizeof(line)));
    net_assert(&s.u, "trial-consumed", has(r.out, "bootctl: next_entry: empty"),
               "the recovery entry's trial was not consumed: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    quit(&s);
    return net_end(&s.u);
}

/** The case machine.uefi.nomenu. */
static int nomenu(struct etude_case *c)
{
    char image[4200];
    char *argv[] = {"mdel", "-i", image, "::/boot/grub/grub.cfg", NULL};
    struct table t;
    struct uefi s;
    struct net_run r;
    char line[160];

    begin(&s, c, 1);
    net_step(&s.u, "delete");
    read_table(&s, &t);
    snprintf(image, sizeof(image), "%s@@%llu", s.disk != NULL ? s.disk : "", t.esp);
    if (net_ok(&s.u) && t.esp == 0)
        net_fail(&s.u, "the disk's GUID table has no first partition");
    net_host(&s.u, argv, "mdel", &r);
    net_assert(&s.u, "menu-deleted", net_exited0(&r),
               "mdel could not delete the menu from partition 1: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);

    net_step(&s.u, "boot");
    up(&s, "uefi");
    as_user(&s, "root", "grep -o rach2.slot=recovery /proc/cmdline; echo NOMENU\"\"LOGIN", "nomenu",
            &r);
    net_assert(&s.u, "nomenu-recovery",
               has(r.out, "rach2.slot=recovery") && has(r.out, "NOMENULOGIN"),
               "the recovery system of the disk with no menu: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    quit(&s);
    return net_end(&s.u);
}

/** The case machine.uefi.stick or machine.uefi.cd: the firmware boots the
 * device `device`, whose option it names so, and recovery answers SSH. */
static int removable(struct etude_case *c, const char *booted, const char *login,
                     const char *device)
{
    char pattern[64];
    struct uefi s;
    struct net_run r;
    char line[160];

    begin(&s, c, 0);
    net_step(&s.u, "boot");
    up(&s, "uefi");
    snprintf(pattern, sizeof(pattern), "%s*%s", STARTED, device);
    net_assert(&s.u, booted, console_has(&s, pattern),
               "the firmware did not say it started the %s's boot option", device);
    as_user(&s, "root", "grep -c '^part ' /proc/disk; echo REMOVABLE\"\"LOGIN", "login", &r);
    net_assert(&s.u, login, net_exited0(&r) && has(r.out, "REMOVABLELOGIN"),
               "the recovery system over SSH: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    quit(&s);
    return net_end(&s.u);
}

/** Give the LBA the entry sector's blocklist names, as GRUB's boot code
 * reads it, or 0 after a failure. */
static unsigned long long blocklist(struct uefi *s, const struct table *t)
{
    unsigned char sector[512];

    if (disk_io(s, s->disk, 0, t->bios_first * 512, sector, sizeof(sector)) != 0)
        return 0;
    return le(sector + 500, 8);
}

/**
 * Read the table's own sectors into `buf`, of (first + 33) * 512 bytes: the
 * protective MBR and the primary header and array, the sectors before the
 * BIOS boot partition, and the backup array and header, the disk's last 33.
 */
static void table_sectors(struct uefi *s, const struct table *t, unsigned char *buf)
{
    if (disk_io(s, s->disk, 0, 0, buf, (size_t)t->bios_first * 512) == 0)
        disk_io(s, s->disk, 0, (t->sectors - 33) * 512, buf + t->bios_first * 512, 33 * 512);
}

/** Write zeros over `count` sectors of the copy from sector `lba`. */
static void zero_sectors(struct uefi *s, unsigned long long lba, unsigned long long count)
{
    unsigned char *zero = calloc((size_t)count, 512);

    if (zero == NULL)
        net_fail(&s->u, "no memory for %llu sectors of zeros", count);
    else
        disk_io(s, s->disk, 1, lba * 512, zero, (size_t)count * 512);
    free(zero);
}

/* The BIOS boot partition's two tails for GRUB's core, as grubslot.h lays
 * them out, and the table's own sectors before the first install. */
struct cores {
    struct table t;
    unsigned long long tail;     /* the sectors of each tail */
    unsigned long long tails[2]; /* the first LBA of tail A and of tail B */
    unsigned char *before;       /* the table's sectors, as table_sectors reads them */
    unsigned char *after;        /* and room to read them again */
};

/**
 * On the machine booted by SeaBIOS, install the core, which must land in
 * tail `x`, A or B, the letter of the assertions; end the machine, check the
 * entry sector and the table's sectors against those before the first
 * install, zero the other tail, and boot by SeaBIOS until the server
 * answers, which only tail `x` can bring up.
 */
static void swap_core(struct uefi *s, struct cores *k, char x)
{
    const char *core = etude_artifact(s->u.c, "gpt-core");
    unsigned long long want = k->tails[x == 'B'], other = k->tails[x != 'B'], named;
    char id[16], text[200], line[160], lower = (char)(x - 'A' + 'a');
    size_t n = (size_t)(k->t.bios_first + 33) * 512;
    struct net_run r;

    if (net_ok(&s->u) && core == NULL)
        net_fail(&s->u, "the case has no GPT core");
    snprintf(id, sizeof(id), "grub-%c", lower);
    send_file(s, core != NULL ? core : "",
              "cat > /tmp/core.img && bootctl grub /tmp/core.img && sync && echo GRUB\"\"DONE", id,
              &r);
    snprintf(text, sizeof(text),
             "in the tail at LBA %llu; the entry sector at LBA %llu now points there", want,
             k->t.bios_first);
    snprintf(id, sizeof(id), "install-%c", lower);
    net_assert(&s->u, id, has(r.out, text) && has(r.out, "GRUBDONE"),
               "bootctl grub on the GPT disk did not say %s: %s", text,
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    quit(s);
    named = blocklist(s, &k->t);
    snprintf(id, sizeof(id), "select-%c", lower);
    net_assert(&s->u, id, named == want, "the entry sector names LBA %llu, not %llu", named, want);
    if (k->after != NULL)
        table_sectors(s, &k->t, k->after);
    snprintf(id, sizeof(id), "table-%c", lower);
    net_assert(&s->u, id,
               k->before != NULL && k->after != NULL && memcmp(k->before, k->after, n) == 0,
               "the install changed a sector of the GUID table");
    zero_sectors(s, other, k->tail);
    up(s, "bios");
    snprintf(id, sizeof(id), "boots-%c", lower);
    net_assert(&s->u, id, 1, "");
}

/** The case machine.uefi.core. */
static int core(struct etude_case *c)
{
    char grubcore[160];
    struct cores k;
    struct uefi s;

    memset(&k, 0, sizeof(k));
    begin(&s, c, 1);
    net_step(&s.u, "bios-boot");
    read_table(&s, &k.t);
    if (net_ok(&s.u) && (k.t.bios_size < 3 || k.t.sectors < k.t.bios_first + 33))
        net_fail(&s.u, "the disk's GUID table has no BIOS boot partition");
    if (net_ok(&s.u)) {
        k.tail = (k.t.bios_size - 1) / 2;
        if (k.tail > 1023)
            k.tail = 1023;
        k.tails[0] = k.t.bios_first + 1;
        k.tails[1] = k.tails[0] + k.tail;
        k.before = malloc((size_t)(k.t.bios_first + 33) * 512);
        k.after = malloc((size_t)(k.t.bios_first + 33) * 512);
        if (k.before == NULL || k.after == NULL)
            net_fail(&s.u, "no memory for the table's sectors");
    }
    if (net_ok(&s.u))
        table_sectors(&s, &k.t, k.before);
    net_assert(&s.u, "built-tail-a", blocklist(&s, &k.t) == k.tails[0],
               "the entry sector at LBA %llu does not name tail A at LBA %llu", k.t.bios_first,
               k.tails[0]);
    up(&s, "bios");
    net_assert(&s.u, "bios-firmware", !console_has(&s, BDS),
               "the boot by SeaBIOS was made by UEFI firmware");
    snprintf(grubcore, sizeof(grubcore),
             "devfs: /dev/grubcore: BIOS boot partition 4, %llu sectors at LBA %llu", k.t.bios_size,
             k.t.bios_first);
    net_assert(&s.u, "grubcore-node", console_has(&s, grubcore), "the kernel did not say %s",
               grubcore);

    net_step(&s.u, "swap-b");
    swap_core(&s, &k, 'B');
    net_step(&s.u, "swap-a");
    swap_core(&s, &k, 'A');
    quit(&s);

    net_step(&s.u, "uefi-boot");
    power_on(&s, "uefi");
    net_wait(&s.b, LOGIN);
    net_assert(&s.u, "uefi-after", console_has(&s, STARTED "*HARDDISK"),
               "the firmware did not say it started the disk's boot option");
    quit(&s);
    free(k.before);
    free(k.after);
    return net_end(&s.u);
}

/**
 * The case machine.uefi.wide or machine.uefi.wide-bios: the install onto
 * the sparse NVMe disk, its boot and its scan. `uefi` says whether OVMF
 * boots the stick and the disk, and SeaBIOS otherwise.
 */
static int wide(struct etude_case *c, int uefi)
{
    /* The answers, in the order rach2-install asks: the disk, root's password
     * twice, the first account and its password twice, the demonstration key
     * the recovery system was reached with as that account's authorized key,
     * no key for the recovery system, the grow it offers, and the disk's name
     * again. */
    static const char answers[] =
        "n=$(awk '$1 == \"disk\" && $3 == \"nvme\" { print $2 }' /proc/disk)\n"
        "[ -n \"$n\" ] || { echo \"no NVMe disk\"; exit 1; }\n"
        "printf '%s\\n' \"/dev/disk$n\" uefi-big uefi-big owner owner-big owner-big \\\n"
        "    /root/.ssh/authorized_keys \"\" \"\" \"/dev/disk$n\" | rach2-install\n"
        "echo \"INSTALL STATUS $?\"\n"
        "sync\n";
    char script[4096], line[160], device[32];
    unsigned long long kib;
    const char *row;
    struct uefi s;
    struct net_run r;
    FILE *f;
    int found;

    begin(&s, c, 0);
    net_step(&s.u, "install");
    if (net_path(&s.u, "answers.sh", script, sizeof(script)) != NULL) {
        f = fopen(script, "w");
        if (f == NULL || fputs(answers, f) == EOF || fclose(f) != 0)
            net_fail(&s.u, "the answers could not be written");
    }
    up(&s, "install");
    send_file(&s, script, "sh -s", "install", &r);
    net_assert(&s.u, "installed",
               has(r.out, "growing the root into the rest of the disk") &&
                   line_begins(r.out, "INSTALL STATUS 0\n"),
               "the install onto the 4 TiB disk: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    quit(&s);

    net_step(&s.u, "boot");
    up(&s, "installed");
    if (uefi)
        net_assert(&s.u, "booted-nvme", console_has(&s, STARTED "*NVMe"),
                   "the firmware did not say it started the NVMe disk's boot option");
    else
        net_assert(&s.u, "booted-bios", !console_has(&s, BDS),
                   "the boot by SeaBIOS was made by UEFI firmware");
    as_user(&s, "owner", "df -k / | tail -n 1; echo BIG\"\"LOGIN", "df", &r);
    row = net_find(r.out, "/dev/disk#p3 *");
    found = row != NULL && sscanf(row, "%31s %llu", device, &kib) == 2 && kib > 2147483648ULL;
    net_assert(&s.u, "root-wide", found && has(r.out, "BIGLOGIN"),
               "the root of the 4 TiB disk, as the first account's df gives it: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    quit(&s);

    net_step(&s.u, "scan");
    power_on(&s, "scan");
    net_wait(&s.b, "ext2: root: ");
    net_wait(&s.b, "\n");
    net_assert(&s.u, "scan-root", console_has(&s, "ext2: root: /dev/disk0p3"),
               "rach2.root=ext2 did not find the 4 TiB disk's root");
    quit(&s);
    return net_end(&s.u);
}

/** Give the CRC-32 of `n` bytes at `p`, as the GUID table's headers and
 * arrays have it: the reflected polynomial 0xEDB88320, from all ones, and
 * the result inverted. */
static unsigned long crc32_of(const unsigned char *p, size_t n)
{
    unsigned long crc = 0xFFFFFFFFUL;
    int bit;

    while (n-- > 0) {
        crc ^= *p++;
        for (bit = 0; bit < 8; bit++)
            crc = crc & 1 ? crc >> 1 ^ 0xEDB88320UL : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFUL;
}

/** Say whether the copy's primary GUID header, at LBA 1, checks: its
 * signature, and the CRC-32 of its own bytes with the field that holds it
 * taken as zero. */
static int primary_checks(struct uefi *s, unsigned long *stored, unsigned long *computed)
{
    unsigned char header[512];
    unsigned long size;

    *stored = *computed = 0;
    if (disk_io(s, s->disk, 0, 512, header, sizeof(header)) != 0 ||
        memcmp(header, "EFI PART", 8) != 0)
        return 0;
    size = (unsigned long)le(header + 12, 4);
    if (size < 92 || size > sizeof(header))
        return 0;
    *stored = (unsigned long)le(header + 16, 4);
    memset(header + 16, 0, 4);
    *computed = crc32_of(header, size);
    return *stored == *computed;
}

/** The case machine.uefi.damaged. */
static int damaged(struct etude_case *c)
{
    char mark[] = "RACH";
    unsigned long stored, computed;
    struct uefi s;
    struct net_run r;
    char line[160];
    int checks;

    begin(&s, c, 1);
    net_step(&s.u, "relocate");
    /* Four bytes of the primary header's checksum, at byte 16 of LBA 1. */
    disk_io(&s, s.disk, 1, 528, mark, 4);
    up(&s, "rescue");
    as_user(&s, "root", "diskctl relocate /dev/disk0; sync", "relocate", &r);
    net_assert(&s.u, "damaged-named",
               console_has(&s, "init: /dev/disk0: its primary GUID table does not check and was "
                               "read from the backup"),
               "the recovery system did not name the damaged table");
    net_assert(&s.u, "relocated",
               has(r.out, "the primary table was written again from the backup") ||
                   has(r.err, "the primary table was written again from the backup"),
               "diskctl relocate on the damaged table: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    quit(&s);
    checks = primary_checks(&s, &stored, &computed);
    net_assert(&s.u, "primary-checks", checks,
               "the copy's primary header has the CRC-32 0x%08lx, and its bytes give 0x%08lx",
               stored, computed);

    net_step(&s.u, "repaired");
    power_on(&s, "repaired");
    net_wait(&s.b, LOGIN);
    net_assert(&s.u, "repaired-boots", !console_has(&s, "the primary GPT is damaged"),
               "the kernel of the repaired disk's boot read the backup table");
    quit(&s);
    return net_end(&s.u);
}

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c);

    if (strcmp(id, "machine.uefi.installed") == 0)
        return installed(c);
    if (strcmp(id, "machine.uefi.recovery") == 0)
        return recovery(c);
    if (strcmp(id, "machine.uefi.nomenu") == 0)
        return nomenu(c);
    if (strcmp(id, "machine.uefi.stick") == 0)
        return removable(c, "booted-stick", "stick-login", "USB HARDDRIVE");
    if (strcmp(id, "machine.uefi.cd") == 0)
        return removable(c, "booted-cd", "cd-login", "DVD-ROM");
    if (strcmp(id, "machine.uefi.core") == 0)
        return core(c);
    if (strcmp(id, "machine.uefi.wide") == 0)
        return wide(c, 1);
    if (strcmp(id, "machine.uefi.wide-bios") == 0)
        return wide(c, 0);
    if (strcmp(id, "machine.uefi.damaged") == 0)
        return damaged(c);
    return 1;
}
