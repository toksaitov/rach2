/*
 * metal_scenario.c: the scenario of the machine cases of
 * tests/cases/machine.metal.json: the metal image reached over SSH with
 * nobody at its console, metal_accept.sh rehearsed against it, and the
 * script's installed-root rows on small GRUB disks.
 *
 * The metal image is the one the Lenovo boots. machine.metal.unattended
 * boots it with a LAN address on the kernel command line, as the stick
 * gives one, and types nothing at the console, so everything it checks
 * follows from the machine having booted on its own: init supervising
 * dropbear, the address reaching the stack, root's key, a pty, a file each
 * way, the boot and system logs, and init's restart of a killed server.
 * Then it runs the release's own acceptance script against the machine,
 * once with no stick named, which must write none, and once through su with
 * the stick ACCEPT01 named, beside two recovery sticks the script must
 * refuse, and judges what the script wrote to the stick with the host's
 * tools.
 *
 * The emulator shows that the metal kernel brings up the network, the USB
 * controller, a hub, a keyboard and the sticks without disturbing anything
 * else it does; the firmware's handoff and the chipset's port routing have
 * no emulation, and the release checks them on the Lenovo.
 *
 * The installed cases each boot one disk that
 * tests/system/metal_installed_disks.sh made, with BusyBox, bootctl and the
 * rows of metal_accept.sh that read the command line, the disk and the ext2
 * root, type `exec /bin/sh /check.sh` at its debug shell, and judge the
 * status and the lines the rows printed.
 *
 * The scenario runs the tree's metal_accept.sh and su_command.sh, as the
 * release does, from the root that ETUDE_TREE names.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "netcase.h"

/* What init prints each time it starts the SSH server, before its pid. */
#define SSHD_STARTED "init: sshd: started as pid "

/* The scripts the unattended case runs, by their paths in the tree. */
#define ACCEPT     "tests/system/accept/metal_accept.sh"
#define SU_COMMAND "tests/system/accept/su_command.sh"

/* The lines of the file sent to the guest and read back. */
#define SEND_LINES 1000

/* The sectors of the stick's KRFFS volume, as metal_sticks.sh makes it. */
#define STICK_VOLUME "skip=100352 count=32768"

/* What the acceptance says of each recovery stick. */
#define RECOVERY_REFUSED "carries a Rach2 recovery ISO in its table and is not written"

/* What the unattended case's steps share. */
struct metal_case {
    struct net_case u;
    struct net_boot b;
    char port[16];
    char root_password[128];
    char demo_key[4096]; /* the paths of the private copies of the keys */
    char test_key[4096];
    const char *stick; /* the copy of the stick the acceptance writes */
    char accept[4096]; /* the absolute paths of the tree's scripts, since a */
    char su[4096];     /* host tool runs in a directory of its own */
};

