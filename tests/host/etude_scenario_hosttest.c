/**
 * Check libetude-case (tools/etude/posix/scenario.c), the library of etude's
 * machine scenarios, with this program as the scenarios, as a stand-in for
 * QEMU when ETUDE_FAKE_QEMU is set, and as the host tool and the guest
 * program they run; and with the real QEMU on the kernel, the test image and
 * a disk of the architecture ARCH names.
 *
 * host-deps: tools/etude/posix/scenario.c tools/etude/posix/case_main.c
 * host-deps: tools/etude/serial.c tools/etude/processor.c
 * host-deps: tools/etude/posix/qemu.c tools/etude/qemu_profile.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/posix/store.c
 * host-deps: tools/etude/capture.c tools/etude/history_json.c
 * host-deps: tools/etude/guest/guest.c tools/etude/guest/fixture.c
 * host-deps: tools/etude/guest/wire.c
 * host-deps: utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/guest
 * host-cflags: -I utils -Wall -Wextra -Werror
 * host-seconds: 300
 *
 * Each row is one attempt of a machine case, run as the executor runs it:
 * this program with the invocation, its report read from its standard
 * output. A machine boots on a private copy of its disk, writes to it and
 * powers off; a host step reads the write from the copy, and the next boot
 * of the same copy reads it back, and the artifact itself is unchanged. A
 * machine that says it powers off and ends without QEMU's event, one killed
 * by a signal, one whose console closes and one whose QMP closes are each
 * told apart from a shutdown, and a wait on each ends at once with the loss.
 * A wait past its step's bound ends at the bound; a stop signal ends the
 * scenario's wait, its machine and its report, and keeps its copies; a
 * scenario that asks for what its manifest does not declare is refused; a
 * failed accelerator and a damaged artifact are reported. Under the stand-in
 * the machine's etude-guest runs a guest program whose report becomes the
 * scenario's, and an etude-guest killed in the middle of a run is a lost
 * end. This program is also the peer that a scenario runs as a service; two
 * machines at once reach it, a service ended unseen is a fault, and failing
 * rows at each stage end their own processes while a held row keeps its
 * own. With the real QEMU, a disk root written at the debug shell and
 * powered off is read back by the next boot, etude-guest runs the test
 * image's /hello, and two machines run /tcptest against the peer.
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "compat/pathmax_host.h"
#include "compat/fixdir_host.c"
#include "etude/artifact.h"
#include "etude/scenario.h"
#include "sha256.h"

extern char **environ;

int etude_guest_main(int argc, char **argv);

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            printf("etude-scenario-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), \
                   #cond);                                                                  \
            fails++;                                                                        \
        }                                                                                   \
    } while (0)

/* The completion bounds of an attempt under the stand-in and the real QEMU,
 * and the step bounds of the rows: the bound row's, the lost rows', which a
 * loss must not wait out, and the others'. */
#define FAKE_ATTEMPT_MS 30000
#define REAL_ATTEMPT_MS 150000
#define SHORT_US        1000000u
#define LOSS_US         10000000u
#define STEP_US         60000000u
#define REAL_US         120000000u

/* The hold row's bound on the step in which the other rows fail beside it,
 * and on its attempt; the bound in which a killed row's processes end; and
 * the most processes of one row the test counts. */
#define HOLD_US         240000000u
#define HOLD_ATTEMPT_MS 300000
#define GONE_MS         15000
#define ROW_PROCS       64

/* This program, by an absolute path. */
static char self[PATH_MAX];

/* ---- The host tool and the guest program. ---- */

/** Say whether the file at `path` has the bytes of `text` anywhere. */
static int file_has(const char *path, const char *text)
{
    FILE *f = fopen(path, "rb");
    size_t len = strlen(text), n;
    char *buf;
    int found = 0;

    if (f == NULL)
        return 0;
    buf = malloc(1u << 24);
    if (buf != NULL) {
        size_t i;

        n = fread(buf, 1, 1u << 24, f);
        for (i = 0; len > 0 && i + len <= n && !found; i++)
            found = memcmp(buf + i, text, len) == 0;
    }
    free(buf);
    fclose(f);
    return found;
}

/** Say `text` on standard output at once, as a line. */
static void say(const char *text)
{
    printf("%s\n", text);
    fflush(stdout);
}

/** Give a socket address of 127.0.0.1 and `port`. */
static struct sockaddr_in loopback(long port)
{
    struct sockaddr_in a;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

/**
 * Bind or connect `fd` to row `mark`'s private Unix socket. Use its name
 * relative to the row's directory so that a long TMPDIR fits. Restore the
 * current directory before returning 0 for success or -1 for an error.
 */
static int peer_endpoint(int fd, const char *mark, int binding)
{
    struct sockaddr_un a;
    char path[PATH_MAX], *name;
    int cwd, result, error;

    if (mark == NULL) {
        errno = EINVAL;
        return -1;
    }
    fix_format(path, sizeof(path), "%s.peer", mark);
    name = strrchr(path, '/');
    if (name == NULL || strlen(name + 1) >= sizeof(a.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    *name++ = '\0';
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, name);
    cwd = open(".", O_RDONLY);
    if (cwd < 0)
        return -1;
    result = chdir(path);
    if (result == 0)
        result = binding ? bind(fd, (struct sockaddr *)&a, sizeof(a))
                         : connect(fd, (struct sockaddr *)&a, sizeof(a));
    error = errno;
    if (fchdir(cwd) != 0)
        exit(1);
    close(cwd);
    errno = error;
    return result;
}

/**
 * Receive the row's bound TCP socket from its controller and check its
 * port. Give the descriptor, owned by the caller, or -1 for an error.
 */
static int peer_socket(const char *port)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    struct msghdr msg;
    struct cmsghdr *c;
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    char byte;
    struct iovec iov = {&byte, 1};
    int fd = -1, connection;

    connection = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connection < 0)
        return -1;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);
    if (peer_endpoint(connection, getenv("ETUDE_TEST_MARK"), 0) == 0 &&
        recvmsg(connection, &msg, 0) == 1 && !(msg.msg_flags & MSG_CTRUNC)) {
        c = CMSG_FIRSTHDR(&msg);
        if (c != NULL && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
            c->cmsg_len == CMSG_LEN(sizeof(fd)))
            memcpy(&fd, CMSG_DATA(c), sizeof(fd));
    }
    close(connection);
    if (fd >= 0 && getsockname(fd, (struct sockaddr *)&a, &len) != 0) {
        close(fd);
        return -1;
    }
    if (fd >= 0 && a.sin_port != htons((unsigned short)strtol(port, NULL, 10))) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

/* The connections a peer serves at once. */
#define PEER_CONNS 8

/**
 * Be a peer on port `port` of 127.0.0.1, as `mode` says: `serve` sends each
 * connection's bytes back and says each line it got and each connection that
 * closed, `once` does so until its first connection closes and exits 0,
 * `early` exits 3 before it serves, and `mute` serves nothing and never says
 * that it is ready. A line `die` makes it exit 1 before it sends anything
 * back. The controller passes the bound socket over the row's private Unix
 * socket; its port must be `port`.
 */
static int peer(const char *port, const char *mode)
{
    struct pollfd p[PEER_CONNS + 1];
    char line[PEER_CONNS][256], text[300];
    size_t len[PEER_CONNS];
    int l, i, n, k;
    ssize_t got;
    char buf[512];

    if (strcmp(mode, "early") == 0) {
        say("peer: cannot serve: told to fail");
        return 3;
    }
    if (strcmp(mode, "mute") == 0)
        for (;;)
            pause();
    l = peer_socket(port);
    if (l < 0 || listen(l, PEER_CONNS) != 0) {
        if (l >= 0)
            close(l);
        snprintf(text, sizeof(text), "peer: cannot serve: %s", strerror(errno));
        say(text);
        return 3;
    }
    snprintf(text, sizeof(text), "peer: listening on %s", port);
    say(text);
    for (i = 0; i <= PEER_CONNS; i++) {
        p[i].fd = i == 0 ? l : -1;
        p[i].events = POLLIN;
    }
    for (;;) {
        if (poll(p, PEER_CONNS + 1, -1) < 0)
            continue;
        if (p[0].revents != 0)
            for (i = 1; i <= PEER_CONNS; i++)
                if (p[i].fd < 0) {
                    p[i].fd = accept(l, NULL, NULL);
                    len[i - 1] = 0;
                    break;
                }
        for (i = 1; i <= PEER_CONNS; i++) {
            if (p[i].fd < 0 || p[i].revents == 0)
                continue;
            got = read(p[i].fd, buf, sizeof(buf));
            if (got <= 0) {
                close(p[i].fd);
                p[i].fd = -1;
                say("peer: closed");
                if (strcmp(mode, "once") == 0)
                    return 0;
                continue;
            }
            for (n = 0; n < got; n++) {
                k = i - 1;
                if (buf[n] != '\n') {
                    if (len[k] + 1 < sizeof(line[k]))
                        line[k][len[k]++] = buf[n];
                    continue;
                }
                line[k][len[k]] = '\0';
                len[k] = 0;
                if (strcmp(line[k], "die") == 0) {
                    say("peer: dying");
                    return 1;
                }
                snprintf(text, sizeof(text), "peer: got %s", line[k]);
                say(text);
            }
            if (write(p[i].fd, buf, (size_t)got) != got)
                say("peer: a reply was cut short");
        }
    }
}

/**
 * Be the tool `argv[2]`: `scan FILE TEXT` exits 0 when FILE has TEXT;
 * `report CASE` reports one assertion of CASE as a guest program does;
 * `killguest FILE` kills the etude-guest whose PID FILE has; and `peer PORT
 * MODE MARK` is a service, whose MARK has its row and this run's fixture
 * directory, so that the test finds it among the processes.
 */
static int tool(int argc, char **argv)
{
    char line[256];
    FILE *f;
    long pid = 0;

    if (argc == 5 && strcmp(argv[2], "scan") == 0)
        return file_has(argv[3], argv[4]) ? 0 : 1;
    if (argc == 6 && strcmp(argv[2], "peer") == 0)
        return peer(argv[3], argv[4]);
    if (argc == 4 && strcmp(argv[2], "report") == 0) {
        printf("guest: running\n");
        printf("@etude 1 begin %s\n@etude 1 assert guest-ran pass\n@etude 1 end 1\n", argv[3]);
        return 0;
    }
    if (argc == 4 && strcmp(argv[2], "killguest") == 0) {
        printf("@etude 1 begin x\n");
        fflush(stdout);
        f = fopen(argv[3], "r");
        if (f != NULL && fgets(line, sizeof(line), f) != NULL)
            pid = strtol(line, NULL, 10);
        if (f != NULL)
            fclose(f);
        if (pid > 0)
            kill((pid_t)pid, SIGKILL);
        return 0;
    }
    return 2;
}

/* ---- The stand-in for QEMU. ---- */

/** Write all of `text` to `fd`, or end the stand-in. */
static void put(int fd, const char *text)
{
    size_t len = strlen(text), done = 0;
    ssize_t n;

    while (done < len) {
        n = write(fd, text + done, len - done);
        if (n <= 0)
            _exit(120);
        done += (size_t)n;
    }
}

/** Connect to the Unix socket at `path`, as QEMU's client chardev does. */
static int fake_connect(const char *path)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
        _exit(121);
    return fd;
}

/** Find the value after `head` of the argument after option `opt`. */
static const char *fake_arg(int argc, char **argv, const char *opt, const char *head)
{
    int i;

    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], opt) == 0 && strncmp(argv[i + 1], head, strlen(head)) == 0)
            return argv[i + 1] + strlen(head);
    return NULL;
}

/* A line read a byte at a time from a socket. */
struct fake_in {
    int fd;
    char line[512];
    size_t len;
};

/** Read what `in` has; 1 with a whole line in `in->line`, 0 for none yet,
 * -1 at its end. */
