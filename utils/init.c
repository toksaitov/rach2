/**
 * Rach2 PID 1: it reaps orphans and keeps a console session running.
 *
 * The kernel starts this as the first user process, so it is init, and it
 * reparents every orphaned process onto it. Its core job is three things a
 * system needs for as long as it runs: reap whatever children land on it (the
 * console session and every orphan the kernel hands over), respawn that
 * session when it exits, so the console is never left without one, and
 * continue a child that is stopped, since nothing else can. Beside that it
 * clears `/var/run`, sets the resource limits and keeps `/bin/update`
 * running. It never exits; if it did, the kernel would have no reaper, so the
 * kernel panics if init dies.
 *
 * Four artifacts are built from this one source, and none of them is named
 * plain `init`: `init-shell` runs a shell on the console, `init-login` puts
 * that console behind `/bin/login`, `init-metal` is `init-login` plus the
 * network and a supervised SSH server, and `init-recovery` is the recovery
 * system's, a shell on the console with the metal build's network, server and
 * file system check beside it. The metal build exists because the machine it
 * is for is across the room: nobody is going to type a command at its console
 * to start a daemon, so the daemon has to be running when the machine
 * finishes booting. The console login stays exactly as it is beside it, and
 * is the way back in when the network is the thing that is broken.
 *
 * Built like the other utils, linked against the ported musl (see
 * utils/Makefile), and staged at /sbin/init by the image manifests.
 */
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

extern char **environ;

#ifdef INIT_CONSOLE_LOGIN
/* The one program the console session may exec in this build. There is no
 * list and no fallback: a fall-through to a shell on any exec failure hands
 * the console to whoever is standing at it, and narrowing the fall-through to
 * ENOENT inverts the danger instead of removing it, since ENOENT is exactly
 * the case of an image built without /bin/login, which is the image that must
 * refuse hardest. So the fallback is compiled out: this artifact has no code
 * path and no string that names a shell. */
#define LOGIN_PROGRAM "/bin/login"

/* The pause before a failed exec is tried again. Longer than the shell build's,
 * so a permanent failure states its reason on the console instead of scrolling
 * it away. */
#define LOGIN_RESPAWN_PAUSE 5
#else
/* The login shell, tried in order: /bin/sh then /ish. The recovery system's
 * init and the `init-shell` build take this path; the other images put the
 * console behind a login, and `init-shell` boots only the test images of the
 * login and shutdown suites. The matching argv[0] has a leading '-', which
 * makes ash a login shell that reads /etc/profile. */
static const char *const Shells[] = {"/bin/sh", "/ish", 0};
static const char *const Shell_Arg0[] = {"-sh", "-ish", 0};
#endif

#ifdef INIT_METAL
/*
 * The SSH server this build supervises, and how it is asked to run. An image
 * that has OpenSSH's server at `OPENSSH_PROGRAM`, which the GNU system's root
 * has, runs that; every other image runs Dropbear's.
 *
 * `dropbearmulti` dispatches on its first argument, so the program name is
 * argv[1] and the flags follow. `-F` keeps it in the foreground, which is what
 * makes it supervisable at all: without it dropbear forks and the parent
 * exits, and init would see that exit as a death and respawn it forever
 * against a daemon that is in fact running. `-R` generates the host keys that
 * are missing, which on a RAM root is every boot and needs the kernel's
 * entropy to be trusted, so such a machine whose CPU offers neither RDSEED nor
 * RDRAND will not get an SSH server (and will still get its console login,
 * which is why both are kept); an installed root keeps its keys. `-g` bars
 * root's password from the exchange, so root arrives by key, unless
 * `/etc/rach2/sshd.conf` permits it (`root_password_login`).
 *
 * OpenSSH's `sshd` takes `-D` for the same foreground and `-e` to write its
 * log to standard error, which is the service log, since the system has no
 * syslog daemon. Its argv[0] is its absolute path, without which it refuses
 * to start, since a SIGHUP makes it execute itself again by that name to
 * read its configuration anew. It makes no host keys: `ssh-keygen -A` runs
 * before each start and makes those missing from `/etc/ssh`, every one on a
 * first boot, with the same need for the kernel's entropy. The same policy
 * file decides root's password, which `PermitRootLogin prohibit-password`
 * bars and `PermitRootLogin yes` permits.
 */
#define SSHD_PROGRAM    "/bin/dropbearmulti"
#define OPENSSH_PROGRAM "/usr/sbin/sshd"
#define OPENSSH_KEYGEN  "/usr/bin/ssh-keygen"

/* The pause after an SSH server that died almost at once. Long enough that a
 * server failing for a permanent reason (no entropy, no network, a port
 * already taken) does not respawn at full speed, and short enough that a
 * machine reachable only over the network is reachable again quickly. */
#define SSHD_RESPAWN_PAUSE 5
#endif

#ifdef INIT_CONSOLE_LOGIN
/* Whether a session child has already failed to exec the login program in
 * this boot. init sets it in `session_ended`; the children forked after that
 * inherit it, which is how the rescue hint is printed once per boot. */
static int login_hint_given;

/**
 * Say on the console that the login program could not be run, and, the first
 * time in a boot, how to get back in. Returns nothing.
 *
 * Written with `write`, not `printf`, because this runs in a child that
 * has just failed an `execve`, and the only thing it can still be sure of is a
 * descriptor. The second line gives the way back in: `rach2.rescue` boots the
 * kernel debug shell with the file system mounted and writable. On the login
 * build it is the only way back in, since nothing there starts a network
 * service; the metal build also has its SSH server. The child exits at once,
 * so it cannot remember that the hint was given; the flag it reads is init's,
 * copied at the fork.
 */
static void report_no_login(void)
{
    static const char first[] = "init: cannot exec /bin/login\n";
    static const char second[] = "init: boot rach2.rescue for the kernel debug shell\n";

    (void)write(STDERR_FILENO, first, sizeof(first) - 1);
    if (!login_hint_given) {
        (void)write(STDERR_FILENO, second, sizeof(second) - 1);
    }
}

