/* Native workloads for the etude-guest protocol host test. */
#ifdef _WIN32
int etude_workload_guest(int argc, char **argv, const char *role)
{
    (void)argc;
    (void)argv;
    (void)role;
    return 125; /* POSIX guest fixtures execute in the declared GNU helper. */
}
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/** Transfer exact bytes without stdio buffering or an external filter. */
static int guest_bytes(int fd, int byte, size_t count)
{
    char block[4096];
    memset(block, byte, sizeof(block));
    while (count) {
        size_t amount = count < sizeof(block) ? count : sizeof(block);
        ssize_t n = write(fd, block, amount);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 125;
        count -= (size_t)n;
    }
    return 0;
}

/** Open a path associated with the fixture's actual process witnesses. */
static int guest_file(const char *suffix, int flags)
{
    const char *marks = getenv("MARKS");
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s%s", marks ? marks : "", suffix);
    return !marks || n < 0 || n >= (int)sizeof(path) ? -1 : open(path, flags, 0600);
}

/* Hold the FIFO until exit, after writing more than one responder read. */
static int guest_backpressure(void)
{
    int witness = guest_file(".pressure", O_WRONLY);
    if (witness < 0 || write(witness, "r", 1) != 1)
        return 125;
    if (guest_bytes(1, '0', 8192) || write(1, "\nFINAL OUTPUT\n", 14) != 14 ||
        write(2, "FINAL ERROR\n", 12) != 12)
        return 125;
    return 0;
}

static int guest_mark(void)
{
    int fd = guest_file("", O_WRONLY | O_CREAT | O_APPEND);
    int ok = fd >= 0 && write(fd, "run\n", 4) == 4;
    if (fd >= 0)
        close(fd);
    return ok ? 0 : 125;
}

static int guest_pid(const char *suffix, pid_t pid)
{
    char text[64];
    int fd = guest_file(suffix, O_WRONLY | O_CREAT | O_TRUNC);
    int n = snprintf(text, sizeof(text), "%ld\n", (long)pid);
    int ok = fd >= 0 && write(fd, text, (size_t)n) == n;
    if (fd >= 0)
        close(fd);
    return ok ? 0 : 125;
}

/** Keep inherited FIFO witnesses open until the controller ends the process. */
static void guest_pause(void)
{
    for (;;)
        pause();
}

/** Start a member; acknowledge its session change before publishing its PID. */
static pid_t guest_member(int escape)
{
    int ready[2];
    char byte;
    pid_t pid;
    if (pipe(ready) || (pid = fork()) < 0)
        return -1;
    if (!pid) {
        close(ready[0]);
        if ((escape && setsid() < 0) || write(ready[1], "r", 1) != 1)
            _exit(125);
        close(ready[1]);
        guest_pause();
        _exit(125);
    }
    close(ready[1]);
    ssize_t count;
    do {
        count = read(ready[0], &byte, 1);
    } while (count < 0 && errno == EINTR);
    if (count != 1) {
        waitpid(pid, NULL, 0);
        pid = -1;
    }
    close(ready[0]);
    return pid;
}

#if defined(__linux__) || defined(__rach2__)
/* Double fork after setsid without a child report. The FIFO descriptor stays
 * open in the daemon until the responder's per-attempt owner reaps it. */
static int guest_detached(int hang)
{
    int ready[2], status;
    pid_t middle, daemon = -1;
    if (guest_file(".open", O_WRONLY) < 0 || pipe(ready) || (middle = fork()) < 0)
        return 125;
    if (!middle) {
        close(ready[0]);
        if (setsid() < 0 || (daemon = fork()) < 0)
            _exit(125);
        if (daemon)
            _exit(0);
        daemon = getpid();
        if (write(ready[1], &daemon, sizeof(daemon)) != sizeof(daemon))
            _exit(125);
        close(ready[1]);
        guest_pause();
        _exit(125);
    }
    close(ready[1]);
    if (read(ready[0], &daemon, sizeof(daemon)) != sizeof(daemon))
        daemon = -1;
    close(ready[0]);
    if (waitpid(middle, &status, 0) != middle || !WIFEXITED(status) || WEXITSTATUS(status) ||
        daemon <= 0 || guest_pid(".escaped", daemon))
        return 125;
    puts("ready");
    fflush(stdout);
    if (hang)
        guest_pause();
    return 7;
}
#endif

/** Make the group, escaped child and FIFO ownership of the old workloads. */
static int guest_holding(const char *role)
{
    pid_t member, escaped;
    char byte;
    int fd;
    if (!strcmp(role, "guest-ignore"))
        signal(SIGTERM, SIG_IGN);
    if (guest_file(".open", O_WRONLY) < 0 || guest_pid(".leader", getpid()))
        return 125;
    if (!strcmp(role, "guest-ignore")) {
        puts("ready");
        fflush(stdout);
        guest_pause();
    }
    member = guest_member(0);
    if (member < 0 || guest_pid(".member", member))
        return 125;
    if (!strcmp(role, "guest-leaving")) {
        if (guest_file(".leader-open", O_WRONLY) < 0)
            return 125;
        puts("ready");
        fflush(stdout);
        fd = guest_file(".release", O_RDONLY);
        if (fd < 0)
            return 125;
        while (read(fd, &byte, 1) == 1 && byte != '\n')
            ;
        close(fd);
        return 0;
    }
    escaped = guest_member(1);
    if (escaped < 0)
        return 125;
    if (guest_pid(".escaped", escaped)) {
        kill(escaped, SIGKILL);
        waitpid(escaped, NULL, 0);
        return 125;
    }
    printf("@etude 1 child %ld\nready\n", (long)escaped);
    fflush(stdout);
    guest_pause();
    return 125;
}

