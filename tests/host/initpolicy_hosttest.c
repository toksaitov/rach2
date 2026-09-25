/**
 * Check service redirection and exact SSH policy lines in the real init source,
 * and the environment init starts the console's login with.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <time.h>
#include <signal.h>
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
#include <sys/stat.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef RB_HALT_SYSTEM
#define RB_HALT_SYSTEM 0xcdef0123
#endif
#ifndef RB_POWER_OFF
#define RB_POWER_OFF 0x4321fedc
#endif

static int Failed, Dup_fail, Dup_calls, Notify_fd = -1;
static int Network_signal, Early_signal, Fork_fail, Fork_blocked, Network_status;
static pid_t Network_pid;
static const char *Policy;
static size_t Policy_size;

/** Replace reboot with a distinct child exit without stopping the host. */
static int policy_reboot(int command)
{
    (void)command;
    _exit(90);
}

/** Inject a signal before the worker returns from fork, or fail the fork. */
static pid_t policy_fork(void)
{
    sigset_t mask;
    pid_t pid;
    int worker = Network_signal && Network_pid == 0;

    if (worker) {
        if (sigprocmask(SIG_BLOCK, NULL, &mask) != 0) {
            exit(2);
        }
        Fork_blocked = sigismember(&mask, SIGUSR1) && sigismember(&mask, SIGUSR2) &&
                       sigismember(&mask, SIGTERM);
        if (Fork_fail) {
            errno = EAGAIN;
            return -1;
        }
    }
    pid = fork();
    if (pid < 0) {
        exit(2);
    }
    if (worker) {
        Network_pid = pid ? pid : -1;
        if (pid == 0 && Early_signal && kill(getpid(), Network_signal) != 0) {
            _exit(2);
        }
    }
    return pid;
}

/** Record whether the worker terminated by signal or called reboot. */
static pid_t policy_waitpid(pid_t pid, int *status, int options)
{
    int result;
    pid_t reaped = waitpid(pid, &result, options);

    if (reaped > 0) {
        if (status) {
            *status = result;
        }
        if (Network_signal && reaped == Network_pid) {
            Network_status = result;
        }
    }
    return reaped;
}

/** Supply policy bytes without opening the host's system configuration. */
static FILE *policy_fopen(const char *path, const char *mode)
{
    FILE *f;
    (void)path;
    (void)mode;
    if (!Policy) {
        return NULL;
    }
    f = tmpfile();
    if (!f || fwrite(Policy, 1, Policy_size, f) != Policy_size) {
        exit(2);
    }
    rewind(f);
    return f;
}

/** Fail each redirection call in turn. */
static int policy_dup2(int from, int to)
{
    Dup_calls++;
    if (Dup_calls == Dup_fail) {
        errno = EBADF;
        return -1;
    }
    return dup2(from, to);
}

/**
 * Refuse filesystem mutation and replace ifup with an observable child exit.
 */
static int policy_unlink(const char *path)
{
    (void)path;
    return 0;
}
static int policy_execv(const char *path, char *const argv[])
{
    char c = 'x';
    (void)path;
    (void)argv;
    if (Network_signal && kill(getppid(), Network_signal) != 0) {
        _exit(2);
    }
    if (Notify_fd >= 0) {
        (void)write(Notify_fd, &c, 1);
    }
    _exit(0);
}

/**
 * Stand in for the console login's exec: the child exits 0 when the first
 * `LOGIN_TIMEOUT` in the environment it passes is `LOGIN_TIMEOUT=0`, the one
 * getenv(3) gives login, and 3 otherwise. Any other program is executed as
 * before.
 */
static int policy_execve(const char *path, char *const argv[], char *const envp[])
{
    int i;

    if (strcmp(path, "/bin/login") != 0) {
        return execve(path, argv, envp);
    }
    for (i = 0; envp[i]; i++) {
        if (strncmp(envp[i], "LOGIN_TIMEOUT=", 14) == 0) {
            _exit(strcmp(envp[i], "LOGIN_TIMEOUT=0") == 0 ? 0 : 3);
        }
    }
    _exit(3);
}