static int fake_read(struct fake_in *in)
{
    char c;
    ssize_t n;

    for (;;) {
        n = read(in->fd, &c, 1);
        if (n <= 0)
            return n < 0 && errno == EINTR ? 0 : -1;
        if (c == '\n' || c == '\r') {
            if (in->len == 0)
                continue;
            in->line[in->len] = '\0';
            in->len = 0;
            return 1;
        }
        if (in->len + 1 < sizeof(in->line))
            in->line[in->len++] = c;
    }
}

/** Give the command a QMP line executes, in `name`, and its id. */
static long fake_command(const char *line, char *name, size_t size)
{
    const char *at = strstr(line, "\"execute\":\""), *id = strstr(line, "\"id\":");
    size_t n = 0;

    name[0] = '\0';
    if (at == NULL)
        return -1;
    for (at += 11; *at != '\0' && *at != '"' && n + 1 < size; at++)
        name[n++] = *at;
    name[n] = '\0';
    return id != NULL ? strtol(id + 5, NULL, 10) : -1;
}

/** Send QMP's event SHUTDOWN of the guest or not, for `reason`. */
static void fake_shutdown(int qmp, int guest, const char *reason)
{
    char text[256];

    snprintf(text, sizeof(text),
             "{\"timestamp\": {\"seconds\": 1, \"microseconds\": 2}, \"event\": \"SHUTDOWN\", "
             "\"data\": {\"guest\": %s, \"reason\": \"%s\"}}\r\n",
             guest ? "true" : "false", reason);
    put(qmp, text);
}

/** Answer one QMP line of the launcher's or the scenario's; exit at quit. */
static void fake_qmp(int qmp, const char *line, const char *accel, const char *mode)
{
    char name[64], text[256];
    long id = fake_command(line, name, sizeof(name));

    if (strcmp(name, "query-accelerators") == 0)
        snprintf(text, sizeof(text),
                 "{\"return\": {\"enabled\": \"%s\", \"present\": [\"kvm\", \"tcg\"]}, "
                 "\"id\": %ld}\r\n",
                 strcmp(mode, "wrong") == 0 ? (strcmp(accel, "tcg") == 0 ? "kvm" : "tcg") : accel,
                 id);
    else if (strcmp(name, "query-status") == 0)
        snprintf(text, sizeof(text),
                 "{\"return\": {\"status\": \"running\", \"running\": true}, \"id\": %ld}\r\n", id);
    else if (strcmp(name, "query-cpus-fast") == 0)
        snprintf(text, sizeof(text), "{\"return\": [{\"cpu-index\": 0}], \"id\": %ld}\r\n", id);
    else if (strcmp(name, "human-monitor-command") == 0)
        snprintf(text, sizeof(text), "{\"return\": \"EAX=00000000\\r\\n\", \"id\": %ld}\r\n", id);
    else if (strcmp(name, "qmp_capabilities") == 0 || strcmp(name, "cont") == 0 ||
             strcmp(name, "quit") == 0)
        snprintf(text, sizeof(text), "{\"return\": {}, \"id\": %ld}\r\n", id);
    else
        snprintf(text, sizeof(text),
                 "{\"id\": %ld, \"error\": {\"class\": \"CommandNotFound\", \"desc\": \"no\"}}\r\n",
                 id);
    put(qmp, text);
    if (strcmp(name, "quit") == 0) {
        fake_shutdown(qmp, 0, "host-qmp-quit");
        _exit(0);
    }
}

/** Append `text` to the disk at `disk`, or read its first bytes into `out`. */
static void fake_disk(const char *disk, const char *text, char *out, size_t size)
{
    int fd = open(disk, text != NULL ? O_WRONLY | O_APPEND : O_RDONLY);
    ssize_t n;

    if (fd < 0)
        _exit(127);
    if (text != NULL) {
        if (write(fd, text, strlen(text)) != (ssize_t)strlen(text))
            _exit(127);
    } else {
        n = read(fd, out, size - 1);
        out[n > 0 ? n : 0] = '\0';
        for (n = 0; out[n] != '\0'; n++)
            if (out[n] < ' ' || out[n] > '~')
                out[n] = '.';
    }
    close(fd);
}

/**
 * Send `TEXT` of `args`, `PORT TEXT`, as a line to the peer on that port of
 * 127.0.0.1, as a guest reaches the host, and say on the console `con`
 * whether the line came back.
 */
static void fake_peer(int con, const char *args)
{
    char *text, want[300], got[300], said[400];
    struct sockaddr_in a = loopback(strtol(args, &text, 10));
    size_t n = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    snprintf(want, sizeof(want), "%s\n", *text == ' ' ? text + 1 : text);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        write(fd, want, strlen(want)) == (ssize_t)strlen(want))
        while (n + 1 < sizeof(got) && read(fd, got + n, 1) == 1 && got[n++] != '\n') {
        }
    got[n] = '\0';
    if (fd >= 0)
        close(fd);
    want[strlen(want) - 1] = '\0';
    if (n > 0 && strncmp(got, want, strlen(want)) == 0)
        snprintf(said, sizeof(said), "fake: peer echoed %s\r\n> ", want);
    else
        snprintf(said, sizeof(said), "fake: peer gone\r\n> ");
    put(con, said);
}

/**
 * Run etude-guest on the console `con` in a child, as the debug shell's
 * `exec /etude-guest` does, with the stage directory ETUDE_FAKE_STAGE and
 * the resend interval of `line`; its PID goes to ETUDE_FAKE_GUEST_PID.
 */
static pid_t fake_guest(int con, const char *line)
{
    const char *resend = strstr(line, "--resend-ms ");
    char state[PATH_MAX], ms[32], pid[32];
    char *args[] = {
        "etude-guest", "--stage", getenv("ETUDE_FAKE_STAGE"), "--state", state, "--resend-ms", ms,
        NULL};
    pid_t child;
    FILE *f;

    snprintf(ms, sizeof(ms), "%ld", resend != NULL ? strtol(resend + 12, NULL, 10) : 2000L);
    snprintf(state, sizeof(state), "%s.state", getenv("ETUDE_FAKE_GUEST_PID"));
    child = fork();
    if (child == 0) {
        dup2(con, 0);
        dup2(con, 1);
        close(con);
        _exit(etude_guest_main(7, args));
    }
    snprintf(pid, sizeof(pid), "%ld\n", (long)child);
    f = fopen(getenv("ETUDE_FAKE_GUEST_PID"), "w");
    if (f != NULL) {
        fputs(pid, f);
        fclose(f);
    }
    return child;
}

/**
 * Act on the line that the console `con` typed: `write TEXT` and `read` the
 * first disk, at `disk` or none at NULL, `poweroff`, `die`, `closecon`,
 * `closeqmp`, `peer PORT TEXT` and `exec /etude-guest`. A console or QMP that
 * it closes has its descriptor set to -1.
 */
static void fake_console(struct fake_in *con, struct fake_in *qmp, const char *disk,
                         const char *mode)
{
    char text[600], read_back[128];

    if (strncmp(con->line, "write ", 6) == 0 && disk != NULL) {
        fake_disk(disk, con->line + 6, NULL, 0);
        put(con->fd, "fake: wrote\r\n> ");
    } else if (strcmp(con->line, "read") == 0 && disk != NULL) {
        fake_disk(disk, NULL, read_back, sizeof(read_back));
        snprintf(text, sizeof(text), "fake: disk says %s\r\n> ", read_back);
        put(con->fd, text);
    } else if (strcmp(con->line, "poweroff") == 0) {
        put(con->fd, "poweroff: powering off\r\n");
        if (strcmp(mode, "lost") == 0)
            _exit(0);
        fake_shutdown(qmp->fd, 1, "guest-shutdown");
        _exit(0);
    } else if (strcmp(con->line, "die") == 0) {
        put(con->fd, "fake: dying\r\n");
        kill(getpid(), SIGKILL);
    } else if (strcmp(con->line, "closecon") == 0) {
        close(con->fd);
        con->fd = -1;
    } else if (strcmp(con->line, "closeqmp") == 0) {
        put(con->fd, "fake: no qmp\r\n> ");
        close(qmp->fd);
        qmp->fd = -1;
    } else if (strncmp(con->line, "peer ", 5) == 0) {
        fake_peer(con->fd, con->line + 5);
    } else if (strncmp(con->line, "exec /etude-guest", 17) == 0) {
        fake_guest(con->fd, con->line);
        close(con->fd);
        con->fd = -1;
    }
}

/**
 * Be QEMU, as ETUDE_FAKE_QEMU's `mode` says: answer QMP's start, say that
 * the machine runs and the shell is ready, and act on what the console
 * types, as fake_console does.
 */
static int fake_qemu(const char *mode, int argc, char **argv)
{
    const char *accel = fake_arg(argc, argv, "-accel", "");
    const char *disk = fake_arg(argc, argv, "-drive", "file=");
    char path[PATH_MAX];
    struct fake_in con, qmp;
    struct pollfd p[2];
    int ok = 1, r;

    if (accel == NULL)
        return 125;
    if (disk != NULL) {
        snprintf(path, sizeof(path), "%s", disk);
        if (strchr(path, ',') != NULL)
            *strchr(path, ',') = '\0';
        disk = path;
    }
    con.fd = fake_connect(fake_arg(argc, argv, "-chardev", "socket,id=etude-console,path="));
    qmp.fd = fake_connect(fake_arg(argc, argv, "-chardev", "socket,id=etude-qmp,path="));
    con.len = qmp.len = 0;
    put(qmp.fd, "{\"QMP\": {\"version\": {}, \"capabilities\": []}}\r\n");
    while (ok) {
        r = fake_read(&qmp);
        if (r < 0)
            _exit(122);
        fake_qmp(qmp.fd, qmp.line, accel, mode);
        ok = strstr(qmp.line, "\"cont\"") == NULL;
    }
    put(con.fd, "fake: ready\r\nType 'help' for available commands.\r\n> ");
    for (;;) {
        p[0].fd = con.fd;
        p[1].fd = qmp.fd;
        p[0].events = p[1].events = POLLIN;
        if (poll(p, 2, -1) < 0)
            continue;
        if (qmp.fd >= 0 && p[1].revents != 0) {
            r = fake_read(&qmp);
            if (r < 0)
                _exit(0);
            if (r > 0)
                fake_qmp(qmp.fd, qmp.line, accel, mode);
        }
        if (con.fd < 0 || p[0].revents == 0)
            continue;
        r = fake_read(&con);
        if (r < 0) {
            close(con.fd);
            con.fd = -1;
            continue;
        }
        if (r == 0)
            continue;
        fake_console(&con, &qmp, disk, mode);
        if (con.fd < 0 && qmp.fd < 0)
            for (;;)
                pause();
    }
}

/* ---- The scenarios. ---- */

/** Say whether status `st` is `want`, reporting assertion `id` so. */
static void expect(struct etude_case *c, const char *id, enum etude_status st,
                   enum etude_status want)
{
    char detail[64];

    snprintf(detail, sizeof(detail), "status %d, not %d", (int)st, (int)want);
    etude_assert(c, id, st == want, detail);
}

/** Give the console offset past the last `text` seen, or report a failure. */
static uint64_t wait_for(struct etude_case *c, struct etude_case_machine *m, const char *id,
                         const char *text, uint64_t from)
{
    uint64_t at = from;
    enum etude_status st = etude_console_wait(m, text, from, &at);

    if (st != ETUDE_OK)
        etude_assert(c, id, 0, text);
    return st == ETUDE_OK ? at : from;
}