/**
 * Record how the console session ended, `lived` seconds after its fork. A
 * session child that exited with 127 in under two seconds is one whose exec of
 * the login program failed, and it has printed the rescue hint, so the
 * children forked after it leave the hint out.
 */
static void session_ended(int status, time_t lived)
{
    if (lived < 2 && WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        login_hint_given = 1;
    }
}
#endif

/**
 * Stop the machine the way the signal asks. Does not return.
 *
 * Installed for the three signals busybox's `halt`, `poweroff` and `reboot`
 * send to pid 1, with busybox's own mapping: SIGUSR1 halts, SIGUSR2 powers off,
 * SIGTERM restarts.
 *
 * The work is done in the handler and not left to the loop through a flag, the
 * usual rule for a handler. That rule keeps a handler from doing what the
 * interrupted code was in the middle of; this one makes a single system call,
 * `reboot`, which is async-signal-safe and does not return when it succeeds,
 * so there is nothing to return to. A failed `reboot` falls to `_exit`, and
 * the kernel panics when init dies.
 *
 * The kernel disables scheduling and attempts to flush the file systems
 * before stopping. Shutdown continues if a flush fails. Init does not first
 * terminate user processes.
 */
static void shutdown_handler(int sig)
{
    int command = RB_AUTOBOOT;

    if (sig == SIGUSR1) {
        command = RB_HALT_SYSTEM;
    } else if (sig == SIGUSR2) {
        command = RB_POWER_OFF;
    }
    reboot(command);
    _exit(1);
}

/* Where a service's output goes once the console belongs to the person at it.
 *
 * The kernel stops writing to the screen when init starts, and a service that
 * kept the console would undo that from the other side: the DHCP client and
 * the SSH server would print over the login prompt. They write here instead,
 * and a person reads the file.
 *
 * It is bounded, not rotated: `bound_service_log` empties it in place when it
 * reaches the cap, which the services' append descriptors survive. Rotation
 * would keep the older records instead of dropping them, and needs a collector
 * that owns the writing, because renaming a file does not move the descriptors
 * running services already hold.
 *
 * Nor does it hold everything a service says. BusyBox's `udhcpc` is started
 * with `-b`, and once it has a lease it backgrounds itself and replaces its
 * own descriptors with /dev/null, so renewals and lease losses reach neither
 * this file nor the screen. What is here is what a service printed before it
 * backgrounded. Both limits are written down in utils/Readme.md.
 */
#define SERVICE_LOG             "/var/log/services"
#define SERVICE_LOG_CAP         (256 * 1024)
/* Turns of init's loop between two looks at the service log. An idle turn is a
   second, so this is about a minute on a quiet machine and less on a busy one. */
#define SERVICE_LOG_CHECK_TURNS 60

static int service_log_fd = -1;
static int service_null_fd = -1;

/**
 * Empty the log when it has reached its cap, leaving a line that says so.
 *
 * The services hold descriptors opened `O_APPEND`, so truncating the file to
 * nothing is enough: their next write lands at the start of it and nothing has
 * to be reopened or renamed. Rotation would need the opposite, a new file the
 * running services cannot be moved onto, and a collector that owns the
 * writing, which init does not have.
 *
 * What is lost is every record the log held. What is kept is a root that does
 * not fill because one service is repeating itself, and on the recovery system
 * that root is memory.
 */
static void bound_service_log(void)
{
    struct stat st;
    static const char note[] = "init: the service log reached its limit and was emptied\n";

    if (service_log_fd < 0 || fstat(service_log_fd, &st) != 0) {
        return;
    }
    if (st.st_size < SERVICE_LOG_CAP) {
        return;
    }
    if (ftruncate(service_log_fd, 0) == 0) {
        (void)write(service_log_fd, note, sizeof(note) - 1);
    }
}

/**
 * Open the service log, removing it first when the last boots left it at the
 * cap, and `/dev/null` as its fallback. A machine with no writable /var keeps
 * -1 and its services are silenced rather than allowed back onto the console,
 * since the console is the one thing this is protecting.
 *
 * `/dev/null` is opened read-write because it stands in for the log when there
 * is no log, and a service redirected onto a read-only descriptor would fail
 * every write rather than discard it. Both descriptors are close-on-exec: a
 * child gets them only through `service_output`.
 */
