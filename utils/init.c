/**
 * Rach2 PID 1: the reaper of orphans and the resurrector of the login shell.
 *
 * The kernel starts this as the first user process, so it is init, and it
 * reparents every orphaned process onto it. Its whole job is two things a system
 * needs forever: reap whatever children land on it (the login shell and every
 * orphan the kernel hands over), and respawn the login shell when that shell
 * exits, so the console is never left without one. It never exits; if it did,
 * the kernel would have no reaper, so the kernel panics if init dies.
 *
 * Built like the other utils, linked against the ported musl (see utils/Makefile),
 * and staged at /sbin/init by the os-simple and os-real image manifests.
 */
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

extern char **environ;

/* The login shell, tried in order: busybox (os-real) then ish (os-simple). One
 * init binary serves both real images; the first path that exists wins. The
 * matching argv[0] carries a leading '-' so the shell runs as a login shell and
 * reads /etc/profile. */
static const char *const Shells[]    = { "/bin/sh", "/ish", 0 };
static const char *const Shell_Arg0[] = { "-sh", "-ish", 0 };

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
    int i;

    if (pid != 0) {
        return pid;             /* init (the parent), or -1 if fork failed */
    }

    setsid();                   /* new session; this child is its leader */
    ioctl(STDIN_FILENO, TIOCSCTTY, 0);  /* claim the console as our ctty */
    for (i = 0; Shells[i] != 0; i++) {
        char *argv[2];
        argv[0] = (char *)Shell_Arg0[i];
        argv[1] = 0;
        execve(Shells[i], argv, environ);   /* returns only on failure */
    }
    _exit(127);                 /* no login shell present in the image */
    return -1;                  /* unreachable; quiets the compiler */
}

int main(void)
{
    pid_t shell = -1;
    time_t spawned = 0;

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

        /* The shell is a live child, so wait() blocks until it or a reparented
         * orphan dies. Reap it; only the login shell's death triggers a respawn. */
        reaped = wait(&status);
        if (reaped == shell) {
            shell = -1;
            /* A shell that dies almost as soon as it starts (a missing or
             * crash-looping binary in a misbuilt image) would otherwise be
             * reforked at full CPU. Pause before respawning so the loop cannot
             * hot-spin; a real interactive session outlives this window, so an
             * ordinary `exit` still brings the prompt back promptly. */
            if (time(0) - spawned < 2) {
                sleep(1);
            }
        }
    }
    return 0;                           /* unreachable */
}
