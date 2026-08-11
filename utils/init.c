/**
 * Rach2 PID 1: the reaper of orphans and the resurrector of the login shell.
 *
 * The kernel starts this as the first user process, so it is init, and it
 * reparents every orphaned process onto it. Its whole job is three things a
 * system needs forever: reap whatever children land on it (the console session
 * and every orphan the kernel hands over), respawn that session when it exits,
 * so the console is never left without one, and continue it if it is ever
 * stopped, since nothing else can. It never exits; if it did,
 * the kernel would have no reaper, so the kernel panics if init dies.
 *
 * Built like the other utils, linked against the ported musl (see utils/Makefile),
 * and staged at /sbin/init by the os-simple and os-real image manifests.
 */
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

extern char **environ;

#ifdef INIT_CONSOLE_LOGIN
/* The one program this build may exec. There is no list and no fallback: a
 * fall-through to a shell on any exec failure hands the console to whoever is
 * standing at it, and narrowing the fall-through to ENOENT inverts the danger
 * rather than removing it, since ENOENT is exactly the case of an image built
 * without /bin/login, which is the image that must refuse hardest. So the
 * fallback is not narrowed, it is compiled out: this artifact holds no code
 * path and no string that names a shell. */
#define LOGIN_PROGRAM "/bin/login"

/* The pause before a failed exec is tried again. Longer than the shell build's,
 * so a permanent failure states its reason on the console instead of scrolling
 * it away. */
#define LOGIN_RESPAWN_PAUSE 5
#else
/* The login shell, tried in order: busybox (os-real) then ish (os-simple). One
 * init binary serves both real images; the first path that exists wins. The
 * matching argv[0] carries a leading '-' so the shell runs as a login shell and
 * reads /etc/profile. */
static const char *const Shells[] = {"/bin/sh", "/ish", 0};
static const char *const Shell_Arg0[] = {"-sh", "-ish", 0};
#endif

#ifdef INIT_CONSOLE_LOGIN
/**
 * Say on the console that the login program could not be run, and how to get
 * back in. Returns nothing.
 *
 * Written with `write` rather than `printf` because this runs in a child that
 * has just failed an `execve`, and the only thing it can still be sure of is a
 * descriptor. The way back in is stated rather than assumed: `rach2.rescue`
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
 * The child does the getty dance before exec: `setsid` makes it a new session's
 * leader (with no controlling terminal), then `ioctl(TIOCSCTTY)` claims the
 * console as that session's controlling terminal. The shell thus starts as a
 * session leader that already owns the terminal, so a job-control shell like
 * busybox ash, which checks `tcgetpgrp` at startup, finds itself the foreground.
 * That is why init must never own the console: it hands a fresh one to each
 * shell it spawns, exactly as a real init delegates a terminal through getty.
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

int main(void)
{
    pid_t shell = -1;
    time_t spawned = 0;

    /* The three shutdown requests, installed before the first child so a
     * request that arrives early is answered rather than defaulting to a
     * terminating disposition, which for pid 1 is a panic. */
    signal(SIGUSR1, shutdown_handler);
    signal(SIGUSR2, shutdown_handler);
    signal(SIGTERM, shutdown_handler);

    for (;;) {
        int status;
        pid_t reaped;

        /* Keep a login shell running. If the fork failed (a full process table),
         * back off a second before retrying rather than spin: wait() returns
         * immediately with ECHILD while init has no children, so retrying without
         * a pause would peg the CPU. */
        if (shell < 0) {
            shell = spawn_shell();
            if (shell < 0) {
                sleep(1);
                continue;
            }
            spawned = time(0);
        }

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
        if (reaped == shell) {
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