static void open_service_log(void)
{
    struct stat st;

    service_null_fd = open("/dev/null", O_RDWR);
    if (service_null_fd >= 0) {
        (void)fcntl(service_null_fd, F_SETFD, FD_CLOEXEC);
    }

    if (stat(SERVICE_LOG, &st) == 0 && st.st_size >= SERVICE_LOG_CAP) {
        (void)unlink(SERVICE_LOG);
    }
    service_log_fd = open(SERVICE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (service_log_fd >= 0) {
        (void)fcntl(service_log_fd, F_SETFD, FD_CLOEXEC);
    }
}

/** Return the service log descriptor, or the null descriptor as fallback. */
static int service_sink(void)
{
    return service_log_fd >= 0 ? service_log_fd : service_null_fd;
}

/**
 * Connect standard input to `/dev/null` and both output streams to the service
 * log, falling back to `/dev/null`. Exit the child if redirection fails.
 */
static void service_output(void)
{
    int sink = service_sink();

    if (service_null_fd < 0 || sink < 0 || dup2(service_null_fd, 0) < 0 || dup2(sink, 1) < 0 ||
        dup2(sink, 2) < 0) {
        _exit(126);
    }
}

#ifdef INIT_METAL
#define RECOVERY_KEYS "/root/.ssh/authorized_keys"

/**
 * Whether this image has anybody to let in over SSH.
 *
 * On the recovery build the server starts where root has an authorized key
 * and nowhere else, which makes a credential a property of the image rather
 * than a switch in the code. A stick built for development has the owner's
 * key and can update a machine remotely; one built for anybody else has none,
 * answers nothing, and is reachable only after a person puts a key on it. The
 * recovery partition of an installed disk is the same: it starts no server
 * unless its image has a key or the installer put one beside it, which
 * `take_disk_keys` adds to root's before this is first asked.
 *
 * Root's own file is the one that counts. `--update` and a repair are done as
 * root, and an image that let an unprivileged account in but had no root key
 * could not do either. The file must be a regular one with a line that is
 * neither blank nor a comment. What such a line says is for the server to
 * judge: an entry may begin with options (`no-port-forwarding
 * ssh-ed25519 ...`), and reading it more closely here would refuse entries
 * Dropbear accepts and admit lines it does not.
 *
 * The metal build always answers yes. An installed system's policy is its
 * own: the installer decides who may log in, puts the account's key in the
 * account's own directory and writes the SSH policy for root, and root's
 * directory is empty by design, so asking about root's keys would leave every
 * freshly installed machine with no server at all.
 */
static int anyone_to_let_in(void)
{
#ifndef INIT_RECOVERY
    return 1;
#else
    struct stat st;
    FILE *f;
    char line[512];
    int usable = 0;

    if (stat(RECOVERY_KEYS, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0;
    }
    f = fopen(RECOVERY_KEYS, "r");
    if (!f) {
        return 0;
    }
    while (!usable && fgets(line, sizeof(line), f)) {
        char *p = line;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '\0' && *p != '\n' && *p != '\r' && *p != '#') {
            usable = 1;
        }
    }
    fclose(f);
    return usable;
#endif
}
#endif /* INIT_METAL */

/**
 * Fork the console session and return its pid (-1 on failure): `/bin/login`
 * on the login builds, the first of `Shells` that executes on the others.
 *
 * The child does what getty does before exec: `setsid` makes it a new
 * session's leader (with no controlling terminal), then `ioctl(TIOCSCTTY)`
 * claims the console as that session's controlling terminal. The program thus
 * starts as a session leader that already owns the terminal, so a
 * job-control shell like busybox ash, which checks `tcgetpgrp` at startup,
 * finds itself the foreground. That is why init must never own the console:
 * it hands the console to each session it spawns, as a real init delegates a
 * terminal through getty.
 *
 * On the login builds the child sets `LOGIN_TIMEOUT=0` before the exec.
 * BusyBox's login, the `/bin/login` of the images that stage BusyBox,
 * otherwise gives up after 60 seconds at its prompt and exits, and the login
 * init starts next would draw a new prompt on an idle console every minute.
 * The os-simple image's own login has no timeout and does not read it.
 */
static pid_t spawn_shell(void)
{
    pid_t pid = fork();
#ifndef INIT_CONSOLE_LOGIN
    int i;
#endif

    if (pid != 0) {
        return pid; /* init (the parent), or -1 if fork failed */
    }

    setsid();                          /* new session; this child is its leader */
    ioctl(STDIN_FILENO, TIOCSCTTY, 0); /* claim the console as our ctty */
#ifdef INIT_CONSOLE_LOGIN
    {
        char *argv[2];
        argv[0] = (char *)"login";
        argv[1] = 0;
        (void)setenv("LOGIN_TIMEOUT", "0", 1);
        execve(LOGIN_PROGRAM, argv, environ); /* returns only on failure */
    }
    report_no_login();
    _exit(127); /* every failure is this one, ENOENT included */
#else
    for (i = 0; Shells[i] != 0; i++) {
        char *argv[2];
        argv[0] = (char *)Shell_Arg0[i];
        argv[1] = 0;
        execve(Shells[i], argv, environ); /* returns only on failure */
    }
    _exit(127); /* no login shell present in the image */
#endif
    return -1; /* unreachable; quiets the compiler */
}

#ifdef INIT_METAL
/**
 * Check every ext2 partition with `e2fsck -p` before anything runs from the
 * disk.
 *
 * An image without `/sbin/e2fsck` skips this, which leaves the recovery
 * system, whose root needs nothing from the disk, as the one that runs it.
 * `main` calls it only when the boot line has `rach2.fsck`, since a person who
 * booted recovery to look, or an installer that booted it to write, did not
 * ask for a repair. The partitions are the four MBR entries the kernel
 * publishes as nodes, and a partition is an ext2 one when byte 56 of its
 * superblock, which starts at byte 1024 of the partition, has the magic. A
 * fork that fails is retried up to five times a second apart, because a check
 * skipped is a repair not made.
 *
 * `e2fsck -p` repairs what needs no question and leaves the rest for a
 * person; its status is the record, on the console and so in the boot log: 0
 * clean, 1 repaired, 2 repaired and a reboot wanted, 4 left with faults, 8 an
 * error.
 */
static void check_ext2_partitions(void)
{
    static const char *const nodes[] = {"/dev/disk0p1", "/dev/disk0p2", "/dev/disk0p3",
                                        "/dev/disk0p4"};
    unsigned i;

    if (access("/sbin/e2fsck", X_OK) != 0) {
        return;
    }
    for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        unsigned char sector[512]; /* a raw window reads whole sectors */
        int fd = open(nodes[i], O_RDONLY);
        pid_t pid;
        int status = 0, tries;

        if (fd < 0) {
            continue;
        }
        if (lseek(fd, 1024, SEEK_SET) != 1024 ||
            read(fd, sector, sizeof(sector)) != (ssize_t)sizeof(sector) || sector[56] != 0x53 ||
            sector[57] != 0xEF) {
            close(fd);
            continue;
        }
        close(fd);
        for (tries = 0; (pid = fork()) < 0 && tries < 5; tries++) {
            sleep(1);
        }
        if (pid == 0) {
            char *argv[4];

            argv[0] = (char *)"e2fsck";
            argv[1] = (char *)"-p";
            argv[2] = (char *)nodes[i];
            argv[3] = 0;
            execve("/sbin/e2fsck", argv, environ);
            _exit(127);
        }
        if (pid < 0) {
            fprintf(stderr, "init: e2fsck: %s: fork failed\n", nodes[i]);
            continue;
        }
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                status = 8 << 8; /* not waited for: reported as e2fsck's own error */
                break;
            }
        }
        fprintf(stderr, "init: e2fsck: %s: status %d\n", nodes[i],
                WIFEXITED(status) ? WEXITSTATUS(status) : 8);
    }
}