#define INIT_METAL
#define INIT_CONSOLE_LOGIN
#define fopen   policy_fopen
#define dup2    policy_dup2
#define unlink  policy_unlink
#define execv   policy_execv
#define execve  policy_execve
#define fork    policy_fork
#define waitpid policy_waitpid
#define reboot  policy_reboot
#define main    init_main
#include "init.c"
#undef main
#undef fopen
#undef dup2
#undef unlink
#undef execv
#undef fork
#undef waitpid
#undef reboot

/** Record failed assertions. */
static void check(int ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "initpolicy-hosttest: fail: %s\n", name);
        Failed++;
    }
}

/** Check exact bytes through the production policy parser. */
static void policy_case(const char *data, size_t len, int want, const char *name)
{
    Policy = data;
    Policy_size = len;
    check(root_password_login() == want, name);
}

/**
 * A child must either redirect all three descriptors or exit before returning.
 */
static void redirect_case(int nullfd, int logfd, int fail, int want)
{
    pid_t pid;
    int status;

    service_null_fd = nullfd;
    service_log_fd = logfd;
    Dup_calls = 0;
    Dup_fail = fail;
    pid = fork();
    if (pid < 0) {
        exit(2);
    }
    if (pid == 0) {
        struct stat a, b;
        char c;
        int ok;

        service_output();
        ok = read(0, &c, 1) == 0 && fstat(1, &a) == 0 && fstat(2, &b) == 0 &&
             a.st_dev == b.st_dev && a.st_ino == b.st_ino;
        _exit(ok ? 0 : 3);
    }
    if (waitpid(pid, &status, 0) != pid) {
        exit(2);
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == want,
          "service cannot return after redirection failure");
}

/**
 * Network redirection must leave init's descriptors unchanged on every path.
 */
static void network_case(int nullfd, int logfd, int fail, int want)
{
    int fds[2], n;
    struct stat before[3], after;
    char c;

    if (pipe(fds) < 0) {
        exit(2);
    }
    for (n = 0; n < 3; n++) {
        if (fstat(n, &before[n]) != 0) {
            exit(2);
        }
    }
    Notify_fd = fds[1];
    service_null_fd = nullfd;
    service_log_fd = logfd;
    Dup_calls = 0;
    Dup_fail = fail;
    start_network();
    close(fds[1]);
    check(read(fds[0], &c, 1) == want, "network setup requires redirected descriptors");
    close(fds[0]);
    Notify_fd = -1;
    for (n = 0; n < 3; n++) {
        check(fstat(n, &after) == 0 && after.st_dev == before[n].st_dev &&
                  after.st_ino == before[n].st_ino,
              "network keeps init console descriptors");
    }
}

/**
 * Exercise init's handlers with real fork, masks and signal delivery. Signals
 * arrive before the worker returns from fork or from its running ifup child.
 * A failed fork must also restore the parent's mask and preserve its handlers.
 */
static void network_signal_case(int nullfd, int logfd, int sig, int early, int fail)
{
    static const int signals[] = {SIGUSR1, SIGUSR2, SIGTERM};
    struct sigaction action, saved[3], current;
    sigset_t mask, original, after;
    size_t i;

    if (sigprocmask(SIG_BLOCK, NULL, &original) != 0) {
        exit(2);
    }
    mask = original;
    sigaddset(&mask, SIGALRM);
    memset(&action, 0, sizeof(action));
    action.sa_handler = shutdown_handler;
    sigemptyset(&action.sa_mask);
    for (i = 0; i < 3; i++) {
        sigdelset(&mask, signals[i]);
        if (sigaction(signals[i], &action, &saved[i]) != 0) {
            exit(2);
        }
    }
    if (sigprocmask(SIG_SETMASK, &mask, NULL) != 0) {
        exit(2);
    }
    Network_signal = sig;
    Early_signal = early;
    Fork_fail = fail;
    Fork_blocked = 0;
    Network_status = -1;
    Network_pid = 0;
    service_null_fd = nullfd;
    service_log_fd = logfd;
    Dup_calls = Dup_fail = 0;
    start_network();
    check(Fork_blocked, "network fork blocks shutdown signals");
    if (!fail) {
        check(Network_status != -1 && WIFSIGNALED(Network_status) &&
                  WTERMSIG(Network_status) == sig,
              early ? "early network signal terminates worker" : "ifup signal terminates worker");
    }
    if (sigprocmask(SIG_BLOCK, NULL, &after) != 0) {
        exit(2);
    }
    for (i = 1; i < NSIG; i++) {
        check(sigismember(&mask, (int)i) == sigismember(&after, (int)i),
              "network restores parent signal mask");
    }
    for (i = 0; i < 3; i++) {
        check(sigaction(signals[i], NULL, &current) == 0 && current.sa_handler == shutdown_handler,
              "network preserves parent shutdown handlers");
        if (sigaction(signals[i], &saved[i], NULL) != 0) {
            exit(2);
        }
    }
    if (sigprocmask(SIG_SETMASK, &original, NULL) != 0) {
        exit(2);
    }
    Network_signal = Fork_fail = 0;
}

