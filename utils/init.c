/**
 * Rach2 PID 1: the reaper of orphans and the resurrector of the login shell.
 *
 * The kernel starts this as the first user process, so it is init, and it
 * reparents every orphaned process onto it. Its whole job is three things a
 * system needs forever: reap whatever children land on it (the console session
 * and every orphan the kernel hands over), respawn that session when it exits,
 * so the console is never left without one, and continue it if it is ever
 * stopped, since nothing else can. It never exits; if it did, the kernel would
 * have no reaper, so the kernel panics if init dies.
 *
 * Three artifacts are built from this one source, and none of them is named
 * plain `init`: `init-shell` runs a shell on the console, `init-login` puts
 * that console behind `/bin/login`, and `init-metal` is `init-login` plus a
 * supervised SSH server. The metal build exists because the machine it is for
 * is across the room: nobody is going to type a command at its console to start
 * a daemon, so the daemon has to be running when the machine finishes booting.
 * The console login stays exactly as it is beside it, and is the way back in
 * when the network is the thing that is broken.
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
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

extern char **environ;

#ifdef INIT_CONSOLE_LOGIN
/* The one program this build may exec. There is no list and no fallback: a
 * fall-through to a shell on any exec failure hands the console to whoever is
 * standing at it, and narrowing the fall-through to ENOENT inverts the danger
 * instead of removing it, since ENOENT is exactly the case of an image built
 * without /bin/login, which is the image that must refuse hardest. So the
 * fallback is not narrowed, it is compiled out: this artifact has no code
 * path and no string that names a shell. */
#define LOGIN_PROGRAM "/bin/login"

/* The pause before a failed exec is tried again. Longer than the shell build's,
 * so a permanent failure states its reason on the console instead of scrolling
 * it away. */
#define LOGIN_RESPAWN_PAUSE 5
#else
/* The login shell, tried in order: /bin/sh then /ish. Neither shipped image
 * boots this build any more, since both put the console behind a login; it is
 * kept for an image that wants a shell without one, and for the contrast the
 * two artifacts draw. The matching argv[0] has a leading '-' so the shell
 * runs as a login shell and reads /etc/profile. */
static const char *const Shells[] = {"/bin/sh", "/ish", 0};
static const char *const Shell_Arg0[] = {"-sh", "-ish", 0};
#endif

#ifdef INIT_METAL
/*
 * The SSH server this build supervises, and how it is asked to run.
 *
 * `dropbearmulti` dispatches on its first argument, so the program name is
 * argv[1] and the flags follow. `-F` keeps it in the foreground, which is what
 * makes it supervisable at all: without it dropbear forks and the parent
 * exits, and init would see that exit as a death and respawn it forever
 * against a daemon that is in fact running. `-R` mints a host key per boot,
 * which needs the kernel's entropy to be trusted, so a machine whose CPU
 * offers neither RDSEED nor RDRAND will not get an SSH server (and will still
 * get its console login, which is the point of keeping both). `-g` bars root's
 * password from the exchange, so root arrives by key, which is the policy the
 * metal image is built around.
 */
#define SSHD_PROGRAM "/bin/dropbearmulti"

/* The pause after an SSH server that died almost at once. Long enough that a
 * server failing for a permanent reason (no entropy, no network, a port
 * already taken) does not respawn at full speed, and short enough that a
 * machine reachable only over the network is reachable again quickly. */
#define SSHD_RESPAWN_PAUSE 5
#endif

#ifdef INIT_CONSOLE_LOGIN
/**
 * Say on the console that the login program could not be run, and how to get
 * back in. Returns nothing.
 *
 * Written with `write`, not `printf` because this runs in a child that
 * has just failed an `execve`, and the only thing it can still be sure of is a
 * descriptor. The way back in is stated, not assumed: `rach2.rescue`
 * boots the kernel debug shell with the filesystem mounted and writable, and it
 * is the only way back in, since nothing starts a network service at boot.
 */