/**
 * Whether the boot line has `want` as a whole token.
 *
 * A whole token and not a prefix: `rach2.ip=none` and `rach2.ip=none.x` are
 * different things. The line is read from `/proc/cmdline` each time, which
 * is twice in a boot.
 */
static int boot_line_has(const char *want)
{
    char line[512];
    int fd = open("/proc/cmdline", O_RDONLY);
    ssize_t n;
    size_t wlen = strlen(want);
    size_t i;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    for (i = 0; i + wlen <= (size_t)n; i++) {
        if ((i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t' || line[i - 1] == '\n') &&
            strncmp(&line[i], want, wlen) == 0) {
            char after = line[i + wlen];

            if (after == '\0' || after == ' ' || after == '\t' || after == '\n') {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * Whether the boot line handed the network to userland.
 *
 * `rach2.ip=none` means the kernel came up with the device probed and IPv4
 * unset because a program will set it. Any other boot line either named an
 * address or accepted the default, and in both cases the machine is already
 * configured: running a DHCP client over that would begin by unconfiguring the
 * address somebody put on the command line, and on a network with no server it
 * would never put one back.
 */
static int boot_wants_userland_network(void)
{
    return boot_line_has("rach2.ip=none");
}

#ifdef INIT_RECOVERY
/* The file `rach2-install` writes on the recovery partition, the directory the
 * partition is mounted on while it is read, and the most the file may hold. */
#define DISK_KEYS_FILE  "/boot/recovery_authorized_keys"
#define DISK_KEYS_MOUNT "/mnt/recovery-partition"
#define DISK_KEYS_MAX   16384
/* The kernel's own limit on disks, DISK_MAX in rach2/sys/block/disk.h. */
#define DISK_NODES      8
/* The recovery partition's label as its FAT32 boot sector holds it, at byte
 * 71: tools/image/mkdisk_grub.sh gives it, and the boot loader's core finds
 * the partition by it when the boot partition does not read. */
#define RECOVERY_LABEL  "RACH2RECOV "

/** The first `rach2.boot=` token of `line` from `from` on, or NULL. */
static char *boot_token(char *line, char *from)
{
    static const char key[] = "rach2.boot=";
    char *at;

    for (at = strstr(from, key); at != NULL; at = strstr(at + 1, key)) {
        if (at == line || at[-1] == ' ' || at[-1] == '\t' || at[-1] == '\n') {
            return at;
        }
    }
    return NULL;
}

/**
 * Read the disk identity the boot line's `rach2.boot=MBRID=XXXXXXXX,1`
 * names into `*id`.
 *
 * Returns 1 when the first `rach2.boot=` token is exactly that, eight
 * hexadecimal digits and `,1`, the boot partition of the disk's own menu,
 * and no second token follows; 0 otherwise. The kernel boots from the first
 * token, and a key taken from a disk another token names would not be the
 * disk this recovery system came from.
 */
static int boot_disk_id(unsigned long *id)
{
    static const char want[] = "rach2.boot=MBRID=";
    char line[512];
    int fd = open("/proc/cmdline", O_RDONLY);
    ssize_t n;
    char *at;
    int i;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    at = boot_token(line, line);
    if (at == NULL || strncmp(at, want, sizeof(want) - 1) != 0 ||
        boot_token(line, at + 1) != NULL) {
        return 0;
    }
    at += sizeof(want) - 1;
    *id = 0;
    for (i = 0; i < 8; i++) {
        int c = at[i];
        int v;

        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            return 0;
        }
        *id = (*id << 4) | (unsigned long)v;
    }
    return at[8] == ',' && at[9] == '1' &&
           (at[10] == '\0' || at[10] == ' ' || at[10] == '\t' || at[10] == '\n');
}

/**
 * Return the number of the one disk whose MBR has identity `id`, -1 when no
 * disk has it and -2 when more than one does.
 *
 * The identity is the four bytes at offset 440 of sector 0, the field the
 * kernel resolves `rach2.boot=` by. A clone of the disk beside it makes the
 * answer ambiguous, and an ambiguous disk gives no key.
 */
static int disk_with_id(unsigned long id)
{
    int found = -1;
    int n;

    for (n = 0; n < DISK_NODES; n++) {
        char node[16];
        unsigned char sector[512];
        int fd;
        ssize_t got;
        unsigned long on_disk;

        snprintf(node, sizeof(node), "/dev/disk%d", n);
        fd = open(node, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        got = read(fd, sector, sizeof(sector));
        close(fd);
        if (got != (ssize_t)sizeof(sector) || sector[510] != 0x55 || sector[511] != 0xAA) {
            continue;
        }
        on_disk = (unsigned long)sector[440] | (unsigned long)sector[441] << 8 |
                  (unsigned long)sector[442] << 16 | (unsigned long)sector[443] << 24;
        if (on_disk != id) {
            continue;
        }
        if (found >= 0) {
            return -2;
        }
        found = n;
    }
    return found;
}

/**
 * Whether the partition node `node` is a recovery partition by its label.
 *
 * The sector is read raw, before anything is mounted, so init never mounts
 * the boot partition or the root, however damaged they are.
 */
static int is_recovery_partition(const char *node)
{
    unsigned char sector[512];
    int fd = open(node, O_RDONLY);
    ssize_t got;

    if (fd < 0) {
        return 0;
    }
    got = read(fd, sector, sizeof(sector));
    close(fd);
    return got == (ssize_t)sizeof(sector) && sector[510] == 0x55 && sector[511] == 0xAA &&
           memcmp(sector + 71, RECOVERY_LABEL, 11) == 0;
}

/**
 * Why the `len` bytes of `keys`, which end in a NUL, will not do as keys, or
 * NULL when they will: they are not text, they hold a private key, or no line
 * of them is neither blank nor a comment.
 */
static const char *keys_fault(const char *keys, long len)
{
    long i;
    int line_start = 1;

    if ((long)strlen(keys) != len) {
        return "the key file is not text";
    }
    if (strstr(keys, "PRIVATE KEY") != NULL) {
        return "the key file holds a private key";
    }
    for (i = 0; i < len; i++) {
        char c = keys[i];

        if (c == '\n') {
            line_start = 1;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            continue;
        } else if (line_start && c != '#') {
            return NULL;
        } else {
            line_start = 0;
        }
    }
    return "no line of the key file is a key";
}

/**
 * Read the key file under the mounted partition into `keys`, which holds
 * `DISK_KEYS_MAX + 1` bytes, and return its length.
 *
 * Returns -1 with `*why` set when the file is there and is refused: not a
 * regular file, larger than `DISK_KEYS_MAX`, or refused by `keys_fault`.
 * Returns 0 with `*why` NULL when there is no file, which is a disk installed
 * without a key.
 */
static long read_disk_keys(char *keys, const char **why)
{
    struct stat st;
    long len = 0;
    int fd;

    *why = NULL;
    fd = open(DISK_KEYS_MOUNT DISK_KEYS_FILE, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT) {
            *why = "the key file cannot be opened";
            return -1;
        }
        return 0;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *why = "the key file is not a regular file";
        return -1;
    }
    for (;;) {
        ssize_t got = read(fd, keys + len, (size_t)(DISK_KEYS_MAX + 1 - len));

        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            close(fd);
            *why = "the key file cannot be read";
            return -1;
        }
        if (got == 0 || len + got > DISK_KEYS_MAX) {
            len += got;
            break;
        }
        len += got;
    }
    close(fd);
    if (len > DISK_KEYS_MAX) {
        *why = "the key file is larger than 16384 bytes";
        return -1;
    }
    keys[len] = '\0';
    *why = keys_fault(keys, len);
    return *why != NULL ? -1 : len;
}

/**
 * Append `len` bytes of keys to root's `authorized_keys`, after a newline if
 * the file there does not end with one. Returns 0 on success, -1 otherwise.
 */
static int append_root_keys(const char *keys, long len)
{
    char last = '\n';
    off_t end;
    long done = 0;
    int fd;

    (void)mkdir("/root/.ssh", 0700);
    fd = open(RECOVERY_KEYS, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        return -1;
    }
    end = lseek(fd, 0, SEEK_END);
    if (end < 0 || (end > 0 && (lseek(fd, end - 1, SEEK_SET) != end - 1 ||
                                read(fd, &last, 1) != 1 || lseek(fd, 0, SEEK_END) != end))) {
        close(fd);
        return -1;
    }
    if (last != '\n' && write(fd, "\n", 1) != 1) {
        close(fd);
        return -1;
    }
    while (done < len) {
        ssize_t put = write(fd, keys + done, (size_t)(len - done));

        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            close(fd);
            return -1;
        }
        done += put;
    }
    if (keys[len - 1] != '\n' && write(fd, "\n", 1) != 1) {
        close(fd);
        return -1;
    }
    return close(fd);
}

/**
 * Add the key the installer put on this disk's recovery partition to root's
 * keys. Returns 1 when a key was added and 0 otherwise.
 *
 * `rach2-install` writes `DISK_KEYS_FILE` on the recovery partition when the
 * person installing gives it a key, so that a machine installed from a stick
 * with no key can still be updated over the network: a root update runs from
 * recovery. Only a boot from that partition reads it, which the recovery
 * entries of the disk's own menu say with `rach2.slot=recovery`. The stick's
 * recovery system keeps the rule that its image decides, so a stick built
 * for nobody answers nobody wherever it is booted. The disk is the one
 * `rach2.boot=` names by its MBR identity, and the partition is its second,
 * where the installed layout keeps recovery, and only when its FAT32 boot
 * sector has the recovery label and it holds `/boot/recovery`, the file the
 * installer finds the partition by; the boot partition and the root are
 * never mounted, whatever their labels say.
 *
 * The partition is mounted read-only and unmounted as soon as the file is
 * read, before anything else runs, so a boot changes nothing on the disk
 * and leaves no mount on it for `reflash` or the installer to refuse. The
 * file is refused, and said to be, for the reasons `read_disk_keys` gives;
 * what its lines say beyond that is the server's to judge, as in
 * `anyone_to_let_in`.
 */
static int take_disk_keys(void)
{
    static char keys[DISK_KEYS_MAX + 1];
    unsigned long id;
    char node[24];
    const char *why;
    long len;
    int added = 0;
    int disk;

    if (!boot_line_has("rach2.slot=recovery") || !boot_disk_id(&id)) {
        return 0;
    }
    disk = disk_with_id(id);
    if (disk == -2) {
        fprintf(stderr, "init: recovery key: two disks have identity %08lx; no key taken\n", id);
        return 0;
    }
    if (disk < 0) {
        fprintf(stderr, "init: recovery key: no disk has identity %08lx\n", id);
        return 0;
    }
    (void)mkdir("/mnt", 0755);
    if (mkdir(DISK_KEYS_MOUNT, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "init: recovery key: %s cannot be made\n", DISK_KEYS_MOUNT);
        return 0;
    }
    snprintf(node, sizeof(node), "/dev/disk%dp2", disk);
    if (!is_recovery_partition(node) ||
        mount(node, DISK_KEYS_MOUNT, "vfat", MS_RDONLY, "umask=077") != 0) {
        (void)rmdir(DISK_KEYS_MOUNT);
        fprintf(stderr, "init: recovery key: /dev/disk%d has no recovery partition\n", disk);
        return 0;
    }
    if (access(DISK_KEYS_MOUNT "/boot/recovery", F_OK) != 0) {
        if (umount(DISK_KEYS_MOUNT) != 0) {
            fprintf(stderr, "init: recovery key: %s: would not unmount\n", node);
            return 0;
        }
        (void)rmdir(DISK_KEYS_MOUNT);
        fprintf(stderr, "init: recovery key: /dev/disk%d has no recovery partition\n", disk);
        return 0;
    }
    len = read_disk_keys(keys, &why);
    if (len < 0) {
        fprintf(stderr, "init: recovery key: %s: refused: %s\n", node, why);
    } else if (len > 0 && append_root_keys(keys, len) != 0) {
        fprintf(stderr, "init: recovery key: %s: cannot be written\n", RECOVERY_KEYS);
    } else if (len > 0) {
        added = 1;
    }
    if (umount(DISK_KEYS_MOUNT) != 0) {
        fprintf(stderr, "init: recovery key: %s: would not unmount\n", node);
        return added;
    }
    (void)rmdir(DISK_KEYS_MOUNT);
    if (added) {
        printf("init: recovery key: taken from %s\n", node);
    }
    return added;
}

/**
 * Take the recovery partition's key and say on the console whether SSH is on,
 * and why. A recovery image built to work on and one built for anybody else
 * are identical from outside, so the machine says which it is, and whether
 * the disk it booted from gave it a key.
 */
static void report_recovery_keys(void)
{
    int built_in = anyone_to_let_in();
    int from_disk = take_disk_keys();

    if (built_in) {
        printf("init: a key was built into this recovery image, so SSH is on\n");
    } else if (from_disk) {
        printf("init: no key was built into this recovery image, and its recovery "
               "partition's key turns SSH on\n");
    } else {
        printf("init: no key was built into this recovery image, so SSH is off\n");
    }
}
#endif

/**
 * Configure the network, once, before the SSH server is started.
 *
 * A machine across the room is reached over the network, and on a machine that
 * is carried between two buildings the address is not something the boot line
 * can name: it comes from whatever server is on the wire there. So init runs
 * `ifup -a`, which reads `/etc/network/interfaces` and does whatever it says,
 * a lease or a fixed address.
 *
 * It runs after the console login has been started and before the SSH server,
 * which is the order that matters: a network that fails to come up must not
 * take the local way in with it, and a server started before there is an
 * address would be listening on one it is about to lose.
 *
 * It runs only where the boot line said userland owns the address. A machine
 * told its address on the command line is configured already, and a DHCP
 * client would begin by taking that address off.
 *
 * The result is reported and not acted on. There is nothing useful to do with
 * a failure here: the machine is up, the console works, and a later `ifup` by
 * hand is the repair.
 *
 * `/var/run/ifstate`, where `ifup` records the interfaces it has brought up,
 * is removed first: on a disk root the previous boot's record survives, and
 * `ifup -a` would refuse eth0 as already configured and bring up nothing.
 */
static void configure_network(void)
{
    pid_t pid;
    int status = 0;

    unlink("/var/run/ifstate");
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "init: network: cannot fork ifup\n");
        return;
    }
    if (pid == 0) {
        char *argv[3];

        argv[0] = (char *)"ifup";
        argv[1] = (char *)"-a";
        argv[2] = NULL;
        execv("/bin/ifup", argv);
        execv("/bin/busybox", argv); /* the applet, where there is no /bin/ifup */
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status)) {
            fprintf(stderr,
                    "init: network: ifup -a did not configure the interface (status %d%s)\n",
                    WEXITSTATUS(status), WEXITSTATUS(status) == 127 ? ", not executed" : "");
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "init: network: ifup -a was killed by signal %d\n", WTERMSIG(status));
        } else {
            fprintf(stderr, "init: network: ifup -a neither exited nor was killed (0x%x)\n",
                    (unsigned)status);
        }
    }
}