/**
 * Require init to start the console's login with its prompt timeout off
 * when init's own environment has `inherited` as `LOGIN_TIMEOUT`, or no
 * `LOGIN_TIMEOUT` when it is null, and to set it in the console's child
 * only, leaving init's own environment, which its other children inherit,
 * as it was.
 */
static void login_case(const char *inherited)
{
    const char *kept;
    int status;
    pid_t pid;

    if (inherited ? setenv("LOGIN_TIMEOUT", inherited, 1) != 0 : unsetenv("LOGIN_TIMEOUT") != 0) {
        exit(2);
    }
    pid = spawn_shell();
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        exit(2);
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the console login waits without a timeout");
    kept = getenv("LOGIN_TIMEOUT");
    check(inherited ? kept && strcmp(kept, inherited) == 0 : kept == NULL,
          "init's own environment keeps its LOGIN_TIMEOUT");
}

/** Test parser boundaries and every redirection failure. */
int main(void)
{
    static const int signals[] = {SIGUSR1, SIGUSR2, SIGTERM};
    char longline[300];
    int nullfd, logfd, i;
    FILE *log;

    policy_case("root-password-login yes\n", 24, 1, "exact SSH policy accepted");
    policy_case("root-password-login yes", 23, 1, "SSH policy at EOF accepted");
    policy_case("root-password-login yes \t\r\v\f\n", 29, 1, "trailing whitespace accepted");
    policy_case("root-password-login yes anything\n", 32, 0, "SSH trailing text rejected");
    policy_case("root-password-login yesterday\n", 29, 0, "SSH prefix rejected");
    policy_case("root-password-login yes\0junk\n", 29, 0, "SSH NUL suffix rejected");
    policy_case(NULL, 0, 0, "missing SSH policy disabled");
    memset(longline, ' ', sizeof(longline));
    memcpy(longline, "root-password-login yes", 23);
    policy_case(longline, sizeof(longline), 1, "long trailing whitespace accepted");
    longline[299] = 'x';
    policy_case(longline, sizeof(longline), 0, "late SSH suffix rejected");
    memset(longline, '#', sizeof(longline));
    memcpy(longline + 127, "root-password-login yes\n", 24);
    policy_case(longline, sizeof(longline), 0, "SSH fragments cannot become lines");
    nullfd = open("/dev/null", O_RDWR);
    log = tmpfile();
    if (nullfd < 0 || !log) {
        exit(2);
    }
    logfd = fileno(log);
    redirect_case(nullfd, logfd, 0, 0);
    redirect_case(nullfd, -1, 0, 0);
    redirect_case(-1, logfd, 0, 126);
    redirect_case(-1, -1, 0, 126);
    for (i = 1; i <= 3; i++) {
        redirect_case(nullfd, logfd, i, 126);
    }
    network_case(nullfd, logfd, 0, 1);
    network_case(-1, logfd, 0, 0);
    for (i = 1; i <= 3; i++) {
        network_case(nullfd, logfd, i, 0);
    }
    for (i = 0; i < 3; i++) {
        network_signal_case(nullfd, logfd, signals[i], 0, 0);
        network_signal_case(nullfd, logfd, signals[i], 1, 0);
    }
    network_signal_case(nullfd, logfd, SIGTERM, 0, 1);
    login_case(NULL);
    login_case("60");
    fclose(log);
    close(nullfd);
    printf("initpolicy-hosttest: result: %s\n", Failed ? "fail" : "pass");
    return Failed ? 1 : 0;
}