/** Run the reboot row: a write, a host read of the copy, and a readback. */
static int scenario_reboot(struct etude_case *c, int real)
{
    const char *nonce = getenv("ETUDE_TEST_NONCE");
    const char *copy, *again, *shell = real ? "\n> " : "> ";
    char *scan[] = {"scanner", "tool", "scan", NULL, (char *)nonce, NULL};
    struct etude_case_machine *m;
    struct etude_termination end;
    char line[128], *reply = NULL;
    uint64_t at;

    if (etude_step(c, "write") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    copy = etude_private_copy(c, "disk");
    at = wait_for(c, m, "stopped", "Type 'help'", 0);
    at = wait_for(c, m, "stopped", shell, at);
    if (!real) {
        expect(c, "qmp-status", etude_qmp(m, "query-status", NULL, &reply), ETUDE_OK);
        etude_assert(c, "qmp-reply", reply != NULL && strstr(reply, "\"running\"") != NULL, reply);
        free(reply);
        expect(c, "qmp-error", etude_qmp(m, "no-such-command", NULL, &reply), ETUDE_FAILED);
        free(reply);
    }
    snprintf(line, sizeof(line), real ? "write /marker %s\r" : "write %s\n", nonce);
    etude_type(m, line);
    at = wait_for(c, m, "stopped", real ? "\n> " : "fake: wrote", at);
    etude_type(m, real ? "poweroff\r" : "poweroff\n");
    expect(c, "stopped", etude_wait_end(m, &end), ETUDE_OK);
    etude_assert(c, "shutdown", end.end == ETUDE_END_SHUTDOWN, "not a shutdown");
    if (etude_step(c, "check") != ETUDE_OK)
        return 1;
    scan[3] = (char *)copy;
    expect(c, "host-ran", etude_host(c, scan, "scan", &end), ETUDE_OK);
    etude_assert(c, "host-sees", end.end == ETUDE_END_EXITED && end.value == 0,
                 "the copy lacks the write");
    if (etude_step(c, "readback") != ETUDE_OK ||
        etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    again = etude_private_copy(c, "disk");
    etude_assert(c, "same-copy", copy != NULL && again == copy, "another copy");
    at = wait_for(c, m, "readback", shell, wait_for(c, m, "readback", "Type 'help'", 0));
    etude_type(m, real ? "cat /marker\r" : "read\n");
    snprintf(line, sizeof(line), "%s", nonce);
    if (etude_console_wait(m, line, at, &at) == ETUDE_OK)
        etude_assert(c, "readback", 1, NULL);
    else
        etude_assert(c, "readback", 0, "the write was not read back");
    etude_type(m, real ? "poweroff\r" : "poweroff\n");
    expect(c, "second-stop", etude_wait_end(m, &end), ETUDE_OK);
    return 0;
}

/** Run the rows of a lost machine: each wait ends with the loss. */
static int scenario_lost(struct etude_case *c, const char *how)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    char *reply = NULL;
    uint64_t at;

    if (etude_step(c, "lose") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    at = wait_for(c, m, "lost-wait", "> ", 0);
    if (strcmp(how, "closeqmp") == 0) {
        etude_type(m, "closeqmp\n");
        wait_for(c, m, "lost-wait", "fake: no qmp", at);
        expect(c, "lost-wait", etude_qmp(m, "query-status", NULL, &reply), ETUDE_LOST);
        free(reply);
        etude_assert(c, "not-shutdown", 1, NULL);
        return 0;
    }
    etude_type(m, strcmp(how, "lost") == 0  ? "poweroff\n"
                  : strcmp(how, "die") == 0 ? "die\n"
                                            : "closecon\n");
    expect(c, "lost-wait", etude_console_wait(m, "never printed", at, &at), ETUDE_LOST);
    if (strcmp(how, "closecon") == 0) {
        etude_assert(c, "not-shutdown", 1, NULL);
        return 0;
    }
    expect(c, "ended", etude_wait_end(m, &end), ETUDE_OK);
    etude_assert(c, "not-shutdown",
                 end.end == ETUDE_END_QEMU_EXITED &&
                     end.value == (strcmp(how, "die") == 0 ? SIGKILL : 0),
                 "taken for another end");
    return 0;
}

/** Run the bound row: a wait for text that never comes ends at the bound. */
static int scenario_bound(struct etude_case *c)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at;

    if (etude_step(c, "wait") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    expect(c, "bounded", etude_console_wait(m, "never printed", 0, &at), ETUDE_BOUND);
    expect(c, "after-bound", etude_wait_end(m, &end), ETUDE_BOUND);
    return 0;
}

/** Run the stop row: announce the wait, which the test's signal ends. */
static int scenario_stop(struct etude_case *c)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at;

    if (etude_step(c, "wait") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    etude_private_copy(c, "disk");
    etude_progress(c, "waiting", 0);
    expect(c, "stopped", etude_console_wait(m, "never printed", 0, &at), ETUDE_STOPPED);
    expect(c, "still-stopped", etude_step(c, "after"), ETUDE_STOPPED);
    return 0;
}

/** Run the misuse row: each request the manifest does not declare is
 * refused. */
static int scenario_misuse(struct etude_case *c)
{
    char *undeclared[] = {"cc", NULL}, *reply = NULL;
    struct etude_case_machine *m, *m2;
    struct etude_termination end;
    const char *none[] = {"kernel", NULL};

    expect(c, "no-step", etude_boot(c, "fixture", NULL, &m, &end), ETUDE_REFUSED);
    etude_assert(c, "no-artifact", etude_artifact(c, "nothing") == NULL, "given");
    etude_assert(c, "no-copy", etude_private_copy(c, "kernel") == NULL, "given");
    expect(c, "host-step", etude_step(c, "host"), ETUDE_OK);
    expect(c, "other-machine", etude_boot(c, "fixture", NULL, &m, &end), ETUDE_REFUSED);
    expect(c, "no-tool", etude_host(c, undeclared, "cc", &end), ETUDE_REFUSED);
    expect(c, "step-back", etude_step(c, "host"), ETUDE_REFUSED);
    expect(c, "machine-step", etude_step(c, "boot"), ETUDE_OK);
    expect(c, "other-disk", etude_boot(c, "fixture", none, &m, &end), ETUDE_REFUSED);
    if (etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    expect(c, "running", etude_boot(c, "fixture", NULL, &m2, &end), ETUDE_REFUSED);
    expect(c, "no-qmp", etude_qmp(m, "query-status", NULL, &reply), ETUDE_REFUSED);
    return 0;
}

/** Run the rows whose machine does not boot: a failed accelerator, and a
 * damaged kernel. */
static int scenario_noboot(struct etude_case *c, int damaged)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    enum etude_status st;

    if (etude_step(c, "boot") != ETUDE_OK)
        return 1;
    st = etude_boot(c, "fixture", NULL, &m, &end);
    if (damaged) {
        expect(c, "refused", st, ETUDE_REFUSED);
        etude_assert(c, "damaged", etude_artifact(c, "kernel") == NULL, "the damage was taken");
    } else {
        expect(c, "refused", st, ETUDE_FAILED);
        etude_assert(c, "damaged", end.end == ETUDE_END_ACCEL_FAILED && m == NULL,
                     "not a failed accelerator");
    }
    return 0;
}

/** Run the guest rows: a program whose report becomes the scenario's, and
 * one that kills etude-guest under it. */
static int scenario_guest(struct etude_case *c, int real)
{
    struct etude_case_machine *m;
    struct etude_termination end;

    if (etude_step(c, "run") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    expect(c, "guest-pass", etude_guest(m, "invocation", &end), ETUDE_OK);
    etude_assert(c, "guest-exit", end.end == ETUDE_END_EXITED && end.value == 0, "another end");
    if (real)
        return 0;
    expect(c, "guest-lost", etude_guest(m, "killer", &end), ETUDE_LOST);
    etude_assert(c, "guest-lost-end", end.end == ETUDE_END_LOST, "taken for another end");
    return 0;
}

/* The arguments of a service's start: the tool `peer`, the case's first
 * port, a mode, and the mark of the row in this run, ETUDE_TEST_MARK. */
struct peer_args {
    char port[16], mark[PATH_MAX + 8];
    char *argv[6];
};

/** Make the arguments of the peer in `mode` on the case's first port. */
static char *const *peer_args(struct etude_case *c, struct peer_args *a, const char *mode)
{
    snprintf(a->port, sizeof(a->port), "%u", etude_port(c, 0));
    snprintf(a->mark, sizeof(a->mark), "mark=%s", getenv("ETUDE_TEST_MARK"));
    a->argv[0] = "tool";
    a->argv[1] = "peer";
    a->argv[2] = a->port;
    a->argv[3] = (char *)mode;
    a->argv[4] = a->mark;
    a->argv[5] = NULL;
    return a->argv;
}

/** Boot `machine` in step `step` and wait for its shell; NULL when it does
 * not come. */
static struct etude_case_machine *boot_shell(struct etude_case *c, const char *step,
                                             const char *machine)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at;

    if (etude_step(c, step) != ETUDE_OK || etude_boot(c, machine, NULL, &m, &end) != ETUDE_OK ||
        etude_console_wait(m, "> ", 0, &at) != ETUDE_OK)
        return NULL;
    return m;
}

/** Have machine `m` send `text` to the peer on `port`, and report assertion
 * `id` by whether the peer's echo came back. */
static void reach(struct etude_case *c, struct etude_case_machine *m, const char *port,
                  const char *text, const char *id)
{
    char line[160];
    uint64_t at;

    snprintf(line, sizeof(line), "peer %s %s\n", port, text);
    if (etude_type(m, line) != ETUDE_OK) {
        etude_assert(c, id, 0, "the console is gone");
        return;
    }
    snprintf(line, sizeof(line), "fake: peer echoed %s", text);
    expect(c, id, etude_console_wait(m, line, 0, &at), ETUDE_OK);
}

/**
 * Run the pair row: a service that both machines reach while they run at
 * once, a loss of one that leaves the other running, and a service stopped
 * and then started again to end by itself.
 */
static int scenario_pair(struct etude_case *c)
{
    struct etude_case_service *s, *again;
    struct etude_case_machine *a, *b;
    struct etude_termination end;
    struct peer_args pa;
    char *reply = NULL;
    uint64_t at;

    if (etude_step(c, "serve") != ETUDE_OK)
        return 1;
    expect(c, "service-ready", etude_service(c, "peer", peer_args(c, &pa, "serve"), &s, &end),
           ETUDE_OK);
    if (s == NULL)
        return 1;
    expect(c, "service-running", etude_service(c, "peer", pa.argv, &again, &end), ETUDE_REFUSED);
    if ((a = boot_shell(c, "first", "alpha")) == NULL)
        return 1;
    reach(c, a, pa.port, "alpha", "alpha-reached");
    expect(c, "service-saw", etude_service_wait(s, "peer: got alpha", 0, &at), ETUDE_OK);
    if ((b = boot_shell(c, "second", "beta")) == NULL)
        return 1;
    reach(c, b, pa.port, "beta", "beta-reached");
    expect(c, "both-run", etude_qmp(a, "query-status", NULL, &reply), ETUDE_OK);
    free(reply);
    etude_type(a, "die\n");
    expect(c, "alpha-lost", etude_console_wait(a, "never printed", 0, &at), ETUDE_LOST);
    reach(c, b, pa.port, "again", "beta-survives");
    if (etude_step(c, "after") != ETUDE_OK)
        return 1;
    expect(c, "service-stopped", etude_service_stop(s, &end), ETUDE_OK);
    etude_assert(c, "stopped-end", end.end == ETUDE_END_STOPPED, "not stopped");
    expect(c, "once-ready", etude_service(c, "peer", peer_args(c, &pa, "once"), &s, &end),
           ETUDE_OK);
    if (s == NULL)
        return 1;
    reach(c, b, pa.port, "last", "once-reached");
    expect(c, "once-ended", etude_service_end(s, &end), ETUDE_OK);
    etude_assert(c, "once-exit", end.end == ETUDE_END_EXITED && end.value == 0, "another end");
    expect(c, "gone-wait", etude_service_wait(s, "never printed", 0, &at), ETUDE_LOST);
    return 0;
}

/** Run the unready row: a service that ends before it is ready, an
 * undeclared one, and one that is never ready within its step. */
static int scenario_unready(struct etude_case *c)
{
    struct etude_case_service *s;
    struct etude_termination end;
    struct peer_args pa;

    if (etude_step(c, "early") != ETUDE_OK)
        return 1;
    expect(c, "early-failed", etude_service(c, "peer", peer_args(c, &pa, "early"), &s, &end),
           ETUDE_FAILED);
    etude_assert(c, "early-end", s == NULL && end.end == ETUDE_END_EXITED && end.value == 3,
                 "another end");
    expect(c, "undeclared", etude_service(c, "nothing", pa.argv, &s, &end), ETUDE_REFUSED);
    etude_assert(c, "no-port", etude_port(c, 1) == 0, "a port the case lacks");
    if (etude_step(c, "mute") != ETUDE_OK)
        return 1;
    expect(c, "mute-bounded", etude_service(c, "peer", peer_args(c, &pa, "mute"), &s, &end),
           ETUDE_BOUND);
    etude_assert(c, "mute-stopped", s == NULL && end.end == ETUDE_END_STOPPED, "not stopped");
    return 0;
}

/** Start the peer in step `serve` and boot alpha in step `first`, and then
 * beta in step `second` when `both`; 0, or 1 when a start failed. */
static int serve_and_boot(struct etude_case *c, struct peer_args *pa, int both,
                          struct etude_case_service **s, struct etude_case_machine **a,
                          struct etude_case_machine **b)
{
    struct etude_termination end;

    if (etude_step(c, "serve") != ETUDE_OK)
        return 1;
    expect(c, "service-ready", etude_service(c, "peer", peer_args(c, pa, "serve"), s, &end),
           ETUDE_OK);
    if (*s == NULL || (*a = boot_shell(c, "first", "alpha")) == NULL)
        return 1;
    if (both && (*b = boot_shell(c, "second", "beta")) == NULL)
        return 1;
    return 0;
}

/**
 * Run the rows that fail beside a service and machines that run: an
 * artifact whose object is damaged (`broken`), a wait past its step's bound
 * (`bound-pair`), a stop or a kill during a wait (`stop-pair`, `kill-pair`),
 * and a service that dies unseen (`unseen`).
 */
static int scenario_fail(struct etude_case *c, const char *row)
{
    char *broken[] = {"broken", NULL};
    struct etude_case_service *s;
    struct etude_case_machine *a, *b = NULL;
    struct etude_termination end;
    struct peer_args pa;
    char line[64];
    uint64_t at;

    if (serve_and_boot(c, &pa, strcmp(row, "broken") != 0 && strcmp(row, "unseen") != 0, &s, &a,
                       &b) != 0)
        return 1;
    if (strcmp(row, "broken") == 0) {
        expect(c, "broken-refused", etude_host(c, broken, "broken", &end), ETUDE_REFUSED);
        return 1;
    }
    if (strcmp(row, "unseen") == 0) {
        snprintf(line, sizeof(line), "peer %s die\n", pa.port);
        etude_type(a, line);
        expect(c, "peer-gone", etude_console_wait(a, "fake: peer gone", 0, &at), ETUDE_OK);
        return 0;
    }
    if (etude_step(c, "wait") != ETUDE_OK)
        return 1;
    if (strcmp(row, "bound-pair") == 0) {
        expect(c, "bounded", etude_console_wait(b, "never printed", 0, &at), ETUDE_BOUND);
        return 1;
    }
    etude_progress(c, "waiting", 0);
    expect(c, "stopped", etude_console_wait(b, "never printed", 0, &at), ETUDE_STOPPED);
    return 0;
}

/**
 * Run the hold row, the case that must not notice the others: its service
 * and both machines run while the other rows fail, and then still answer.
 */
static int scenario_hold(struct etude_case *c)
{
    struct etude_case_service *s;
    struct etude_case_machine *a, *b;
    struct etude_termination end;
    struct peer_args pa;
    uint64_t at;

    if (serve_and_boot(c, &pa, 1, &s, &a, &b) != 0 || etude_step(c, "wait") != ETUDE_OK)
        return 1;
    etude_progress(c, "waiting", 0);
    expect(c, "released", etude_service_wait(s, "peer: got done", 0, &at), ETUDE_OK);
    reach(c, a, pa.port, "alpha", "alpha-alive");
    reach(c, b, pa.port, "beta", "beta-alive");
    expect(c, "service-stopped", etude_service_stop(s, &end), ETUDE_OK);
    return 0;
}

/**
 * Run the real peer row: two machines at once run the test image's tcptest
 * against the service, which echoes, and both connections close.
 */
static int scenario_real_peer(struct etude_case *c)
{
    struct etude_case_machine *a, *b;
    struct etude_case_service *s;
    struct etude_termination end;
    struct peer_args pa;
    char line[64];
    uint64_t at, aa, ba;

    if (etude_step(c, "serve") != ETUDE_OK)
        return 1;
    expect(c, "service-ready", etude_service(c, "peer", peer_args(c, &pa, "serve"), &s, &end),
           ETUDE_OK);
    if (s == NULL || etude_step(c, "alpha") != ETUDE_OK ||
        etude_boot(c, "alpha", NULL, &a, &end) != ETUDE_OK || etude_step(c, "beta") != ETUDE_OK ||
        etude_boot(c, "beta", NULL, &b, &end) != ETUDE_OK)
        return 1;
    snprintf(line, sizeof(line), "exec /tcptest %s\r", pa.port);
    aa = wait_for(c, a, "alpha-tcp", "\n> ", wait_for(c, a, "alpha-tcp", "Type 'help'", 0));
    etude_type(a, line);
    ba = wait_for(c, b, "beta-tcp", "\n> ", wait_for(c, b, "beta-tcp", "Type 'help'", 0));
    etude_type(b, line);
    expect(c, "alpha-tcp", etude_console_wait(a, "All TCP tests passed!", aa, &at), ETUDE_OK);
    expect(c, "beta-tcp", etude_console_wait(b, "All TCP tests passed!", ba, &at), ETUDE_OK);
    if (etude_service_wait(s, "peer: closed", 0, &at) == ETUDE_OK)
        expect(c, "both-closed", etude_service_wait(s, "peer: closed", at, &at), ETUDE_OK);
    else
        etude_assert(c, "both-closed", 0, "no connection closed");
    expect(c, "service-stopped", etude_service_stop(s, &end), ETUDE_OK);
    return 0;
}

/** Run row `row` of a case with services, or give 1 for an unknown row. */
static int scenario_services(struct etude_case *c, const char *row)
{
    if (strcmp(row, "pair") == 0)
        return scenario_pair(c);
    if (strcmp(row, "unready") == 0)
        return scenario_unready(c);
    if (strcmp(row, "broken") == 0 || strcmp(row, "bound-pair") == 0 ||
        strcmp(row, "stop-pair") == 0 || strcmp(row, "kill-pair") == 0 ||
        strcmp(row, "unseen") == 0)
        return scenario_fail(c, row);
    if (strcmp(row, "hold") == 0)
        return scenario_hold(c);
    if (strcmp(row, "real-peer") == 0)
        return scenario_real_peer(c);
    return 1;
}

int etude_scenario(struct etude_case *c)
{
    const char *row = getenv("ETUDE_TEST_ROW");

    if (row == NULL)
        return 1;
    if (strcmp(row, "reboot") == 0 || strcmp(row, "real-reboot") == 0)
        return scenario_reboot(c, row[0] == 'r' && row[1] == 'e' && row[2] == 'a');
    if (strcmp(row, "lost") == 0 || strcmp(row, "die") == 0 || strcmp(row, "closecon") == 0 ||
        strcmp(row, "closeqmp") == 0)
        return scenario_lost(c, row);
    if (strcmp(row, "bound") == 0)
        return scenario_bound(c);
    if (strcmp(row, "stop") == 0)
        return scenario_stop(c);
    if (strcmp(row, "misuse") == 0)
        return scenario_misuse(c);
    if (strcmp(row, "wrong") == 0 || strcmp(row, "damaged") == 0)
        return scenario_noboot(c, strcmp(row, "damaged") == 0);
    if (strcmp(row, "guest") == 0 || strcmp(row, "real-guest") == 0)
        return scenario_guest(c, row[0] == 'r');
    return scenario_services(c, row);
}

/* ---- The rows. ---- */

/* One attempt, as the test plans and judges it. */
struct row {
    const char *name;    /* the row, ETUDE_TEST_ROW, and the case's last part */
    const char *mode;    /* the stand-in's mode, or NULL for a real QEMU */
    const char *steps;   /* the case's steps, `id:machine` or `id`, by spaces */
    const char *asserts; /* its assertions, by spaces */
    const char *disk;    /* the machine's disk, or NULL */
    int helper;          /* the machine runs etude-guest */
    uint64_t bound_us;   /* each step's bound */
    int want_status;     /* the scenario's exit status */
    int want_end;        /* its report ends with `end` */
    int want_kept;       /* it keeps its scratch */
    int pair;            /* the case has machines alpha and beta, the service
                            peer and a port */
    const char *odd;     /* a step whose bound is `odd_us`, or NULL */
    uint64_t odd_us;
    const char *evidence; /* `FILE=TEXT` pairs by `;`, which a kept scratch's
                             evidence must have */
    int stop_sig;         /* what a stop sends: SIGTERM when 0 */
};

/* The port of the row that runs, for its invocation and its peer. */
static unsigned row_port;

/** Put `path` in the store as an artifact, its digest in `sha`. */
static int stored(struct etude_store *s, const char *path, char sha[65])
{
    if (etude_store_put(s, path, NULL, sha, NULL) != 0) {
        printf("etude-scenario-test: fail: %s cannot be stored: %s\n", path, strerror(errno));
        fails++;
        return -1;
    }
    return 0;
}

/** Write `text` to the file at `path`. */
static int write_file(const char *path, const char *text, size_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (f == NULL)
        return -1;
    ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/** Give the SHA-256 of the file at `path` in `hex` and its size. */
static long file_sha(const char *path, char hex[65])
{
    struct sha256 h;
    unsigned char d[32], buf[65536];
    FILE *f = fopen(path, "rb");
    size_t n;
    long size = 0;

    if (f == NULL)
        return -1;
    sha256_init(&h);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sha256_update(&h, buf, n);
        size += (long)n;
    }
    fclose(f);
    sha256_final(&h, d);
    sha256_hex(d, hex);
    return size;
}

/**
 * Write the guest invocation of the case `case_id` that runs `program` of
 * digest `sha` and `size` bytes with `args`, a JSON list's inside, to
 * `path`.
 */
static int write_invocation(const char *path, const char *case_id, const char *token,
                            const char *program, const char *sha, long size, const char *args)
{
    char text[4096];

    fix_format(text, sizeof(text),
               "{\"schema_version\": 1, \"run_id\": \"test-run\", \"attempt_id\": \"a-%s\", "
               "\"attempt_token\": \"%s\", \"case_id\": \"%s\", \"program\": \"%s\", "
               "\"arguments\": [%s], \"environment\": {\"PATH\": \"/bin\", \"HOME\": "
               "\"${WORKSPACE}\"}, \"stdin\": null, \"role\": \"ordinary\", \"files\": "
               "[{\"name\": \"%s\", \"sha256\": \"%s\", \"size_bytes\": %ld, \"mode\": \"0755\"}], "
               "\"fixtures\": [{\"fixture\": \"workspace\", \"given\": []}], \"deadline_us\": "
               "60000000, \"grace_us\": 1000000, \"output_max_bytes\": 65536}",
               token, token, case_id, program, args, program, sha, size);
    return write_file(path, text, strlen(text));
}

/**
 * Append to `out` the JSON list of `words`, separated by spaces, each made
 * by `item`; a step's word is `ID` or `ID:MACHINE`.
 */
static void json_list(char *out, size_t size, const char *words, int steps)
{
    char word[64], one[512], *colon;
    const char *p = words;
    size_t n;

    fix_format(out, size, "[");
    while (*p != '\0') {
        n = strcspn(p, " ");
        snprintf(word, sizeof(word), "%.*s", (int)n, p);
        p += n + (p[n] == ' ');
        colon = strchr(word, ':');
        if (colon != NULL)
            *colon = '\0';
        if (steps)
            fix_format(one, sizeof(one),
                       "{\"step_id\": \"%s\", \"stage\": \"execute\", \"does\": \"a step\"%s%s%s}",
                       word, colon != NULL ? ", \"machine\": \"" : "",
                       colon != NULL ? colon + 1 : "", colon != NULL ? "\"" : "");
        else
            fix_format(one, sizeof(one),
                       "{\"assertion_id\": \"%s\", \"checks\": \"a check\", \"source\": "
                       "\"scenario\"}",
                       word);
        if (strlen(out) + strlen(one) + 4 >= size)
            exit(1);
        if (out[1] != '\0')
            strcat(out, ", ");
        strcat(out, one);
    }
    strcat(out, "]");
}

/** Write row `r`'s case, as one family's manifest, to `path`. */
static int write_manifest(const char *path, const struct row *r)
{
    char steps[2048], asserts[4096], text[16384];

    json_list(steps, sizeof(steps), r->steps, 1);
    json_list(asserts, sizeof(asserts), r->asserts, 0);
    fix_format(
        text, sizeof(text),
        "{\"schema_version\": 1, \"family_id\": \"test.scenario\", \"summary\": \"rows\", "
        "\"cases\": [{\"case_id\": \"test.scenario.%s\", \"behavior\": \"a row\", \"units\": "
        "[\"none\"], \"domain\": \"machine\", \"architectures\": [\"i686\", \"x86_64\"], "
        "\"workload\": {\"arguments\": [], \"inputs\": {}, \"repetitions\": 1, \"seed\": null}, "
        "\"requirements\": {\"host_api\": \"c\", \"tools\": [], \"features\": {}}, "
        "\"execution\": {\"program\": \"scenario\", \"stdin\": null, \"role\": \"ordinary\", "
        "\"privileges\": \"an ordinary user\", \"isolation\": [{\"fixture\": \"scratch\"}, "
        "{\"fixture\": \"disk\", \"names\": [\"disk\"]}], \"children\": \"none\", \"cleanup\": "
        "\"the library\"}, \"steps\": %s, \"machines\": [{\"machine_id\": \"fixture\", "
        "\"qemu\": {\"kernel\": \"kernel\", \"initrd\": %s, \"disks\": %s, \"cpu\": \"max\", "
        "\"processors\": 1, \"memory_bytes\": 134217728, \"firmware\": \"direct\", \"devices\": "
        "[], \"boot_options\": %s}, \"console\": \"serial\", \"qmp\": %s, \"helper\": %s}], "
        "\"assertions\": %s, \"branches\": [], \"termination\": {\"end\": \"exited\", \"value\": "
        "0}, \"progress\": [{\"witness_id\": \"stage\", \"kind\": \"step\", \"means\": \"a step "
        "ended\", \"limit\": 8}, {\"witness_id\": \"waiting\", \"kind\": \"quiet\", \"means\": "
        "\"a wait\", \"limit\": null, \"bound\": {\"floor_us\": 1, \"initial_us\": 2, "
        "\"ceiling_us\": 3}}], \"bounds\": {\"case\": {\"floor_us\": 1, \"initial_us\": 2, "
        "\"ceiling_us\": 3}, \"gap\": {\"floor_us\": 1, \"initial_us\": 2, \"ceiling_us\": 3}, "
        "\"stages\": {}}, \"resources\": {\"cpu_millis\": 1000, \"cpu_ceiling_millis\": 1000, "
        "\"memory_bytes\": 268435456, \"disk_bytes\": 67108864, \"locks\": [], \"ports\": 0, "
        "\"loop_devices\": 0}, \"prepare\": [{\"name\": \"kernel\", \"node\": \"k\", "
        "\"per_architecture\": true}, {\"name\": \"disk\", \"node\": \"d\", \"per_architecture\": "
        "true}, {\"name\": \"initrd\", \"node\": \"i\", \"per_architecture\": true}, {\"name\": "
        "\"scanner\", \"node\": \"s\", \"per_architecture\": false}, {\"name\": \"invocation\", "
        "\"node\": \"v\", \"per_architecture\": false}, {\"name\": \"killer\", \"node\": \"x\", "
        "\"per_architecture\": false}], \"timing\": [], \"native\": {\"plan\": \"none\", "
        "\"class\": \"boot\", \"reason\": \"a test\"}}]}",
        r->name, steps, r->helper ? "\"initrd\"" : "null", r->disk != NULL ? "[\"disk\"]" : "[]",
        r->mode == NULL && r->disk != NULL ? "[\"rach2.fsdisk\"]" : "[]",
        strcmp(r->name, "misuse") == 0 ? "false" : "true", r->helper ? "true" : "false", asserts);
    return write_file(path, text, strlen(text));
}

/** Write row `r`'s case of two machines and a service to `path`: under the
 * stand-in each machine has a disk of its own, and under the real QEMU each
 * boots the test image with a network card. */
static int write_pair_manifest(const char *path, const struct row *r)
{
    char steps[2048], asserts[4096], machines[2048], text[16384];
    int real = r->mode == NULL;

    json_list(steps, sizeof(steps), r->steps, 1);
    json_list(asserts, sizeof(asserts), r->asserts, 0);
    fix_format(
        machines, sizeof(machines),
        "[{\"machine_id\": \"alpha\", \"qemu\": {\"kernel\": \"kernel\", \"initrd\": %s, "
        "\"disks\": %s, \"cpu\": \"max\", \"processors\": 1, \"memory_bytes\": 134217728, "
        "\"firmware\": \"direct\", \"devices\": %s, \"boot_options\": []}, \"console\": "
        "\"serial\", \"qmp\": true, \"helper\": false}, {\"machine_id\": \"beta\", \"qemu\": "
        "{\"kernel\": \"kernel\", \"initrd\": %s, \"disks\": %s, \"cpu\": \"max\", "
        "\"processors\": 1, \"memory_bytes\": 134217728, \"firmware\": \"direct\", "
        "\"devices\": %s, \"boot_options\": []}, \"console\": \"serial\", \"qmp\": true, "
        "\"helper\": false}]",
        real ? "\"initrd\"" : "null", real ? "[]" : "[\"disk\"]", real ? "[\"rtl8139\"]" : "[]",
        real ? "\"initrd\"" : "null", real ? "[]" : "[\"disk2\"]", real ? "[\"rtl8139\"]" : "[]");
    fix_format(
        text, sizeof(text),
        "{\"schema_version\": 1, \"family_id\": \"test.scenario\", \"summary\": \"rows\", "
        "\"cases\": [{\"case_id\": \"test.scenario.%s\", \"behavior\": \"a row\", \"units\": "
        "[\"none\"], \"domain\": \"machine\", \"architectures\": [\"i686\", \"x86_64\"], "
        "\"workload\": {\"arguments\": [], \"inputs\": {}, \"repetitions\": 1, \"seed\": null}, "
        "\"requirements\": {\"host_api\": \"c\", \"tools\": [], \"features\": {}}, "
        "\"execution\": {\"program\": \"scenario\", \"stdin\": null, \"role\": \"ordinary\", "
        "\"privileges\": \"an ordinary user\", \"isolation\": [{\"fixture\": \"scratch\"}, "
        "{\"fixture\": \"disk\", \"names\": [\"disk\", \"disk2\"]}], \"children\": \"none\", "
        "\"cleanup\": \"the library\"}, \"steps\": %s, \"machines\": %s, \"services\": "
        "[{\"service_id\": \"peer\", \"program\": \"scanner\", \"ready\": \"peer: listening\", "
        "\"does\": \"echo what the machines send to the case's port\"}], \"assertions\": %s, "
        "\"branches\": [], \"termination\": {\"end\": \"exited\", \"value\": 0}, \"progress\": "
        "[{\"witness_id\": \"stage\", \"kind\": \"step\", \"means\": \"a step ended\", "
        "\"limit\": 8}, {\"witness_id\": \"waiting\", \"kind\": \"quiet\", \"means\": \"a "
        "wait\", \"limit\": null, \"bound\": {\"floor_us\": 1, \"initial_us\": 2, "
        "\"ceiling_us\": 3}}], \"bounds\": {\"case\": {\"floor_us\": 1, \"initial_us\": 2, "
        "\"ceiling_us\": 3}, \"gap\": {\"floor_us\": 1, \"initial_us\": 2, \"ceiling_us\": 3}, "
        "\"stages\": {}}, \"resources\": {\"cpu_millis\": 1000, \"cpu_ceiling_millis\": 1000, "
        "\"memory_bytes\": 268435456, \"disk_bytes\": 67108864, \"locks\": [], \"ports\": 1, "
        "\"loop_devices\": 0}, \"prepare\": [{\"name\": \"kernel\", \"node\": \"k\", "
        "\"per_architecture\": true}, {\"name\": \"disk\", \"node\": \"d\", \"per_architecture\": "
        "true}, {\"name\": \"disk2\", \"node\": \"d\", \"per_architecture\": true}, {\"name\": "
        "\"initrd\", \"node\": \"i\", \"per_architecture\": true}, {\"name\": \"scanner\", "
        "\"node\": \"s\", \"per_architecture\": false}, {\"name\": \"broken\", \"node\": \"b\", "
        "\"per_architecture\": false}], \"timing\": [], \"native\": {\"plan\": \"none\", "
        "\"class\": \"boot\", \"reason\": \"a test\"}}]}",
        r->name, steps, machines, asserts);
    return write_file(path, text, strlen(text));
}

/* Where a row runs QEMU, its store and the digests of its artifacts. */
struct site {
    struct etude_store *store;
    char store_dir[PATH_MAX];
    char qemu_i686[PATH_MAX], qemu_x86_64[PATH_MAX];
    const char *arch;
    char kernel[65], disk[65], initrd[65], scanner[65], inv[65], killer[65], broken[65];
};

/** Write row `r`'s invocation to `path`, with the scratch `scratch`. */
static int write_scenario(const char *path, const struct row *r, const struct site *s,
                          const char *manifest, const char *scratch)
{
    char steps[1024] = "", one[160], word[64], text[8192], ports[16] = "";
    const char *p = r->steps;
    size_t n;

    while (*p != '\0') {
        n = strcspn(p, " :");
        snprintf(word, sizeof(word), "%.*s", (int)n, p);
        p += strcspn(p, " ");
        p += *p == ' ';
        fix_format(one, sizeof(one), "%s\"%s\": %llu", steps[0] != '\0' ? ", " : "", word,
                   (unsigned long long)(r->odd != NULL && strcmp(word, r->odd) == 0 ? r->odd_us
                                                                                    : r->bound_us));
        strcat(steps, one);
    }
    if (r->pair)
        fix_format(ports, sizeof(ports), "%u", row_port);
    fix_format(text, sizeof(text),
               "{\"schema_version\": 1, \"run_id\": \"test-run\", \"attempt_id\": \"test-%s\", "
               "\"manifest\": \"%s\", \"case_id\": \"test.scenario.%s\", \"architecture\": "
               "\"%s\", \"store\": \"%s\", \"artifacts\": {\"kernel\": \"%s\", \"disk\": \"%s\", "
               "\"initrd\": \"%s\", \"scanner\": \"%s\", \"invocation\": \"%s\", \"killer\": "
               "\"%s\", \"disk2\": \"%s\", \"broken\": \"%s\"}, \"tools\": {}, \"site\": "
               "{\"accel\": \"tcg\", \"qemu_i686\": \"%s\", \"qemu_x86_64\": \"%s\", "
               "\"ovmf_i686\": null, \"ovmf_x86_64\": null}, \"steps\": {%s}, \"ports\": [%s], "
               "\"cleanup_us\": 30000000, \"scratch\": \"%s\", \"grace_us\": %u, "
               "\"console_max_bytes\": 16777216, \"output_max_bytes\": 1048576, "
               "\"resend_us\": 200000}",
               r->name, manifest, r->name, s->arch, s->store_dir, s->kernel, s->disk, s->initrd,
               s->scanner, s->inv, s->killer, s->disk, s->broken, s->qemu_i686, s->qemu_x86_64,
               steps, ports, scratch, r->mode != NULL ? 300000u : 5000000u);
    return write_file(path, text, strlen(text));
}

/* What one attempt gave. */
struct outcome {
    char out[65536]; /* its standard output */
    size_t len;
    int status; /* its exit status, or -1 */
    int kept;   /* it said that it kept its scratch */
};

/** Give the monotonic clock in milliseconds. */
static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The controller's bound TCP socket and the Unix listener that passes it. */
struct peer_fixture {
    int tcp, handoff;
    char path[PATH_MAX];
};

/** Pass `peer`'s TCP socket to one service that has connected to it. */
static void peer_handoff(struct peer_fixture *peer)
{
    struct msghdr msg;
    struct cmsghdr *c;
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    char byte = 0;
    struct iovec iov = {&byte, 1};
    struct sigaction ignore, old;
    int connection = accept(peer->handoff, NULL, NULL);

    if (connection < 0) {
        CHECK(0, "the peer's socket request can be accepted");
        return;
    }
    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);
    c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(peer->tcp));
    memcpy(CMSG_DATA(c), &peer->tcp, sizeof(peer->tcp));
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &old) != 0) {
        CHECK(0, "the peer's socket transfer can ignore sigpipe");
        close(connection);
        return;
    }
    CHECK(sendmsg(connection, &msg, 0) == 1, "the peer receives its bound socket");
    sigaction(SIGPIPE, &old, NULL);
    close(connection);
}