/**
 * Configure networking in a redirected child, then wait for it.
 * A failed fork skips configuration. Redirection failure exits only the child;
 * init keeps the console descriptors used by later login sessions.
 * Block shutdown signals across fork until the child has default dispositions,
 * so a signal to the worker terminates it without invoking init's handler.
 */
static void start_network(void)
{
    sigset_t shutdown, saved;
    struct sigaction action;
    pid_t network;

    sigemptyset(&shutdown);
    sigaddset(&shutdown, SIGUSR1);
    sigaddset(&shutdown, SIGUSR2);
    sigaddset(&shutdown, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &shutdown, &saved) != 0) {
        return;
    }
    network = fork();

    if (network == 0) {
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGUSR1, &action, NULL) != 0 || sigaction(SIGUSR2, &action, NULL) != 0 ||
            sigaction(SIGTERM, &action, NULL) != 0 || sigprocmask(SIG_SETMASK, &saved, NULL) != 0) {
            _exit(126);
        }
        service_output();
        configure_network();
        _exit(0);
    }
    (void)sigprocmask(SIG_SETMASK, &saved, NULL);
    if (network > 0) {
        while (waitpid(network, NULL, 0) < 0 && errno == EINTR) {
        }
    }
}

/**
 * Return 1 when `/etc/rach2/sshd.conf` has a line containing exactly
 * `root-password-login yes`, apart from trailing whitespace. Return 0 if the
 * file is unavailable, cannot be read, or contains no such line.
 */