/** Say whether `text` has `part`; no text has nothing. */
static int has(const char *text, const char *part)
{
    return text != NULL && strstr(text, part) != NULL;
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

/** Say whether the words of `text` include the number `n`. */
static int has_pid(const char *text, long n)
{
    const char *p;
    char *end;

    for (p = text; p != NULL && *p != '\0'; p = end) {
        p += strspn(p, " \t\r\n");
        if (*p == '\0')
            break;
        if (strtol(p, &end, 10) == n && end != p && strchr(" \t\r\n", *end) != NULL)
            return 1;
        if (end == p)
            end = (char *)p + strcspn(p, " \t\r\n");
    }
    return 0;
}

/** Put `name` in the environment with `value`, or fail the attempt. */
static void put_env(struct metal_case *s, const char *name, const char *value)
{
    if (net_ok(&s->u) && setenv(name, value, 1) != 0)
        net_fail(&s->u, "%s could not be put in the environment", name);
}

/** Take the acceptance's settings out of the environment. */
static void clear_env(void)
{
    unsetenv("RACH2_ACCEPT_PASSWORD");
    unsetenv("RACH2_ACCEPT_SU");
    unsetenv("RACH2_ACCEPT_FAT_IMAGE");
    unsetenv("RACH2_ACCEPT_USER");
    unsetenv("RACH2_ACCEPT_KEY");
}

/**
 * Run `command` on the machine as root by the demo key, on a terminal when
 * `tty` is nonzero, keeping what it prints as evidence `output`. ssh reads
 * no file of the account that runs it, offers that key alone, and keeps no
 * record of the host's key.
 */
static enum etude_status as_root(struct metal_case *s, const char *command, int tty,
                                 const char *output, struct net_run *r)
{
    char *argv[] = {NET_SSH, NET_SSH_KEY(s->demo_key), tty ? "-tt" : "-T", "-p",
                    s->port, "root@127.0.0.1",         (char *)command,    NULL};

    return net_host(&s->u, argv, output, r);
}

/** Give the pid of init's `n`th record of starting the server, counted
 * from 1, once the whole line has arrived, or -1. */
static long sshd_pid(struct metal_case *s, unsigned n)
{
    uint64_t at;
    char *text;
    long pid;

    if (net_times(&s->b, SSHD_STARTED, n, &at) != ETUDE_OK)
        return -1;
    s->b.at = at;
    if (net_wait(&s->b, "\n") != ETUDE_OK)
        return -1;
    text = net_text(&s->b, at, s->b.at);
    pid = text[0] >= '0' && text[0] <= '9' ? strtol(text, NULL, 10) : -1;
    free(text);
    if (pid < 0)
        net_fail(&s->u, "init's record %u of starting the server has no pid", n);
    return pid;
}

/** Wait until the server answers on the case's port, as sshpeer judges it:
 * a connection that has the server's identification. */
static void answered(struct metal_case *s, const char *output)
{
    char *argv[] = {"sshpeer", "wait", s->port, NULL};
    struct net_run r;
    char line[160];

    net_host(&s->u, argv, output, &r);
    if (net_ok(&s->u) && !(net_exited0(&r) && has(r.out, "sshpeer: answered")))
        net_fail(&s->u, "sshpeer: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/** Boot the metal image and read what its console says before the login
 * prompt: the address and the disk policy. */
static void boot(struct metal_case *s)
{
    char *text = NULL;
    uint64_t at;

    net_step(&s->u, "boot");
    net_boot(&s->b, &s->u, "metal", 1, NULL);
    if (net_times(&s->b, "rach2 login", 1, &at) == ETUDE_OK)
        text = net_text(&s->b, 0, at);
    net_assert(&s->u, "login-prompt", 1, "");
    net_assert(&s->u, "address", has(text, "IPv4 192.168.90.15/24"),
               "the kernel did not report the address of its command line");
    net_assert(&s->u, "disk-policy", has(text, "disk: policy: read-only"),
               "the kernel did not report the read-only disk policy");
    free(text);
}

/** Wait for init's server, with nobody at the console, and run a command
 * by key and `tty` on a terminal; give the server's pid. */
static long server(struct metal_case *s)
{
    struct net_run r;
    char line[160];
    long pid;

    net_step(&s->u, "server");
    pid = sshd_pid(s, 1);
    answered(s, "answered");
    net_assert(&s->u, "server", 1, "");
    net_step(&s->u, "session");
    as_root(s, "echo METAL\"\"READY", 0, "ready", &r);
    net_assert(&s->u, "key-command", net_exited0(&r) && has(r.out, "METALREADY"),
               "the command did not run: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    as_root(s, "tty; exit", 1, "tty", &r);
    net_assert(&s->u, "pty", net_find(r.out, "/dev/pts/") != NULL, "the session had no pty: %s",
               said(&r, line, sizeof(line)));
    net_run_free(&r);
    return pid;
}

/** Give the first two words of `text`, a CRC and a size from cksum, in
 * `buf` of `size` bytes, or "none". */
static const char *crc_and_size(const char *text, char *buf, size_t size)
{
    unsigned long crc, bytes;

    if (text == NULL || sscanf(text, "%lu %lu", &crc, &bytes) != 2)
        snprintf(buf, size, "none");
    else
        snprintf(buf, size, "%lu %lu", crc, bytes);
    return buf;
}

/** Write the file to send, SEND_LINES lines each with its number, so that
 * a lost or repeated chunk changes it, at `path`. */
static void make_send(struct metal_case *s, const char *path)
{
    FILE *f;
    int i;

    if (!net_ok(&s->u))
        return;
    f = fopen(path, "w");
    for (i = 0; f != NULL && i < SEND_LINES; i++)
        fprintf(f, "line %d the quick brown fox jumps over the lazy dog\n", i);
    if (f == NULL || fclose(f) != 0)
        net_fail(&s->u, "the file to send could not be written");
}

/** Send a file to the guest, compare the cksum the guest reads of it with
 * the host's, and read it back. */
static void transfer(struct metal_case *s)
{
    char send[4096] = "", host[64], guest[64], line[160], *text;
    char *put[] = {NET_SSH,          NET_SSH_KEY(s->demo_key), "-T", "-p", s->port,
                   "root@127.0.0.1", "cat > /tmp/metal.in",    NULL};
    char *sum[] = {"cksum", send, NULL};
    struct net_run r;

    net_step(&s->u, "transfer");
    net_path(&s->u, "send", send, sizeof(send));
    make_send(s, send);
    net_host(&s->u, sum, "send.cksum", &r);
    crc_and_size(r.out, host, sizeof(host));
    net_run_free(&r);
    net_ssh_input(&s->u, put, send, "sent", &r);
    if (net_ok(&s->u) && !net_exited0(&r))
        net_fail(&s->u, "the file could not be sent: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    as_root(s, "cksum < /tmp/metal.in", 0, "guest.cksum", &r);
    crc_and_size(r.out, guest, sizeof(guest));
    net_run_free(&r);
    net_assert(&s->u, "file-sent", strcmp(host, "none") != 0 && strcmp(host, guest) == 0,
               "sent [%s], the guest read [%s]", host, guest);
    as_root(s, "cat /tmp/metal.in", 0, "back", &r);
    text = net_file(send);
    net_assert(&s->u, "file-back", net_exited0(&r) && r.out != NULL && strcmp(r.out, text) == 0,
               "the file came back changed: %lu bytes of %lu",
               (unsigned long)(r.out != NULL ? strlen(r.out) : 0), (unsigned long)strlen(text));
    free(text);
    net_run_free(&r);
}

/**
 * Kill the server init reported starting, `before`, and require init to
 * report starting another, which answers and is running. init reports a
 * second start only after it has reaped the first, so a kill that failed
 * cannot pass, and the second must run under another pid.
 */
static void restart(struct metal_case *s, long before)
{
    char command[64];
    struct net_run r;
    long after = -1;
    int running, again;

    net_step(&s->u, "restart");
    as_root(s, "pidof dropbearmulti", 0, "pidof.before", &r);
    running = has_pid(r.out, before);
    net_run_free(&r);
    if (net_ok(&s->u) && !running)
        net_fail(&s->u, "the server init reported starting, pid %ld, is not running", before);
    snprintf(command, sizeof(command), "kill -9 %ld", before);
    as_root(s, command, 0, "kill", &r);
    net_run_free(&r);
    after = sshd_pid(s, 2);
    answered(s, "answered.again");
    as_root(s, "pidof dropbearmulti", 0, "pidof.after", &r);
    again = has_pid(r.out, after);
    net_run_free(&r);
    net_assert(&s->u, "restart", again && after != before,
               "the second server, pid %ld after %ld, is not the one running", after, before);
}

/** Read the machine's own boot log, and send a line to the system log
 * through /dev/log. syslogd writes the line after logger has returned, so
 * the guest reads its log until the line is there; the step's bound ends a
 * guest that never writes it. */
static void logs(struct metal_case *s)
{
    struct net_run r;
    char line[160];

    net_step(&s->u, "logs");
    as_root(s, "cat /dev/bootlog", 0, "bootlog", &r);
    net_assert(&s->u, "bootlog",
               has(r.out, "Rach2 Booting") && has(r.out, "Initializing system components") &&
                   has(r.out, "net: ready"),
               "the boot log lacks a line of the boot: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    as_root(s,
            "logger -t metaltest 'a line for the system log' || exit 1; "
            "while [ \"$(grep -c 'metaltest: a line for the system log' /var/log/messages)\" "
            "!= 1 ]; do :; done; echo SYS\"\"LOG",
            0, "syslog", &r);
    net_assert(&s->u, "syslog", net_exited0(&r) && has(r.out, "SYSLOG"),
               "logger's line did not reach /var/log/messages: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
}

/** Run a quoted command that exits 7 as root through su_command.sh, logged
 * in as the test account, and require its output and status unchanged. */
static void su(struct metal_case *s)
{
    char known[4096] = "";
    char *argv[] = {
        "sh",    s->su,  s->test_key, "127.0.0.1",
        s->port, "test", known,       "printf '%s\\n' \"a 'quote' and a dollar \\$\"; exit 7",
        NULL};
    struct net_run r;
    size_t n = 0;

    net_step(&s->u, "su");
    net_path(&s->u, "su.known", known, sizeof(known));
    put_env(s, "RACH2_ACCEPT_PASSWORD", s->root_password);
    net_host(&s->u, argv, "su", &r);
    clear_env();
    if (r.out != NULL)
        for (n = strlen(r.out); n > 0 && strchr("\r\n", r.out[n - 1]) != NULL; n--)
            r.out[n - 1] = '\0';
    net_assert(&s->u, "su-quoting",
               r.end.end == ETUDE_END_EXITED && r.end.value == 7 && r.out != NULL &&
                   strcmp(r.out, "a 'quote' and a dollar $") == 0,
               "status %d, output [%.80s]", r.end.value, r.out != NULL ? r.out : "");
    net_run_free(&r);
}

/** Give the cksum of the stick in `buf` of `size` bytes, or "none". */
static const char *stick_sum(struct metal_case *s, const char *output, char *buf, size_t size)
{
    char *argv[] = {"cksum", (char *)s->stick, NULL};
    struct net_run r;

    net_host(&s->u, argv, output, &r);
    crc_and_size(net_exited0(&r) ? r.out : NULL, buf, size);
    net_run_free(&r);
    return buf;
}

/** Run the acceptance script with `serial` named, or none for NULL,
 * keeping its output as evidence `output`. */
static void accept_run(struct metal_case *s, const char *serial, const char *output,
                       struct net_run *r)
{
    char *argv[] = {"sh", s->accept, "127.0.0.1", s->port, (char *)serial, NULL};

    net_host(&s->u, argv, output, r);
    clear_env();
}

/** The acceptance with no stick named: it must pass and write none. */
static void unnamed(struct metal_case *s)
{
    char before[64], after[64], line[160];
    struct net_run r;

    net_step(&s->u, "unnamed");
    stick_sum(s, "stick.before", before, sizeof(before));
    put_env(s, "RACH2_ACCEPT_KEY", s->demo_key);
    accept_run(s, NULL, "accept.unnamed", &r);
    net_assert(&s->u, "unnamed-pass",
               printed(&r, "metal-accept: PASS") &&
                   printed(&r, "was not named by the owner and is not written") &&
                   !printed(&r, "is the one written"),
               "the acceptance with no stick named: %s", said(&r, line, sizeof(line)));
    net_run_free(&r);
    stick_sum(s, "stick.after", after, sizeof(after));
    net_assert(&s->u, "unnamed-stick", strcmp(before, "none") != 0 && strcmp(before, after) == 0,
               "the stick's cksum was [%s] and is [%s]", before, after);
}

/** Give the flush count of the stick, disk0, in /proc/usb, or -1. */
static long flushes(struct metal_case *s, const char *output)
{
    struct net_run r;
    char line[256];
    long n;

    as_root(s, "cat /proc/usb", 0, output, &r);
    n = net_number_after(net_copy_line(net_find(r.out, "disk0 *flushes #"), line, sizeof(line)),
                         "flushes ");
    net_run_free(&r);
    return n;
}

/**
 * The acceptance as the release runs it on the Lenovo: logged in as the test
 * account by its key, root's commands through su with root's password, and
 * the stick ACCEPT01 named, whose FAT volume the script also reads on the
 * host. The stick's flushes are counted around it, since it must flush what
 * it wrote to the stick before it unmounts. Its run is left in `r`.
 */
static void accept(struct metal_case *s, struct net_run *r)
{
    char image[4200];
    long before, after;

    net_step(&s->u, "accept");
    before = flushes(s, "usb.before");
    snprintf(image, sizeof(image), "%s@@1048576", s->stick != NULL ? s->stick : "");
    put_env(s, "RACH2_ACCEPT_SU", "1");
    put_env(s, "RACH2_ACCEPT_FAT_IMAGE", image);
    put_env(s, "RACH2_ACCEPT_USER", "test");
    put_env(s, "RACH2_ACCEPT_KEY", s->test_key);
    put_env(s, "RACH2_ACCEPT_PASSWORD", s->root_password);
    accept_run(s, "ACCEPT01", "accept", r);
    after = flushes(s, "usb.after");
    net_assert(&s->u, "accept-pass", net_exited0(r) && printed(r, "metal-accept: PASS"),
               "the acceptance did not pass, status %d", r->end.value);
    net_assert(&s->u, "accept-flush", before >= 0 && after > before,
               "the stick's flushes went from %ld to %ld", before, after);
    net_assert(&s->u, "accept-disk",
               printed(r, "disk: disk 0 usb ") &&
                   printed(r, "disk: identity 0 512 removable free ACCEPT01") &&
                   printed(r, "disk: the MBR hashes to "),
               "the disk row did not read the registry");
    net_assert(&s->u, "accept-image", printed(r, "the file system in that file mounts and reads"),
               "the image row did not mount and read its file");
    net_assert(&s->u, "accept-fat",
               printed(r, "stick: the volume ACCEPT on /dev/disk#p# mounts") &&
                   printed(r, "stick: remount bytes match after successful unmounts") &&
                   printed(r, "stick: host FAT bytes match while unmounted"),
               "the stick section did not write and read the volume ACCEPT");
    net_assert(&s->u, "accept-sticks",
               net_count(r->out, RECOVERY_REFUSED) + net_count(r->err, RECOVERY_REFUSED) == 2 &&
                   printed(r, "stick: /dev/disk# (ACCEPT01) is the one written"),
               "the acceptance did not refuse both recovery sticks and take ACCEPT01");
    net_assert(&s->u, "accept-krffs",
               printed(r, "stick: the KRFFS volume on * mounts, reads and takes a write"),
               "the KRFFS section did not write the stick's volume");
    net_assert(&s->u, "accept-hub",
               printed(r, "behind a hub: port") && printed(r, "hub: device #: # ports"),
               "the hub section did not find the hub and the devices behind it");
}

/** Judge with the host's tools what the acceptance, whose run is `r`, wrote
 * to the stick, and read the boot log once more. */
static void stick(struct metal_case *s, const struct net_run *r)
{
    char volume[4096] = "", file[4096] = "", mbr[128], line[160], *text;
    char *take[] = {"sh",
                    "-c",
                    "dd if=\"$1\" of=\"$2\" bs=512 " STICK_VOLUME " 2>/dev/null",
                    "sh",
                    (char *)s->stick,
                    volume,
                    NULL};
    char *fsck[] = {"fsck-krffs", volume, "--strict", NULL};
    char *readfile[] = {"readfile-krffs", volume, "/accept.txt", file, NULL};
    char *hash[] = {"sh",
                    "-c",
                    "dd if=\"$1\" bs=512 count=1 2>/dev/null | shasum -a 256",
                    "sh",
                    (char *)s->stick,
                    NULL};
    struct net_run t;
    int clean;

    net_step(&s->u, "stick");
    net_path(&s->u, "stick.krffs", volume, sizeof(volume));
    net_path(&s->u, "accept.txt", file, sizeof(file));
    net_host(&s->u, take, "stick.take", &t);
    if (net_ok(&s->u) && !net_exited0(&t))
        net_fail(&s->u, "the stick's KRFFS volume could not be copied");
    net_run_free(&t);
    net_host(&s->u, fsck, "stick.fsck", &t);
    clean = net_exited0(&t);
    net_run_free(&t);
    net_host(&s->u, readfile, "stick.read", &t);
    text = net_file(file);
    net_assert(&s->u, "stick-volume", clean && net_exited0(&t) && has(text, "written-on-the-stick"),
               clean ? "the stick's volume has no /accept.txt with the machine's bytes"
                     : "fsck.krffs --strict finds the stick's volume faulty");
    free(text);
    net_run_free(&t);
    snprintf(mbr, sizeof(mbr), "disk: the MBR hashes to ");
    net_host(&s->u, hash, "stick.mbr", &t);
    if (net_exited0(&t) && strspn(t.out, "0123456789abcdef") == 64)
        strncat(mbr, t.out, 64);
    net_run_free(&t);
    net_assert(&s->u, "stick-mbr", strlen(mbr) == 24 + 64 && printed(r, mbr),
               "the acceptance did not print the hash of the stick's first sector, [%s]", mbr + 24);
    as_root(s, "cat /dev/bootlog", 0, "bootlog.after", &t);
    net_assert(&s->u, "bootlog-kept",
               has(t.out, "boot: Rach2 Booting") && has(t.out, "usb: controller:") &&
                   has(t.out, "lapic:"),
               "the boot log lost its early lines: %s", said(&t, line, sizeof(line)));
    net_run_free(&t);
}

/** Give in `buf` of `size` bytes the absolute path of `path`, a script of
 * the tree that ETUDE_TREE names, or of the working directory without it,
 * and say whether it is there. */
static int in_tree(const char *path, char *buf, size_t size)
{
    char dir[4096];
    const char *tree = getenv("ETUDE_TREE");

    if (tree == NULL || tree[0] != '/')
        tree = getcwd(dir, sizeof(dir));
    return tree != NULL && (size_t)snprintf(buf, size, "%s/%s", tree, path) < size &&
           access(buf, R_OK) == 0;
}

/** The case machine.metal.unattended. The acceptance saves the boot log it
 * reads with the attempt's evidence. */
static int unattended(struct etude_case *c)
{
    char bootlog[4096] = "", *slash;
    struct metal_case s;
    struct net_run r;
    unsigned port;
    long pid;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    port = etude_port(c, 0);
    snprintf(s.port, sizeof(s.port), "%u", port);
    if (port == 0)
        net_fail(&s.u, "the case has no port");
    if (net_ok(&s.u) &&
        (!in_tree(ACCEPT, s.accept, sizeof(s.accept)) || !in_tree(SU_COMMAND, s.su, sizeof(s.su))))
        net_fail(&s.u, "%s is not in the tree: set ETUDE_TREE to its root", ACCEPT);
    net_secret(&s.u, "demo-password", s.root_password, sizeof(s.root_password));
    net_key(&s.u, "demo-key", s.demo_key, sizeof(s.demo_key));
    net_key(&s.u, "test-key", s.test_key, sizeof(s.test_key));
    if (net_path(&s.u, "metal-bootlog.txt", bootlog, sizeof(bootlog)) != NULL &&
        (slash = strrchr(bootlog, '/')) != NULL) {
        *slash = '\0';
        put_env(&s, "RACH2_ACCEPT_RECORDS", bootlog);
    }
    if (net_ok(&s.u) && (s.stick = etude_private_copy(c, "stick")) == NULL)
        net_fail(&s.u, "the stick has no private copy");
    boot(&s);
    pid = server(&s);
    transfer(&s);
    restart(&s, pid);
    logs(&s);
    su(&s);
    unnamed(&s);
    accept(&s, &r);
    stick(&s, &r);
    net_run_free(&r);
    return net_end(&s.u);
}

/* An installed case: its row's status and the lines its rows print. */
struct installed {
    const char *case_id;
    const char *status;
    struct {
        const char *assertion;
        const char *pattern;
    } want[4];
};

static const struct installed rows[] = {
    {"machine.metal.installed-candidate",
     "accept-status: 0",
     {{"root-writable", "  the ext2 root is mounted writable from /dev/disk0p3"},
      {"candidate-slot", "  the ext2 root boot came from the candidate slot"},
      {"root-write", "  the ext2 root takes a file and gives it back"}}},
    {"machine.metal.installed-gpt",
     "accept-status: 0",
     {{"gpt-cmdline", "cmdline: *rach2.root=GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,3 "},
      {"root-writable", "  the ext2 root is mounted writable from /dev/disk0p3"},
      {"candidate-slot", "  the ext2 root boot came from the candidate slot"},
      {"root-write", "  the ext2 root takes a file and gives it back"}}},
    {"machine.metal.installed-wrong-partition",
     "accept-status: 1",
     {{"mounted", "/dev/disk0p4 / ext2 rw,relatime"},
      {"refused", "  FAIL: the ext2 root is not the mounted root:"}}},
    {"machine.metal.installed-readonly",
     "accept-status: 1",
     {{"mounted", "/dev/disk0p3 / ext2 ro,relatime"},
      {"refused", "  FAIL: the ext2 root is not the mounted root:"}}},
    {"machine.metal.installed-fallback",
     "accept-status: 1",
     {{"fallback-cmdline", "cmdline: *rach2.slot=fallback"},
      {"refused", "  FAIL: the ext2 root boot did not come from the candidate slot:"}}},
};

/** Say whether `text` has `prefix` followed by 64 hexadecimal digits. */
static int has_hash(const char *text, const char *prefix)
{
    const char *p = text;

    while ((p = strstr(p, prefix)) != NULL) {
        p += strlen(prefix);
        if (strspn(p, "0123456789abcdef") == 64)
            return 1;
    }
    return 0;
}

/** An installed case, `row`: boot the disk, run /check.sh at the debug
 * shell, and judge its status and lines. */
static int installed(struct etude_case *c, const struct installed *row)
{
    struct net_case u;
    struct net_boot b;
    char *text = NULL, *console = NULL;
    size_t i;

    net_case(&u, c);
    net_step(&u, "boot");
    net_boot(&b, &u, "installed", 1, NULL);
    net_wait(&b, NET_SHELL);
    net_step(&u, "rows");
    if (net_command(&b, "exec /bin/sh /check.sh", "METAL_INSTALLED_DONE", &text) == ETUDE_OK)
        console = net_text(&b, 0, b.at);
    net_assert(&u, "status", net_find(text, row->status) != NULL, "/check.sh did not print `%s`",
               row->status);
    net_assert(&u, "mbr", text != NULL && has_hash(text, "  disk: the MBR hashes to "),
               "the rows printed no hash of the MBR");
    net_assert(&u, "no-diagnostic", console != NULL && strstr(console, "rach2.diskdiag") == NULL,
               "the console has rach2.diskdiag");
    for (i = 0; i < 4 && row->want[i].assertion != NULL; i++)
        net_assert(&u, row->want[i].assertion, net_find(text, row->want[i].pattern) != NULL,
                   "no line `%s`", row->want[i].pattern);
    free(text);
    free(console);
    return net_end(&u);
}

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c);
    size_t i;

    if (strcmp(id, "machine.metal.unattended") == 0)
        return unattended(c);
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        if (strcmp(id, rows[i].case_id) == 0)
            return installed(c, &rows[i]);
    fprintf(stderr, "metal_scenario: no case %s\n", id);
    return 1;
}