/** Require the actual terminal descriptors and controlling terminal. */
static int guest_terminal(void)
{
    char line[4096];
    int fd;
    if (guest_mark() || !isatty(0) || !isatty(1) || !isatty(2) ||
        (fd = open("/dev/tty", O_RDONLY)) < 0)
        return 1;
    close(fd);
    if (!fgets(line, sizeof(line), stdin))
        return 1;
    printf("got %s", line);
    return 0;
}

/** Whether a command's record in the session directory `dir` has this
 * process's `started` event, `started CMD TOKEN PID START`. */
static int guest_started_in(const char *dir)
{
    char path[4096], line[512];
    struct dirent *c;
    DIR *cmds = opendir(dir);
    FILE *f;
    long pid;
    int found = 0;

    if (cmds == NULL)
        return 0;
    while (!found && (c = readdir(cmds)) != NULL) {
        if (c->d_name[0] == '.' ||
            snprintf(path, sizeof(path), "%s/%s", dir, c->d_name) >= (int)sizeof(path) ||
            (f = fopen(path, "r")) == NULL)
            continue;
        while (!found && fgets(line, sizeof(line), f))
            found = sscanf(line, "started %*s %*s %ld", &pid) == 1 && pid == (long)getpid();
        fclose(f);
    }
    closedir(cmds);
    return found;
}

/** Print `recorded` when a session of the state directory beside `MARKS`
 * has this process's `started` event, and `unrecorded` otherwise. */
static int guest_recorded(void)
{
    const char *marks = getenv("MARKS");
    size_t len = marks ? strlen(marks) : 0;
    char base[4096], dir[4096];
    struct dirent *s;
    DIR *sessions;
    int found = 0;

    if (len < 6 || strcmp(marks + len - 6, "/marks") != 0 ||
        snprintf(base, sizeof(base), "%.*s/state/s", (int)(len - 6), marks) >= (int)sizeof(base) ||
        (sessions = opendir(base)) == NULL)
        return 125;
    while (!found && (s = readdir(sessions)) != NULL)
        found = s->d_name[0] != '.' &&
                snprintf(dir, sizeof(dir), "%s/%s", base, s->d_name) < (int)sizeof(dir) &&
                guest_started_in(dir);
    closedir(sessions);
    puts(found ? "recorded" : "unrecorded");
    return 0;
}

/** Observe the executable path and all arguments supplied by the fixtures. */
static int guest_arguments(int argc, char **argv)
{
    int i;
    if (guest_mark())
        return 125;
    printf("%s|", argv[0]);
    for (i = 1; i < argc; i++)
        printf("%s%s", i > 1 ? " " : "", argv[i]);
    putchar('\n');
    return 0;
}

int etude_workload_guest(int argc, char **argv, const char *role)
{
    char cwd[4096];
    const char *home = getenv("HOME");
    if (!strcmp(role, "guest-pass")) {
        if (guest_mark() || !getcwd(cwd, sizeof(cwd)))
            return 125;
        printf("@etude 1 begin host.etude.guest\n%s|%s|%s\n@etude 1 end 0\n",
               argc > 1 ? argv[1] : "", home ? home : "", cwd);
    } else if (!strcmp(role, "guest-fail")) {
        fputs("broken\n", stderr);
        return 3;
    } else if (!strcmp(role, "guest-both")) {
        puts("out");
        fputs("err\n", stderr);
    } else if (!strcmp(role, "guest-backpressure")) {
        return guest_backpressure();
    } else if (!strcmp(role, "guest-flood")) {
        return guest_bytes(1, '0', 1200);
    } else if (!strcmp(role, "guest-long")) {
        if (guest_bytes(1, '0', 10000000) || guest_bytes(2, '1', 10000000))
            return 125;
        signal(SIGTERM, SIG_DFL);
        kill(getpid(), SIGTERM);
        return 125;
    } else if (!strcmp(role, "guest-quick")) {
        puts("quick");
    } else if (!strcmp(role, "guest-recorded")) {
        return guest_recorded();
    } else if (!strcmp(role, "guest-args")) {
        return guest_arguments(argc, argv);
    } else if (!strcmp(role, "guest-term")) {
        return guest_terminal();
    } else if (!strcmp(role, "guest-init")) {
        if (guest_mark())
            return 125;
        printf("@etude 1 begin host.etude.guest\npid %ld\n", (long)getpid());
        return 7;
#if defined(__linux__) || defined(__rach2__)
    } else if (!strcmp(role, "guest-detached-pass") || !strcmp(role, "guest-detached-hang")) {
        return guest_detached(!strcmp(role, "guest-detached-hang"));
#endif
    } else if (!strcmp(role, "guest-ignore") || !strcmp(role, "guest-hang") ||
               !strcmp(role, "guest-leaving")) {
        return guest_holding(role);
    } else {
        return 125;
    }
    return 0;
}
#endif