static int root_password_login(void)
{
    FILE *f = fopen("/etc/rach2/sshd.conf", "r");
    static const char setting[] = "root-password-login yes";
    size_t matched = 0;
    int c, bad = 0, yes = 0;

    if (!f) {
        return 0;
    }
    for (;;) {
        c = fgetc(f);
        if (c == '\n' || c == EOF) {
            if (!bad && matched == sizeof(setting) - 1) {
                yes = 1;
            }
            if (c == EOF) {
                break;
            }
            matched = 0;
            bad = 0;
        } else if (matched < sizeof(setting) - 1) {
            if (c == setting[matched]) {
                matched++;
            } else {
                bad = 1;
            }
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\v' && c != '\f') {
            bad = 1;
        }
    }
    if (ferror(f)) {
        yes = 0;
    }
    fclose(f);
    return yes;
}

/**
 * Fill `argv`, which has room for eight pointers, with the SSH server's
 * command line, and return the program to execute: OpenSSH's when `openssh`
 * is set and Dropbear's when it is not, with root's password barred unless
 * `password_login` is set.
 */
static const char *sshd_command(int openssh, int password_login, char **argv)
{
    int n = 0;

    if (openssh) {
        argv[n++] = (char *)OPENSSH_PROGRAM;
        argv[n++] = (char *)"-D";
        argv[n++] = (char *)"-e";
        argv[n++] = (char *)"-o";
        argv[n++] = (char *)(password_login ? "PermitRootLogin=yes"
                                            : "PermitRootLogin=prohibit-password");
        argv[n] = 0;
        return OPENSSH_PROGRAM;
    }
    argv[n++] = (char *)"dropbearmulti";
    argv[n++] = (char *)"dropbear";
    argv[n++] = (char *)"-F";
    argv[n++] = (char *)"-R";
    if (!password_login) {
        argv[n++] = (char *)"-g";
    }
    argv[n++] = (char *)"-p";
    argv[n++] = (char *)"22";
    argv[n] = 0;
    return SSHD_PROGRAM;
}