static void report_no_login(void)
{
    static const char first[] = "init: cannot exec /bin/login\n";
    static const char second[] = "init: boot rach2.rescue for the kernel debug shell\n";
    static int said = 0;

    (void)write(STDERR_FILENO, first, sizeof(first) - 1);
    if (!said) {
        (void)write(STDERR_FILENO, second, sizeof(second) - 1);
        said = 1;
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
 * Doing the work in the handler is the decision here, and it is the opposite of
 * the usual rule that a handler records a flag for the loop to act on. That
 * rule exists because a handler must not do what the interrupted code was in
 * the middle of; this one does a single bare system call that never returns,
 * which is async-signal-safe by construction and cannot be re-entered because
 * there is nothing to return to. The flag version costs a race for nothing:
 * init blocks in `waitpid`, and a signal arriving after the loop tested the
 * flag but before the kernel blocked it would set a flag nothing reads until
 * the next child event, which on an idle console is never. That window cannot
 * be closed here, since closing it needs a wait that unmasks atomically and
 * this kernel's `ppoll` takes a signal mask without applying it.
 *
 * Nothing is torn down first. The kernel flushes the page cache and clears the
 * dirty-mount flag inside the call, with scheduling already off, so no
 * committed write is lost; what is given up is the chance to ask the programs
 * on the machine to stop politely, which would mean a scheduling policy of
 * init's own invention at the last moment of the machine's life.
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

/**
 * Fork the login shell into its own session and return its pid (-1 on failure).
 *
 * The child does the getty dance before exec: `setsid` makes it a new
 * session's leader (with no controlling terminal), then `ioctl(TIOCSCTTY)`
 * claims the console as that session's controlling terminal. The shell thus
 * starts as a session leader that already owns the terminal, so a
 * job-control shell like busybox ash, which checks `tcgetpgrp` at startup,
 * finds itself the foreground. That is why init must never own the console:
 * it hands a fresh one to each shell it spawns, exactly as a real init
 * delegates a terminal through getty.
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
 * Check every ext2 partition before anything runs from the disk, on the
 * recovery system, which is the system that carries `/sbin/e2fsck` and
 * whose root needs nothing from the disk; an image without the program
 * skips this, and so does a boot whose line lacks `rach2.fsck`: a person
 * who booted recovery to look, or an installer that booted it to write,
 * did not ask for a repair. The partitions are the four MBR entries the
 * kernel publishes as nodes. A partition is an ext2 one by the magic at byte
 * 1080 of it. A fork that fails is retried a few times a second apart,
 * as the shell's is, because a check skipped is a repair not made.
 * `e2fsck -p` repairs what needs no question and leaves the rest for a
 * person; its status is the record, on the console and so in the boot
 * log: 0 clean, 1 repaired, 2 repaired and a reboot wanted, 4 left with
 * faults, 8 an error. The magic is at byte 56 of the superblock, which
 * starts at byte 1024 of the partition.
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
 */
static void configure_network(void)
{
    pid_t pid;
    int status = 0;

    /* What `ifup` recorded on the boot before this one. The file says which
     * interfaces are up now, and nothing of it survives a restart; on a
     * machine whose root is a disk it does survive, and `ifup -a` then
     * refuses eth0 as already configured and brings up nothing. */
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
        fprintf(stderr, "init: network: ifup -a did not configure the interface (status %d%s)\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                WIFEXITED(status) && WEXITSTATUS(status) == 127 ? ", not executed" : "");
    }
}

/**
 * Fork the SSH server and return its pid (-1 on failure).
 *
 * Unlike the console session this child does NOT call `setsid` or claim a
 * controlling terminal. Dropbear makes a session per login and gives each one
 * a pty of its own; handing it the console as well would put two sessions on
 * one terminal, and the console login is the recovery path precisely because
 * nothing else is on it.
 */
static pid_t spawn_sshd(void)
{
    pid_t pid = fork();

    if (pid != 0) {
        return pid; /* init (the parent), or -1 if fork failed */
    }
    {
        char *argv[8];
        argv[0] = (char *)"dropbearmulti";
        argv[1] = (char *)"dropbear";
        argv[2] = (char *)"-F";
        argv[3] = (char *)"-R";
        argv[4] = (char *)"-g";
        argv[5] = (char *)"-p";
        argv[6] = (char *)"22";
        argv[7] = 0;
        execve(SSHD_PROGRAM, argv, environ); /* returns only on failure */
    }
    {
        static const char msg[] = "init: cannot exec /bin/dropbearmulti\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    _exit(127);
    return -1; /* unreachable; quiets the compiler */
}
#endif

/*
 * Empty /var/run.
 *
 * What is in it describes the machine as it was running, not as it is: `ifup`
 * records there which interfaces it has configured, and a DHCP client leaves
 * its pid. On a machine whose /var/run survives a reboot, the next `ifup -a`
 * reads the last boot's record, decides the interface is already configured,
 * and takes no lease at all, while a stale pid file names a process that is
 * either gone or somebody else. Linux keeps this directory in memory for the
 * same reason. This file system has nowhere to put it that a reboot clears, so
 * init clears it, first, before anything reads it. What it removes is one
 * level of ordinary entries: nothing here makes a directory under it, and one
 * that appears is reported, not descended into.
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
        /* One component and nothing else. A name is read from a file system
         * that may have been damaged, and a stored name carrying a slash would
         * otherwise be joined into a path that leaves this directory: pid one
         * would then delete a file elsewhere on the machine at every boot. */
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
        /* A file first, then a directory: nothing here creates one, and one
         * that is there is reported, not left silently. */
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
#define LIMIT_BAD ((rlim_t)-2)
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

    /* The three shutdown requests, installed before the first child so a
     * request that arrives early is answered instead of defaulting to a
     * terminating disposition, which for pid 1 is a panic. */
    signal(SIGUSR1, shutdown_handler);
    signal(SIGUSR2, shutdown_handler);
    signal(SIGTERM, shutdown_handler);

    clear_var_run();
    apply_limits();
#ifdef INIT_METAL
    if (boot_line_has("rach2.fsck")) {
        check_ext2_partitions();
    }
#endif

    for (;;) {
        int status;
        pid_t reaped;

        /* Keep a login shell running. If the fork failed (a full process
         * table), back off a second before retrying, not spin: wait()
         * returns immediately with ECHILD while init has no children, so
         * retrying without a pause would peg the CPU. */
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
        /* The network comes up once, after the console login and before the
         * server that needs it. */
        if (!network_done) {
            network_done = 1;
            if (boot_wants_userland_network()) {
                configure_network();
            }
        }

        /* And keep an SSH server running beside it. A failed fork is left for
         * the next time round the loop, not retried here: the console
         * session above is the machine's other way in, and holding this loop
         * to retry a daemon would delay reaping the orphans that are the whole
         * reason a process table filled up. */
        if (sshd < 0) {
            sshd = spawn_sshd();
            if (sshd >= 0) {
                sshd_spawned = time(0);
            }
        }
#endif

        /* The shell is a live child, so this blocks until it or a reparented
         * orphan dies. Reap it; only the login shell's death triggers a
         * respawn.
         *
         * WUNTRACED, because a stop is not a death and init is the only thing
         * that can undo one here: `^Z` at a console prompt stops the child, and
         * there is no shell above it to type `fg`, so a plain `wait` would
         * simply go on blocking and the console would be gone until the next
         * reboot. `SIGSTOP` cannot be caught at all, so no amount of care in
         * the child covers this; the recovery has to live in the parent. */
        reaped = waitpid(-1, &status, WUNTRACED);
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
        /* `reaped > 0` and not just the pid comparison: `waitpid` reports a
         * failure as -1, and while no child is running the pid held here is
         * -1 too, so the bare comparison would read an interrupted wait as
         * the death of a server that was not running. */
        if (reaped > 0 && reaped == sshd) {
            sshd = -1;
            /* A server that died almost as soon as it started has a permanent
             * reason: no trusted entropy for the host key, no network, or a
             * port already held. Pause so it does not respawn at full speed,
             * and let the console session carry on regardless. */
            if (time(0) - sshd_spawned < 2) {
                sleep(SSHD_RESPAWN_PAUSE);
            }
            continue;
        }
#endif
        if (reaped > 0 && reaped == shell) {
            shell = -1;
            /* A shell that dies almost as soon as it starts (a missing or
             * crash-looping binary in a misbuilt image) would otherwise be
             * reforked at full CPU. Pause before respawning so the loop cannot
             * hot-spin; a real interactive session outlives this window, so an
             * ordinary `exit` still brings the prompt back promptly. */
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