/* One attempt that runs: its process, the pipe of its output, and the time
 * past which it is killed. */
struct attempt {
    const struct row *r;
    pid_t pid;
    int fd;
    int limit;
    uint64_t bound;
    int stopped;
    char err[PATH_MAX];
    struct outcome *o;
    struct peer_fixture *peer;
};

/**
 * Start the scenario of row `r` with invocation `inv` and the environment
 * `env`, its completion bound `limit` milliseconds from now.
 */
static void attempt_start(struct attempt *a, const struct row *r, const char *inv, char **env,
                          int limit, struct outcome *o)
{
    char *argv[] = {self, (char *)inv, NULL};
    int out[2], fd;

    memset(a, 0, sizeof(*a));
    memset(o, 0, sizeof(*o));
    a->r = r;
    a->o = o;
    a->fd = -1;
    a->limit = limit;
    a->bound = now_ms() + (uint64_t)limit;
    o->status = -1;
    fix_format(a->err, sizeof(a->err), "%s/%s.err", fix_dir(), r->name);
    if (pipe(out) != 0)
        return;
    a->pid = fork();
    if (a->pid == 0) {
        fd = open(a->err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(out[1], 1);
        dup2(fd, 2);
        close(out[0]);
        close(out[1]);
        close(fd);
        execve(self, argv, env);
        _exit(127);
    }
    close(out[1]);
    a->fd = out[0];
}

/**
 * Read the attempt's output until it has `want`, which gives 1, or ends or
 * passes `until` milliseconds, 0 for its completion bound, which gives 0;
 * when `stop_at` is not NULL, send the row's stop signal once the output has
 * it. Past the completion bound the attempt is killed.
 */
static int attempt_read(struct attempt *a, const char *want, const char *stop_at, uint64_t until)
{
    struct outcome *o = a->o;
    struct pollfd p[2];
    ssize_t n;

    while (a->fd >= 0) {
        if (want != NULL && strstr(o->out, want) != NULL)
            return 1;
        if (until != 0 && now_ms() > until)
            return 0;
        p[0].fd = a->fd;
        p[1].fd = a->peer->handoff;
        p[0].events = p[1].events = POLLIN;
        if (now_ms() > a->bound) {
            printf("etude-scenario-test: fail: %s: the attempt did not end within %d ms\n",
                   a->r->name, a->limit);
            fails++;
            kill(a->pid, SIGKILL);
            break;
        }
        if (poll(p, 2, 1000) <= 0)
            continue;
        if (p[1].revents & POLLIN)
            peer_handoff(a->peer);
        if (p[0].revents == 0)
            continue;
        n = read(a->fd, o->out + o->len, sizeof(o->out) - 1 - o->len);
        if (n <= 0)
            break;
        o->len += (size_t)n;
        o->out[o->len] = '\0';
        if (stop_at != NULL && !a->stopped && strstr(o->out, stop_at) != NULL) {
            a->stopped = 1;
            kill(a->pid, a->r->stop_sig != 0 ? a->r->stop_sig : SIGTERM);
        }
    }
    return 0;
}

/** Read the attempt's output to its end, reap it, and read its stderr. */
static void attempt_end(struct attempt *a, const char *stop_at)
{
    struct outcome *o = a->o;
    char line[512];
    FILE *f;
    int st;

    attempt_read(a, NULL, stop_at, 0);
    if (a->fd >= 0)
        close(a->fd);
    a->fd = -1;
    if (a->pid > 0 && waitpid(a->pid, &st, 0) == a->pid && WIFEXITED(st))
        o->status = WEXITSTATUS(st);
    f = fopen(a->err, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        o->kept |= strncmp(line, "etude-case: kept: ", 18) == 0;
        if (fails > 0 || getenv("ETUDE_TEST_VERBOSE") != NULL)
            printf("    %s: stderr: %s", a->r->name, line);
    }
    fclose(f);
}

/** Say whether the output `o` has the record `rec`, a line of its own. */
static int has_record(const struct outcome *o, const char *rec)
{
    char line[300];

    snprintf(line, sizeof(line), "@etude 1 %s\n", rec);
    return strstr(o->out, line) != NULL;
}

/** Check that each `FILE=TEXT` of row `r`'s evidence is in the evidence of
 * its kept scratch `scratch`. */
static void judge_evidence(const struct row *r, const char *scratch)
{
    char pair[256], path[PATH_MAX], *eq;
    const char *p = r->evidence;
    size_t n;

    while (p != NULL && *p != '\0') {
        n = strcspn(p, ";");
        snprintf(pair, sizeof(pair), "%.*s", (int)n, p);
        p += n + (p[n] == ';');
        eq = strchr(pair, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        fix_format(path, sizeof(path), "%s/evidence/%s", scratch, pair);
        if (!file_has(path, eq + 1)) {
            printf("etude-scenario-test: fail: %s: evidence %s lacks `%s`\n", r->name, pair,
                   eq + 1);
            fails++;
        }
    }
}

/** Check outcome `o` of row `r`: each assertion passed, the end record and
 * the exit status are as the row says, and the scratch is kept or not. */
static void judge(const struct row *r, const struct outcome *o, const char *scratch)
{
    char word[64], rec[160], copies[4352];
    const char *p = r->asserts;
    struct stat st;
    size_t n;

    snprintf(rec, sizeof(rec), "begin test.scenario.%s", r->name);
    CHECK(has_record(o, rec), r->name);
    while (*p != '\0') {
        n = strcspn(p, " ");
        snprintf(word, sizeof(word), "%.*s", (int)n, p);
        p += n + (p[n] == ' ');
        snprintf(rec, sizeof(rec), "assert %s pass", word);
        if (!has_record(o, rec)) {
            printf("etude-scenario-test: fail: %s: no `%s`\n", r->name, rec);
            fails++;
        }
    }
    CHECK(strstr(o->out, " fail ") == NULL, r->name);
    CHECK((strstr(o->out, "@etude 1 end ") != NULL) == r->want_end, r->name);
    CHECK(o->status == r->want_status, r->name);
    CHECK(o->kept == r->want_kept, r->name);
    snprintf(copies, sizeof(copies), "%s/copies", scratch);
    CHECK((stat(copies, &st) == 0) == (r->want_kept && r->disk != NULL), r->name);
    if (r->want_kept)
        judge_evidence(r, scratch);
    if (fails > 0 || getenv("ETUDE_TEST_VERBOSE") != NULL)
        printf("    %s: stdout:\n%s", r->name, o->out);
}

/** Make the environment of row `r`'s scenario: this program's, with the
 * row, the nonce, the stand-in's mode and its files, and the mark of the
 * row's services, a path in this run's fixture directory. */
static char **row_env(const struct row *r, char *bufs[6])
{
    size_t n = 0, i;
    char **env;

    while (environ[n] != NULL)
        n++;
    env = calloc(n + 7, sizeof(*env));
    if (env == NULL)
        exit(1);
    for (i = 0; i < n; i++)
        env[i] = environ[i];
    for (i = 0; i < 6; i++) {
        bufs[i] = malloc(PATH_MAX + 64);
        if (bufs[i] == NULL)
            exit(1);
    }
    snprintf(bufs[0], PATH_MAX, "ETUDE_TEST_ROW=%s", r->name);
    snprintf(bufs[1], PATH_MAX, "ETUDE_TEST_NONCE=etude-nonce-%ld-%s", (long)getpid(), r->name);
    snprintf(bufs[2], PATH_MAX, "ETUDE_FAKE_QEMU=%s", r->mode != NULL ? r->mode : "");
    snprintf(bufs[3], PATH_MAX, "ETUDE_FAKE_STAGE=%s", fix_dir());
    snprintf(bufs[4], PATH_MAX, "ETUDE_FAKE_GUEST_PID=%s/%s.guest", fix_dir(), r->name);
    snprintf(bufs[5], PATH_MAX, "ETUDE_TEST_MARK=%s/%s", fix_dir(), r->name);
    for (i = 0; i < 6; i++)
        env[n + i] = bufs[i];
    return env;
}

/**
 * Bind a TCP socket for row `mark` and a Unix listener that passes it to
 * each peer, in `peer`. Keep the TCP socket bound until the row ends, even
 * between service instances. Give its port, or 0; the caller closes both
 * descriptors and removes `peer->path` on every exit path.
 */
static unsigned reserve_peer(struct peer_fixture *peer, const char *mark)
{
    struct sockaddr_in a = loopback(0);
    socklen_t len = sizeof(a);

    peer->tcp = socket(AF_INET, SOCK_STREAM, 0);
    peer->handoff = socket(AF_UNIX, SOCK_STREAM, 0);
    if (peer->tcp < 0 || peer->handoff < 0 || fcntl(peer->tcp, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(peer->handoff, F_SETFD, FD_CLOEXEC) != 0 ||
        bind(peer->tcp, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(peer->tcp, (struct sockaddr *)&a, &len) != 0 ||
        peer_endpoint(peer->handoff, mark, 1) != 0)
        return 0;
    fix_format(peer->path, sizeof(peer->path), "%s.peer", mark);
    return listen(peer->handoff, 1) == 0 ? ntohs(a.sin_port) : 0;
}

/* A row that runs, with what its files and its judgement need. */
struct row_run {
    const struct row *r;
    struct attempt a;
    struct outcome *o;
    unsigned port;
    struct peer_fixture peer;
    char scratch[PATH_MAX];
    char *bufs[6];
    char **env;
    int started;
};

/** Write row `r`'s files on site `s` and start its attempt in `rr`. */
static void row_start(struct row_run *rr, const struct row *r, const struct site *s, int limit)
{
    char manifest[PATH_MAX], inv[PATH_MAX], mark[PATH_MAX];

    memset(rr, 0, sizeof(*rr));
    rr->peer.tcp = rr->peer.handoff = -1;
    rr->r = r;
    rr->env = row_env(r, rr->bufs);
    rr->o = malloc(sizeof(*rr->o));
    if (rr->o == NULL)
        exit(1);
    fix_format(mark, sizeof(mark), "%s/%s", fix_dir(), r->name);
    rr->port = row_port = r->pair ? reserve_peer(&rr->peer, mark) : 0;
    fix_format(manifest, sizeof(manifest), "%s/%s.manifest.json", fix_dir(), r->name);
    fix_format(inv, sizeof(inv), "%s/%s.invocation.json", fix_dir(), r->name);
    fix_format(rr->scratch, sizeof(rr->scratch), "%s/%s.scratch", fix_dir(), r->name);
    if ((r->pair && row_port == 0) || mkdir(rr->scratch, 0700) != 0 ||
        (r->pair ? write_pair_manifest(manifest, r) : write_manifest(manifest, r)) != 0 ||
        write_scenario(inv, r, s, manifest, rr->scratch) != 0) {
        printf("etude-scenario-test: fail: %s: its files cannot be written\n", r->name);
        fails++;
        return;
    }
    if (limit == 0)
        limit = r->mode != NULL ? FAKE_ATTEMPT_MS : REAL_ATTEMPT_MS;
    attempt_start(&rr->a, r, inv, rr->env, limit, rr->o);
    rr->a.peer = &rr->peer;
    rr->started = 1;
}

/** Finish the row of `rr`, judge it unless `judged` is 0, and free it. */
static void row_finish(struct row_run *rr, const char *stop_at, int judged)
{
    int i;

    if (rr->started) {
        attempt_end(&rr->a, stop_at);
        if (judged)
            judge(rr->r, rr->o, rr->scratch);
    }
    if (rr->peer.tcp >= 0)
        close(rr->peer.tcp);
    if (rr->peer.handoff >= 0)
        close(rr->peer.handoff);
    if (rr->peer.path[0] != '\0')
        unlink(rr->peer.path);
    for (i = 0; i < 6; i++)
        free(rr->bufs[i]);
    free(rr->env);
    free(rr->o);
}

/** Run row `r` on site `s`, and judge it; `stop_at` as attempt_read() takes
 * it. */
static void run_row(const struct row *r, const struct site *s, const char *stop_at)
{
    struct row_run rr;

    row_start(&rr, r, s, 0);
    row_finish(&rr, stop_at, 1);
}

/** Order two PIDs for qsort. */
static int pid_order(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;

    return x < y ? -1 : x > y;
}

/**
 * Put in `pids`, sorted, the processes that run with one of the texts of
 * `any`, a list that NULL ends, in their arguments, and with `need` too when
 * it is not NULL; give their count, or -1 when ps cannot be read.
 *
 * ps lists every process of the machine, among them those of other runs of
 * this test, which ci's runners and the worker copies start at the same
 * time. Each caller's texts contain the path of this run's fixture
 * directory, which no other run has, so that a run neither counts nor kills
 * another's processes.
 */
static int find_procs(const char *need, const char *const any[], long pids[ROW_PROCS])
{
    char line[8192];
    FILE *f = popen("ps -ww -eo pid=,args=", "r");
    int n = 0, i;

    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        for (i = 0; any[i] != NULL; i++)
            if (strstr(line, any[i]) != NULL)
                break;
        if (any[i] != NULL && (need == NULL || strstr(line, need) != NULL) && n < ROW_PROCS)
            pids[n++] = strtol(line, NULL, 10);
    }
    if (pclose(f) != 0)
        return -1;
    qsort(pids, (size_t)n, sizeof(pids[0]), pid_order);
    return n;
}

/**
 * Put in `pids`, sorted, the processes of row `name` in this run, found by
 * their arguments: the scenario and its keepers by its invocation, the
 * stand-ins for QEMU and the services by the copies and programs in its
 * scratch, and its services by their mark; give their count, or -1 when ps
 * cannot be read. Each of the three contains a path in this run's fixture
 * directory.
 */
static int row_procs(const char *name, long pids[ROW_PROCS])
{
    char inv[PATH_MAX + 64], scratch[PATH_MAX + 64], mark[PATH_MAX + 64];
    const char *any[] = {inv, scratch, mark, NULL};

    fix_format(inv, sizeof(inv), "%s/%s.invocation.json", fix_dir(), name);
    fix_format(scratch, sizeof(scratch), "%s/%s.scratch/", fix_dir(), name);
    fix_format(mark, sizeof(mark), " mark=%s/%s\n", fix_dir(), name);
    return find_procs(NULL, any, pids);
}

/**
 * Require that every process of row `name` has ended within GONE_MS, and
 * kill those left, so that a failure leaves nothing running.
 */
static void row_gone(const char *name)
{
    uint64_t until = now_ms() + GONE_MS;
    long pids[ROW_PROCS];
    int n, i;

    while ((n = row_procs(name, pids)) > 0 && now_ms() < until)
        poll(NULL, 0, 100);
    if (n == 0)
        return;
    printf("etude-scenario-test: fail: %s: %d of its processes are left\n", name, n);
    fails++;
    for (i = 0; i < n; i++)
        kill((pid_t)pids[i], SIGKILL);
}

/** Send `text` as a line to the peer on `port`, as a machine would. */
static int tell_peer(unsigned port, const char *text)
{
    struct sockaddr_in a = loopback((long)port);
    char buf[64];
    int fd = socket(AF_INET, SOCK_STREAM, 0), ok;

    ok = fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
         write(fd, text, strlen(text)) == (ssize_t)strlen(text) && read(fd, buf, sizeof(buf)) > 0;
    if (fd >= 0)
        close(fd);
    return ok ? 0 : -1;
}

/* The rows of a service and two machines. */
static const struct row pair_row[] = {
    {"pair", "disk", "serve first:alpha second:beta after",
     "service-ready service-running alpha-reached service-saw beta-reached both-run alpha-lost "
     "beta-survives service-stopped stopped-end once-ready once-reached once-ended once-exit "
     "gone-wait",
     "disk", 0, STEP_US, 0, 1, 1, 1, NULL, 0, NULL, 0},
};

/* The row that runs while the others fail. */
static const struct row hold_row[] = {
    {"hold", "disk", "serve first:alpha second:beta wait",
     "service-ready released alpha-alive beta-alive service-stopped", "disk", 0, STEP_US, 0, 1, 0,
     1, "wait", HOLD_US, NULL, 0},
};

/* The rows that fail beside the hold row, at a stage each: a service never
 * ready, a damaged artifact, a step's bound, a stop, and a service that
 * dies unseen. */
static const struct row fail_rows[] = {
    {"unready", "disk", "early mute",
     "early-failed early-end undeclared no-port mute-bounded mute-stopped", NULL, 0, STEP_US, 0, 1,
     1, 1, "mute", SHORT_US, "peer.1.out=peer: cannot serve", 0},
    {"broken", "disk", "serve first:alpha", "service-ready broken-refused", "disk", 0, STEP_US, 1,
     0, 1, 1, NULL, 0,
     "peer.1.out=peer: listening;alpha.1.capture=query-status: {\"return\": {\"status\": "
     "\"running\"",
     0},
    {"bound-pair", "disk", "serve first:alpha second:beta wait", "service-ready bounded", "disk", 0,
     STEP_US, 1, 0, 1, 1, "wait", SHORT_US,
     "alpha.1.capture=query-cpus-fast: {\"return\": [{\"cpu-index\": "
     "0}];beta.1.capture=EAX=00000000",
     0},
    {"stop-pair", "disk", "serve first:alpha second:beta wait", "service-ready stopped", "disk", 0,
     STEP_US, 1, 0, 1, 1, NULL, 0,
     "peer.1.out=peer: listening;beta.1.capture=query-status: {\"return\": {\"status\": "
     "\"running\"",
     SIGTERM},
    {"unseen", "disk", "serve first:alpha", "service-ready peer-gone", "disk", 0, STEP_US, 1, 1, 1,
     1, NULL, 0, "peer.1.out=peer: dying", 0},
};

/* The row killed during its wait, whose keepers must end what it ran. */
static const struct row kill_row[] = {
    {"kill-pair", "disk", "serve first:alpha second:beta wait", "service-ready stopped", "disk", 0,
     STEP_US, -1, 0, 0, 1, NULL, 0, NULL, SIGKILL},
};

/* What stops a row during its wait. */
#define WAITING "@etude 1 progress waiting 0\n"

/**
 * Run the hold row, and while its service and machines run, each failing
 * row and the killed one: each must end every process of its own, and leave
 * the hold row's processes as they were, which then still answer.
 */
static void test_isolation(const struct site *s)
{
    long before[ROW_PROCS], after[ROW_PROCS];
    struct row_run hold, killed;
    int nb, na;
    size_t i;

    row_start(&hold, hold_row, s, HOLD_ATTEMPT_MS);
    if (!hold.started ||
        !attempt_read(&hold.a, WAITING, NULL, now_ms() + (uint64_t)FAKE_ATTEMPT_MS)) {
        printf("etude-scenario-test: fail: hold: its service and machines did not start\n");
        fails++;
        if (hold.started)
            kill(hold.a.pid, SIGKILL);
        row_finish(&hold, NULL, 1);
        row_gone("hold");
        return;
    }
    nb = row_procs("hold", before);
    CHECK(nb >= 6, "hold: its scenario, keepers, service and machines run");
    for (i = 0; i <= sizeof(fail_rows) / sizeof(fail_rows[0]); i++) {
        const struct row *r =
            i < sizeof(fail_rows) / sizeof(fail_rows[0]) ? &fail_rows[i] : kill_row;

        if (r == kill_row) {
            row_start(&killed, r, s, 0);
            row_finish(&killed, WAITING, 0);
            CHECK(killed.a.stopped, "kill-pair: killed during its wait");
        } else {
            run_row(r, s, r->stop_sig != 0 ? WAITING : NULL);
        }
        row_gone(r->name);
        na = row_procs("hold", after);
        if (na != nb || memcmp(before, after, (size_t)nb * sizeof(before[0])) != 0) {
            printf("etude-scenario-test: fail: %s: the hold row's processes changed\n", r->name);
            fails++;
        }
    }
    CHECK(tell_peer(hold.port, "done\n") == 0, "hold: its peer answers");
    row_finish(&hold, NULL, 1);
    row_gone("hold");
}

/** Say whether a stand-in for QEMU of this run, or an etude-guest it ran, is
 * left: this program run with `-accel` and with paths in this run's fixture
 * directory, its kernel's in the store among them. */
static int machines_left(void)
{
    char qemu[PATH_MAX + 16], dir[PATH_MAX + 2];
    const char *any[] = {dir, NULL};
    long pids[ROW_PROCS];

    fix_format(qemu, sizeof(qemu), "%s -accel", self);
    fix_format(dir, sizeof(dir), "%s/", fix_dir());
    return find_procs(qemu, any, pids) != 0;
}

/** Copy the file at `from` to `to`, executable when `mode` says so. */
static int copy_file(const char *from, const char *to, mode_t mode)
{
    char buf[65536];
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    size_t n;
    int ok = in != NULL && out != NULL;

    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    if (in != NULL)
        fclose(in);
    if (out != NULL && fclose(out) != 0)
        ok = 0;
    return ok && chmod(to, mode) == 0 ? 0 : -1;
}

/** Damage the object of digest `sha` in store `s`: its first byte. */
static int damage(struct etude_store *st, const char *sha)
{
    char path[PATH_MAX];
    int fd;

    if (etude_store_path(st, sha, path, sizeof(path)) != 0 || chmod(path, 0644) != 0)
        return -1;
    fd = open(path, O_WRONLY);
    if (fd < 0 || write(fd, "A", 1) != 1)
        return -1;
    close(fd);
    return 0;
}

/**
 * Store the stand-in rows' artifacts: a kernel, a disk and a RAM image
 * that the stand-in reads or writes, this program as the scanner, and the
 * guest invocations of this program, staged as etude-guest's program.
 */
static int stand_in_site(struct site *s)
{
    const char *inv = fix_path("guest.invocation.json"), *kill_inv = fix_path("killer.json");
    char args[PATH_MAX + 64], sha[65];
    long size = file_sha(self, sha);

    s->arch = "i686";
    snprintf(s->qemu_i686, sizeof(s->qemu_i686), "%s", self);
    snprintf(s->qemu_x86_64, sizeof(s->qemu_x86_64), "%s", self);
    snprintf(s->store_dir, sizeof(s->store_dir), "%s", fix_path("store"));
    s->store = etude_store_open(s->store_dir);
    if (s->store == NULL || size < 0 || write_file(fix_path("kernel"), "a kernel\n", 9) != 0 ||
        write_file(fix_path("disk"), "disk: ", 6) != 0 ||
        write_file(fix_path("initrd"), "ram\n", 4) != 0 ||
        write_file(fix_path("broken"), "a broken artifact\n", 18) != 0 ||
        copy_file(self, fix_path("etude-scenario-hosttest"), 0755) != 0)
        return -1;
    snprintf(args, sizeof(args), "\"tool\", \"report\", \"test.scenario.guest\"");
    if (write_invocation(inv, "test.scenario.guest", "1111111111111111", "etude-scenario-hosttest",
                         sha, size, args) != 0)
        return -1;
    snprintf(args, sizeof(args), "\"tool\", \"killguest\", \"%s/guest.guest\"", fix_dir());
    if (write_invocation(kill_inv, "test.scenario.guest", "2222222222222222",
                         "etude-scenario-hosttest", sha, size, args) != 0)
        return -1;
    return stored(s->store, fix_path("kernel"), s->kernel) ||
           stored(s->store, fix_path("disk"), s->disk) ||
           stored(s->store, fix_path("initrd"), s->initrd) || stored(s->store, self, s->scanner) ||
           stored(s->store, inv, s->inv) || stored(s->store, kill_inv, s->killer) ||
           stored(s->store, fix_path("broken"), s->broken) || damage(s->store, s->broken);
}

/**
 * Give the path of `name` on PATH in `out`, or NULL when it is not there.
 *
 * tests/run puts its QEMU wrappers first on PATH, in the directory
 * RACH2_QEMU_SHIMS names, and each adds an `-accel` of its own. That
 * directory is skipped, so that QEMU is given the one accelerator the
 * library asks for.
 */
static const char *on_path(const char *name, char *out, size_t size)
{
    const char *path = getenv("PATH"), *shims = getenv("RACH2_QEMU_SHIMS"), *end;
    size_t len;

    for (; path != NULL && *path != '\0'; path = *end == ':' ? end + 1 : end) {
        end = strchr(path, ':');
        if (end == NULL)
            end = path + strlen(path);
        len = (size_t)(end - path);
        if (len == 0 || (shims != NULL && len == strlen(shims) && strncmp(path, shims, len) == 0))
            continue;
        snprintf(out, size, "%.*s/%s", (int)len, path, name);
        if (access(out, X_OK) == 0)
            return out;
    }
    return NULL;
}

/* The store of the real rows, under the tree's build/tests, since it has a
 * disk; removed at the test's exit. */
static char real_store[PATH_MAX];

/** Remove the real rows' store. */
static void remove_real_store(void)
{
    char cmd[PATH_MAX + 16];

    if (real_store[0] == '\0')
        return;
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", real_store);
    if (system(cmd) != 0)
        printf("etude-scenario-test: %s could not be removed\n", real_store);
}

/** End the test at a signal, through its exit's cleanup. */
static void on_signal(int sig)
{
    (void)sig;
    exit(1);
}

/**
 * Store the real rows' artifacts: the kernel, the test image and the disk
 * of the architecture ARCH names, i686 when it is unset, this program as
 * the scanner, and an invocation of the test image's /hello.
 */
static int real_site(struct site *s)
{
    const char *arch = getenv("ARCH") != NULL && *getenv("ARCH") != '\0' ? getenv("ARCH") : "i686";
    const char *dir = strcmp(arch, "x86_64") == 0 ? "build/x86_64" : "build";
    const char *inv = fix_path("hello.invocation.json");
    char kernel[PATH_MAX], initrd[PATH_MAX], disk[PATH_MAX], hello[PATH_MAX], sha[65];
    long size;

    s->arch = arch;
    if (on_path("qemu-system-i386", s->qemu_i686, sizeof(s->qemu_i686)) == NULL ||
        on_path("qemu-system-x86_64", s->qemu_x86_64, sizeof(s->qemu_x86_64)) == NULL) {
        printf("etude-scenario-test: fail: cannot run the boots: no qemu-system-i386 and "
               "qemu-system-x86_64 on PATH\n");
        return -1;
    }
    snprintf(kernel, sizeof(kernel), "%s/rach2/kernel", dir);
    snprintf(initrd, sizeof(initrd), "%s/images/test.krffs", dir);
    snprintf(disk, sizeof(disk), "%s/images/host-etude-scenario.disk", dir);
    snprintf(hello, sizeof(hello), "%s/tests/hello", dir);
    size = file_sha(hello, sha);
    if (access(kernel, R_OK) != 0 || access(initrd, R_OK) != 0 || access(disk, R_OK) != 0 ||
        size < 0) {
        printf("etude-scenario-test: fail: cannot run the boots: %s, %s, %s or %s is not built\n",
               kernel, initrd, disk, hello);
        return -1;
    }
    snprintf(real_store, sizeof(real_store), "build/tests/etude-scenario.XXXXXX");
    if (mkdir("build/tests", 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdtemp(real_store) == NULL) {
        real_store[0] = '\0';
        return -1;
    }
    atexit(remove_real_store);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    if (realpath(real_store, s->store_dir) == NULL)
        return -1;
    s->store = etude_store_open(s->store_dir);
    if (s->store == NULL || write_invocation(inv, "test.scenario.real-guest", "3333333333333333",
                                             "hello", sha, size, "") != 0)
        return -1;
    snprintf(s->broken, sizeof(s->broken), "%s", sha);
    return stored(s->store, kernel, s->kernel) || stored(s->store, disk, s->disk) ||
           stored(s->store, initrd, s->initrd) || stored(s->store, self, s->scanner) ||
           stored(s->store, inv, s->inv) || stored(s->store, inv, s->killer);
}

/* The stand-in rows: name, mode, steps, assertions, disk, helper, step
 * bound, exit status, end record, kept scratch, and the fields of a row of
 * two machines and a service. */
static const struct row stand_in_rows[] = {
    {"reboot", "disk", "write:fixture check readback:fixture",
     "qmp-status qmp-reply qmp-error stopped shutdown host-ran host-sees same-copy readback "
     "second-stop",
     "disk", 0, STEP_US, 0, 1, 0, 0, NULL, 0, NULL, 0},
    {"lost", "lost", "lose:fixture", "lost-wait ended not-shutdown", "disk", 0, LOSS_US, 0, 1, 0, 0,
     NULL, 0, NULL, 0},
    {"die", "die", "lose:fixture", "lost-wait ended not-shutdown", "disk", 0, LOSS_US, 0, 1, 0, 0,
     NULL, 0, NULL, 0},
    {"closecon", "disk", "lose:fixture", "lost-wait not-shutdown", "disk", 0, LOSS_US, 0, 1, 0, 0,
     NULL, 0, NULL, 0},
    {"closeqmp", "disk", "lose:fixture", "lost-wait not-shutdown", "disk", 0, LOSS_US, 0, 1, 0, 0,
     NULL, 0, NULL, 0},
    {"bound", "disk", "wait:fixture", "bounded after-bound", "disk", 0, SHORT_US, 0, 1, 0, 0, NULL,
     0, NULL, 0},
    {"misuse", "disk", "host boot:fixture",
     "no-step no-artifact no-copy host-step other-machine no-tool step-back machine-step "
     "other-disk running no-qmp",
     "disk", 0, STEP_US, 0, 1, 1, 0, NULL, 0, NULL, 0},
    {"wrong", "wrong", "boot:fixture", "refused damaged", "disk", 0, STEP_US, 0, 1, 0, 0, NULL, 0,
     NULL, 0},
    {"guest", "disk", "run:fixture", "guest-pass guest-exit guest-ran guest-lost guest-lost-end",
     NULL, 1, LOSS_US, 0, 1, 1, 0, NULL, 0, NULL, 0},
};

/* The stop row, which keeps its copy and gives no end record. */
static const struct row stop_row[] = {
    {"stop", "disk", "wait:fixture after:fixture", "stopped still-stopped", "disk", 0, STEP_US, 1,
     0, 1, 0, NULL, 0, NULL, 0},
};

/* The damaged row, run on a store whose kernel is damaged. */
static const struct row damaged_row[] = {
    {"damaged", "disk", "boot:fixture", "refused damaged", "disk", 0, STEP_US, 0, 1, 1, 0, NULL, 0,
     NULL, 0},
};

/* The real rows. */
static const struct row real_rows[] = {
    {"real-reboot", NULL, "write:fixture check readback:fixture",
     "stopped shutdown host-ran host-sees same-copy readback second-stop", "disk", 0, REAL_US, 0, 1,
     0, 0, NULL, 0, NULL, 0},
    {"real-guest", NULL, "run:fixture", "guest-pass guest-exit", NULL, 1, REAL_US, 0, 1, 0, 0, NULL,
     0, NULL, 0},
    {"real-peer", NULL, "serve alpha:alpha beta:beta",
     "service-ready alpha-tcp beta-tcp both-closed service-stopped", NULL, 0, REAL_US, 0, 1, 0, 1,
     NULL, 0, NULL, 0},
};

/** Run the stand-in rows, the stop row and the damaged row. */
static void test_stand_in(void)
{
    struct site s;
    size_t i;

    memset(&s, 0, sizeof(s));
    if (stand_in_site(&s) != 0) {
        printf("etude-scenario-test: fail: the stand-in rows' artifacts cannot be made\n");
        fails++;
        return;
    }
    for (i = 0; i < sizeof(stand_in_rows) / sizeof(stand_in_rows[0]); i++)
        run_row(&stand_in_rows[i], &s, NULL);
    run_row(stop_row, &s, WAITING);
    run_row(pair_row, &s, NULL);
    test_isolation(&s);
    CHECK(etude_store_check(s.store, s.disk, NULL) == 0, "the disk artifact is unchanged");
    CHECK(damage(s.store, s.kernel) == 0, "damaged");
    run_row(damaged_row, &s, NULL);
    CHECK(!machines_left(), "every stand-in QEMU and etude-guest has ended");
    etude_store_close(s.store);
}

/** Run the real rows. */
static void test_real(void)
{
    struct site s;
    size_t i;

    memset(&s, 0, sizeof(s));
    if (real_site(&s) != 0) {
        fails++;
        return;
    }
    for (i = 0; i < sizeof(real_rows) / sizeof(real_rows[0]); i++)
        run_row(&real_rows[i], &s, NULL);
    CHECK(etude_store_check(s.store, s.disk, NULL) == 0, "the real disk artifact is unchanged");
    etude_store_close(s.store);
}

/**
 * Run the test, or this program as the host tool or the stand-in for QEMU,
 * and exit; with one argument, the invocation, return so that the library's
 * own `main` runs the scenario. A constructor, since `main` is
 * libetude-case's.
 */
static void __attribute__((constructor)) dispatch(int argc, char **argv)
{
    const char *fake = getenv("ETUDE_FAKE_QEMU");

    if (argc == 2)
        return;
    if (argc >= 3 && strcmp(argv[1], "tool") == 0)
        exit(tool(argc, argv));
    if (fake != NULL && *fake != '\0')
        exit(fake_qemu(fake, argc, argv));
    if (realpath(argv[0], self) == NULL) {
        printf("etude-scenario-test: fail: %s cannot be found\n", argv[0]);
        exit(1);
    }
    if (setenv("TMPDIR", fix_dir(), 1) != 0)
        exit(1);
    test_stand_in();
    if (getenv("ETUDE_TEST_NO_REAL") == NULL || *getenv("ETUDE_TEST_NO_REAL") == '\0')
        test_real();
    if (fails != 0)
        exit(1);
    fix_passed();
    printf("etude_scenario_hosttest: pass\n");
    exit(0);
}