/**
 * Make the host keys OpenSSH's server lacks with `ssh-keygen -A`, and wait
 * for it.
 *
 * It runs in the server's child after the redirection, so what it prints goes
 * to the service log and init's own loop goes on reaping while a key is made,
 * which takes seconds on an emulated machine. A key it could not make is
 * reported by the server, which does not start without one.
 */
static void make_host_keys(void)
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        char *argv[3];

        argv[0] = (char *)"ssh-keygen";
        argv[1] = (char *)"-A";
        argv[2] = 0;
        execve(OPENSSH_KEYGEN, argv, environ);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

/**
 * Fork the SSH server and return its pid (-1 on failure).
 *
 * Unlike the console session this child does NOT call `setsid` or claim a
 * controlling terminal. The server makes a session per login and gives each
 * one a pty of its own; handing it the console as well would put two sessions
 * on one terminal, and the console login is the recovery path precisely
 * because nothing else is on it.
 */
static pid_t spawn_sshd(void)
{
    int password_login = root_password_login();
    int openssh = access(OPENSSH_PROGRAM, X_OK) == 0;
    pid_t pid = fork();

    if (pid != 0) {
        return pid; /* init (the parent), or -1 if fork failed */
    }
    service_output();
    {
        static const char msg[] = "init: cannot exec ";
        char *argv[8];
        const char *program = sshd_command(openssh, password_login, argv);

        if (openssh) {
            make_host_keys();
        }
        execve(program, argv, environ); /* returns only on failure */
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)write(STDERR_FILENO, program, strlen(program));
        (void)write(STDERR_FILENO, "\n", 1);
    }
    _exit(127);
    return -1; /* unreachable; quiets the compiler */
}
#endif

/**
 * Empty /var/run.
 *
 * What is in it describes the machine as it was running, not as it is: `ifup`
 * records there which interfaces it has configured, and a DHCP client leaves
 * its pid. On a machine whose /var/run survives a reboot, the next `ifup -a`
 * reads the last boot's record, decides the interface is already configured,
 * and takes no lease at all, while a stale pid file names a process that is
 * either gone or somebody else. Linux keeps this directory in memory for the
 * same reason. This file system has nowhere to put it that a reboot clears, so
 * init clears it, first, before anything reads it.
 *
 * It removes one level: each entry is unlinked, or removed if it is an empty
 * directory, and a directory that is not empty is reported, not descended
 * into. A name holding a slash is refused, since a name read from a damaged
 * file system would otherwise be joined into a path outside this directory,
 * and pid 1 would delete a file elsewhere at every boot.
 */
static void clear_var_run(void)
{
    DIR *d = opendir("/var/run");
    struct dirent *e;

    if (!d) {
        return; /* an image without it has nothing to clear */
    }
    while ((e = readdir(d)) != NULL) {
        char path[NAME_MAX + 16];
        const char *n = e->d_name;
        size_t i;

        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) {
            continue;
        }
        for (i = 0; n[i]; i++) {
            if (n[i] == '/') {
                break;
            }
        }
        if (n[i] == '/') {
            fprintf(stderr, "init: /var/run: refusing a name that is not one: %s\n", n);
            continue;
        }
        if (snprintf(path, sizeof(path), "/var/run/%s", n) >= (int)sizeof(path)) {
            fprintf(stderr, "init: /var/run: name too long to remove: %s\n", n);
            continue;
        }
        if (unlink(path) != 0 && rmdir(path) != 0) {
            fprintf(stderr, "init: /var/run: cannot remove %s\n", n);
        }
    }
    closedir(d);
}

/**
 * Start the periodic sync (`/bin/update`), which sleeps thirty seconds and
 * calls sync(2) for ever, so written data reaches the disk without anyone
 * asking. Returns its pid, or -1 when the fork failed.
 */
static pid_t spawn_update(void)
{
    pid_t pid = fork();

    if (pid != 0) {
        return pid;
    }
    service_output();
    {
        char *argv[2];

        argv[0] = (char *)"update";
        argv[1] = 0;
        execve("/bin/update", argv, environ);
        _exit(127);
    }
}

/**
 * One limit value as the file writes it: a decimal number, or `unlimited`.
 * A word that is not one of those, a negative number, or one past what the
 * type holds is `LIMIT_BAD`.
 */
#define LIMIT_BAD ((rlim_t) - 2)
static rlim_t limit_value(const char *word)
{
    char *end;
    unsigned long long v;

    if (strcmp(word, "unlimited") == 0) {
        return RLIM_INFINITY;
    }
    if (word[0] == '-' || word[0] == '+') {
        return LIMIT_BAD;
    }
    errno = 0;
    v = strtoull(word, &end, 10);
    if (*end != '\0' || end == word || errno == ERANGE || v >= (unsigned long long)RLIM_INFINITY) {
        return LIMIT_BAD;
    }
    return (rlim_t)v;
}

/**
 * The resource limits every session starts with, from `/etc/rach2/limits`:
 * one line per limit, `nproc`, `nofile` or `as`, then the soft and the hard
 * value, a number or `unlimited`. They are set on init itself, and fork and
 * exec hand them down to every login and everything it starts, root's
 * included, and root may raise them. A line that does not parse is reported
 * and skipped; a missing file leaves the kernel's own defaults, which are
 * the table's limits.
 */
static void apply_limits(void)
{
    FILE *f = fopen("/etc/rach2/limits", "r");
    char line[128];
    int lineno = 0;

    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[16], soft[32], hard[32];
        struct rlimit rl;
        int resource;

        lineno++;
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (sscanf(line, "%15s %31s %31s", name, soft, hard) != 3) {
            fprintf(stderr, "init: limits: line %d: not a name and two values\n", lineno);
            continue;
        }
        if (strcmp(name, "nproc") == 0) {
            resource = RLIMIT_NPROC;
        } else if (strcmp(name, "nofile") == 0) {
            resource = RLIMIT_NOFILE;
        } else if (strcmp(name, "as") == 0) {
            resource = RLIMIT_AS;
        } else {
            fprintf(stderr, "init: limits: line %d: %s is not a limit\n", lineno, name);
            continue;
        }
        rl.rlim_cur = limit_value(soft);
        rl.rlim_max = limit_value(hard);
        if (rl.rlim_cur == LIMIT_BAD || rl.rlim_max == LIMIT_BAD) {
            fprintf(stderr, "init: limits: line %d: a value is not a number\n", lineno);
            continue;
        }
        if (setrlimit(resource, &rl) != 0) {
            fprintf(stderr, "init: limits: line %d: %s refused: %s\n", lineno, name,
                    strerror(errno));
        }
    }
    fclose(f);
}

/**
 * Set the machine up, then reap, respawn and supervise for as long as it runs.
 *
 * The three shutdown handlers go in before the first child, so a request that
 * arrives early is answered and does not meet a terminating default, which
 * for pid 1 is a panic. Then `/var/run` is emptied, the limits set and the
 * service log opened. The recovery build takes the key the installer put on
 * the recovery partition it booted from, and says on the console whether a
 * key was built into its image and whether the partition's turned SSH on,
 * since the two kinds of image look identical from outside. A metal boot
 * whose line has `rach2.fsck` then checks its ext2 partitions.
 *
 * Each turn respawns what is missing: the console session, whose failed fork
 * (a full process table) is retried after a second so the loop does not spin;
 * `/bin/update`; and on the metal builds the network, once, after the console
 * session and before the SSH server that needs it, then the server wherever
 * `anyone_to_let_in` allows one. A failed fork of the server or of `update` is
 * left for the next turn, so the loop keeps reaping the orphans that filled
 * the process table.
 *
 * The wait is a poll, `waitpid` with `WNOHANG` and a second's sleep when no
 * child has changed, because init must also bound the service log while its
 * children live, which a poll does without a timer. A death is noticed within
 * that second. The log is looked at every `SERVICE_LOG_CHECK_TURNS` turns,
 * busy or idle, since a machine whose children keep dying never takes the idle
 * path. `WUNTRACED` reports a stopped child, and init continues it at once:
 * `^Z` at the console prompt stops the session, and there is no shell above it
 * to type `fg`.
 *
 * A console session or SSH server that dies within two seconds of its start
 * has a permanent reason (a missing or crashing binary, no entropy for the
 * host key, no network, a port already taken), and is respawned only after a
 * pause, so the loop does not spin; an interactive session outlives that
 * window, so an ordinary `exit` brings the prompt back at once.
 */
int main(void)
{
    pid_t shell = -1;
    pid_t updater = -1;
    time_t spawned = 0;
#ifdef INIT_METAL
    pid_t sshd = -1;
    time_t sshd_spawned = 0;
    int network_done = 0;
#endif
    unsigned turns = 0;

    signal(SIGUSR1, shutdown_handler);
    signal(SIGUSR2, shutdown_handler);
    signal(SIGTERM, shutdown_handler);

    clear_var_run();
    apply_limits();
    open_service_log();
#ifdef INIT_RECOVERY
    report_recovery_keys();
#endif
#ifdef INIT_METAL
    if (boot_line_has("rach2.fsck")) {
        check_ext2_partitions();
    }
#endif

    for (;;) {
        int status;
        pid_t reaped;

        if (shell < 0) {
            shell = spawn_shell();
            if (shell < 0) {
                sleep(1);
                continue;
            }
            spawned = time(0);
        }
        if (updater < 0) {
            updater = spawn_update(); /* a failed fork is retried next round */
        }

#ifdef INIT_METAL
        if (!network_done) {
            network_done = 1;
            if (boot_wants_userland_network()) {
                start_network();
            }
        }

        if (sshd < 0 && anyone_to_let_in()) {
            sshd = spawn_sshd();
            if (sshd >= 0) {
                sshd_spawned = time(0);
                printf("init: sshd: started as pid %ld\n", (long)sshd);
                fflush(stdout);
            }
        }
#endif

        if (++turns >= SERVICE_LOG_CHECK_TURNS) {
            turns = 0;
            bound_service_log();
        }
        reaped = waitpid(-1, &status, WUNTRACED | WNOHANG);
        if (reaped == 0) {
            sleep(1);
            continue;
        }
        if (reaped < 0 && errno == EINTR) {
            continue;
        }
        if (reaped > 0 && WIFSTOPPED(status)) {
            kill(reaped, SIGCONT);
            continue;
        }
        if (reaped > 0 && reaped == updater) {
            updater = -1; /* respawned next round, after a pause below */
            sleep(1);
            continue;
        }
#ifdef INIT_METAL
        /* -1 is both a failed wait and the pid of a server not running. */
        if (reaped > 0 && reaped == sshd) {
            sshd = -1;
            if (time(0) - sshd_spawned < 2) {
                sleep(SSHD_RESPAWN_PAUSE);
            }
            continue;
        }
#endif
        if (reaped > 0 && reaped == shell) {
            shell = -1;
#ifdef INIT_CONSOLE_LOGIN
            session_ended(status, time(0) - spawned);
#endif
            if (time(0) - spawned < 2) {
#ifdef INIT_CONSOLE_LOGIN
                sleep(LOGIN_RESPAWN_PAUSE);
#else
                sleep(1);
#endif
            }
        }
    }
    return 0; /* unreachable */
}
