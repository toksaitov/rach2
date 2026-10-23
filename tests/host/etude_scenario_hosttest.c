/**
 * Check libetude-case (tools/etude/posix/scenario.c), the library of etude's
 * machine scenarios, with this program as the scenarios, as a stand-in for
 * QEMU when ETUDE_FAKE_QEMU is set, and as the host tool and the guest
 * program they run; and with the real QEMU on the kernel, the test image and
 * a disk of the architecture ARCH names.
 *
 * host-deps: tests/host/compat/process_host.c
 * host-deps: tests/host/compat/desktop_host.c tests/host/compat/scenario_host.c
 * host-deps: tools/etude/posix/scenario.c
 * host-deps: tools/etude/serial.c tools/etude/processor.c
 * host-deps: tools/etude/posix/qemu.c tools/etude/qemu_profile.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/posix/store.c
 * host-deps: tools/etude/capture.c tools/etude/history_json.c
 * host-deps: tools/etude/guest/guest.c tools/etude/guest/fixture.c
 * host-deps: tools/etude/guest/wire.c
 * host-deps: tools/etude/volume/volume.c
 * host-deps: utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/guest
 * host-cflags: -I utils -Wall -Wextra -Werror
 * host-first: it runs for minutes
 * host-arch: it boots the kernel, the test image and a disk of ARCH
 *
 * Each row is one attempt of a machine case, run as the executor runs it: this
 * program with the invocation, its report read from its standard output. A
 * machine boots on a private copy of its disk, writes to it and powers off; a
 * host step reads the write from the copy, and the next boot of the same copy
 * reads it back, and the artifact itself is unchanged. A search of the
 * console of the machine that powered off finds what it printed after the
 * offset given and nothing before it, and finds no text it did not print;
 * etude_case_id gives the case's identity. The same holds for a
 * machine without the 8042 whose disk is a drive of a USB stick on a USB
 * controller, whose stand-in QEMU requires those arguments. A machine that says
 * it powers off and ends without QEMU's event, one killed by a signal, one
 * whose console closes and one whose QMP closes are each told apart from a
 * shutdown, and a wait on each ends at once with the loss. The end a wait
 * observes is the boot's machine record in the report. On a machine of two
 * processors, etude_fault finds a panic after an NMI on the console and a halt
 * without a record in the registers, each only once it has happened, and
 * records it as the boot's end, which no later end replaces. A wait past its
 * step's bound ends at the bound; a stop signal ends the scenario's wait, its
 * machine and its report, and keeps its copies; a scenario that asks for what
 * its manifest does not declare is refused; a failed accelerator, a damaged
 * artifact and an unsafe memory profile are reported. A case of a boundary
 * machine and an ordinary one whose reservation covers the boundary machine
 * alone has a boot of either refused before QEMU starts; with both covered, the
 * boundary machine boots on the reservation left beside the other, answers
 * QMP's queries and the human monitor's `info`, has every other QMP command
 * refused, and has `help` and a second memory probe refused at its console.
 * Under the stand-in the machine's etude-guest runs a guest program whose
 * report becomes the scenario's, and an etude-guest killed in the middle of a
 * run is a lost end. A media row boots a machine twice with the same empty
 * sparse disk of 4 TiB on NVMe, a CD whose medium is the artifact's object and
 * an empty disk on AHCI without flushes, and the stand-in lists its drives; the
 * disks are removed after the pass. A boot whose profile records its second
 * card's frames gives the scenario the file the stand-in wrote for that card,
 * removed after a pass and kept after a failure. A machine's first card has a
 * restricted network that forwards two of the case's ports to the guest, one by
 * TCP and one by UDP, and the guest's connections to 10.0.2.100:7777 to a third
 * through this program as the relay, and its second card has a user network
 * without forwards; a machine that forwards a port the case does not hold is
 * refused before QEMU starts. This program is also the peer that a scenario
 * runs as a service; two machines at once reach it, a service ended unseen is a
 * fault, failing rows at each stage end their own processes while a held row
 * keeps its own, and a SIGTERM to every process of a case, its keepers among
 * them, ends them all and leaves the scenario seeing its service ended by the
 * stop. A row that passes leaves nothing in its scratch but its machines'
 * consoles. With the real QEMU, a disk root written at the debug shell and
 * powered off is read back by the next boot, from a private copy on
 * AHCI and from a qcow2 overlay over the disk on an IDE drive with a serial,
 * whose map lists the write and whose raw conversion has it; etude-guest runs
 * the test image's /hello, two machines run /tcptest against the peer, the
 * frames of one machine's card recorded in its capture, QEMU accepts a
 * connection at the forwarded TCP port and has bound the forwarded UDP port,
 * /tcptest reaches the peer through the guest forward and is refused it at the
 * gateway, the kernel finds the size of the media row's empty disk, and the
 * kernel is stopped and resumed thirty times by a declared suspension and then
 * powers off. Under the stand-in, which writes the monotonic clock on its
 * console every TICK_MS, the same suspension holds each stop, with no time
 * written inside one and one after each; a stop signal, a step's bound and a
 * kill of the scenario during a stop each leave no stopped QEMU.
 */
#ifdef _WIN32
#include "win/files.h"
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "compat/pathmax_host.h"
#include "compat/scenario_host.h"
#include "compat/scenario_guest.h"
#include "compat/fixdir_host.c"
#include "compat/process_host.h"
#include "etude/artifact.h"
#include "etude/report.h"
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

/* The step bounds of the rows: the bound row's, the lost rows', which a loss
 * must not wait out, the others', and that of the real forward row's step in
 * which the guest is refused a connection, which a connection let through
 * must not wait out. */
#define SHORT_US  1000000u
#define LOSS_US   10000000u
#define STEP_US   60000000u
#define REAL_US   120000000u
#define CLOSED_US 30000000u

/* The hold row's bound on the step in which the other rows fail beside it,
 * and the most processes of one row the test counts. */
#define HOLD_US   240000000u
#define ROW_PROCS 64

/* The suspension rows: the cycles and intervals of the rows that run them
 * all; the stop of the rows that end during their one stop, which the end
 * must cut short; their steps' bound, within the attempt's, and the bound of
 * suspend-bound's step that ends its stop; the interval of the stand-in's
 * ticks, and the most ticks a scenario reads. */
#define STALLS         30
#define STALL_RUN_US   100000u
#define STALL_US       50000u
#define LONG_STOP_US   120000000u
#define STALL_STEP_US  20000000u
#define STALL_BOUND_US 3000000u
#define TICK_MS        2
#define TICKS_MAX      65536

/* The byte of the detail row's long detail that is the last its record keeps:
 * the line's limit less its newline, the prefix and `assert cut fail `. */
#define CUT_HEAD   "assert cut fail "
#define DETAIL_CUT (ETUDE_REPORT_LINE_MAX - sizeof(ETUDE_REPORT_PREFIX) - sizeof(CUT_HEAD))

/* The media rows' empty disk, 4 TiB as the uefi suite's, on NVMe, their CD,
 * whose medium is the artifact `disk`, and an empty disk on AHCI whose
 * flushes QEMU keeps from the host. */
#define BIG_BYTES (UINT64_C(4) << 40)
#define MEDIA_DISKS                                                                         \
    "[{\"name\": \"big\", \"bus\": \"nvme\", \"serial\": \"rach2big\", \"discard\": true, " \
    "\"size_bytes\": 4398046511104}, {\"name\": \"disk\", \"media\": \"cdrom\"}, "          \
    "{\"name\": \"blank\", \"bus\": \"ahci\", \"size_bytes\": 1048576, \"flush\": false}]"

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

/** Say whether the file at `path` is a little-endian pcap capture whose
 * frames have the bytes of `text`. */
static int pcap_has(const char *path, const char *text)
{
    static const unsigned char magic[4] = {0xd4, 0xc3, 0xb2, 0xa1};
    unsigned char head[4];
    FILE *f = fopen(path, "rb");
    int ok;

    if (f == NULL)
        return 0;
    ok = fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, magic, 4) == 0;
    fclose(f);
    return ok && file_has(path, text);
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
static int peer_endpoint(host_socket fd, const char *mark, int binding)
{
#ifdef _WIN32
    char path[PATH_MAX];
    unsigned port;
    struct sockaddr_in address;
    FILE *file;
    if (mark == NULL || binding)
        return -1;
    fix_format(path, sizeof(path), "%s.peer", mark);
    file = fopen(path, "r");
    if (file == NULL)
        return -1;
    port = 0;
    fscanf(file, "%u", &port);
    fclose(file);
    address = loopback(port);
    return port != 0 ? connect((SOCKET)fd, (struct sockaddr *)&address, sizeof(address)) : -1;
#else
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
#endif
}

/**
 * Receive the row's bound TCP socket from its controller and check its
 * port. Give the descriptor, owned by the caller, or -1 for an error.
 */
static host_socket peer_socket(const char *port)
{
    struct sockaddr_in a;
    sh_socklen len = sizeof(a);
    host_socket fd = HOST_SOCKET_INVALID, connection;
#ifndef _WIN32
    int received_fd = -1;
#endif
#ifdef _WIN32
    connection = sh_socket(SOCK_STREAM);
    if (connection >= 0 && peer_endpoint(connection, getenv("ETUDE_TEST_MARK"), 0) == 0)
        fd = sh_receive_socket(connection);
    if (connection >= 0)
        host_socket_close(connection);
#else
    struct msghdr msg;
    struct cmsghdr *c;
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    char byte;
    struct iovec iov = {&byte, 1};

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
            c->cmsg_len == CMSG_LEN(sizeof(received_fd))) {
            memcpy(&received_fd, CMSG_DATA(c), sizeof(received_fd));
            fd = received_fd;
        }
    }
    host_socket_close(connection);
#endif
    if (fd >= 0 && getsockname(fd, (struct sockaddr *)&a, &len) != 0) {
        host_socket_close(fd);
        return -1;
    }
    if (fd >= 0 && a.sin_port != htons((unsigned short)strtol(port, NULL, 10))) {
        host_socket_close(fd);
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
    struct sh_poll p[PEER_CONNS + 1];
    char line[PEER_CONNS][256], text[300];
    size_t len[PEER_CONNS];
    host_socket l;
    int i, n, k;
    ssize_t got;
    char buf[512];

    if (strcmp(mode, "early") == 0) {
        say("peer: cannot serve: told to fail");
        return 3;
    }
    if (strcmp(mode, "mute") == 0)
        host_wait_forever();
    l = peer_socket(port);
    if (l < 0 || listen(l, PEER_CONNS) != 0) {
        if (l >= 0)
            host_socket_close(l);
        snprintf(text, sizeof(text), "peer: cannot serve: %s", strerror(errno));
        say(text);
        return 3;
    }
    snprintf(text, sizeof(text), "peer: listening on %s", port);
    say(text);
    for (i = 0; i <= PEER_CONNS; i++) {
        p[i].fd = i == 0 ? l : -1;
        p[i].events = SH_POLLIN;
    }
    for (;;) {
        if (sh_poll(p, PEER_CONNS + 1, -1) < 0)
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
            got = host_socket_read(p[i].fd, buf, sizeof(buf));
            if (got <= 0) {
                host_socket_close(p[i].fd);
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
            if (host_socket_write(p[i].fd, buf, (size_t)got) != got)
                say("peer: a reply was cut short");
        }
    }
}

/** Write the `len` bytes at `buf` to `fd`; 0, or -1 for an error. */
#ifndef _WIN32
static int write_bytes(int fd, const char *buf, size_t len)
{
    ssize_t n;

    while (len > 0) {
        n = write(fd, buf, len);
        if (n <= 0)
            return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

#endif

/**
 * Be the relay of a guest forward, as QEMU runs it for each connection:
 * join standard input and output, the guest's connection, to a connection to
 * `port` of 127.0.0.1, half-close each side when the other's bytes end, and
 * exit 0 once both have ended. Standard error is the guest's connection too,
 * so the relay says nothing.
 */
static int relay(const char *port)
{
#ifdef _WIN32
    return scenario_relay(port);
#else
    struct sockaddr_in a = loopback(strtol(port, NULL, 10));
    struct sh_poll p[2];
    char buf[4096];
    ssize_t n;
    int s, from_guest = 1, to_guest = 1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) != 0)
        return 1;
    while (from_guest || to_guest) {
        p[0].fd = from_guest ? 0 : -1;
        p[1].fd = to_guest ? s : -1;
        p[0].events = p[1].events = SH_POLLIN;
        if (sh_poll(p, 2, -1) < 0)
            continue;
        if (p[0].revents != 0) {
            n = read(0, buf, sizeof(buf));
            if (n <= 0) {
                shutdown(s, SHUT_WR);
                from_guest = 0;
            } else if (write_bytes(s, buf, (size_t)n) != 0) {
                return 1;
            }
        }
        if (p[1].revents != 0) {
            n = read(s, buf, sizeof(buf));
            if (n <= 0) {
                shutdown(1, SHUT_WR);
                to_guest = 0;
            } else if (write_bytes(1, buf, (size_t)n) != 0) {
                return 1;
            }
        }
    }
    return 0;
#endif
}

/**
 * Be the tool `argv[2]`: `scan FILE TEXT` exits 0 when FILE has TEXT;
 * `report CASE` reports one assertion of CASE as a guest program does;
 * `killguest FILE` kills the etude-guest whose PID FILE has; `peer PORT
 * MODE MARK` is a service, whose MARK has its row and this run's fixture
 * directory, so that the test finds it among the processes; and `relay
 * PORT` is the relay of a guest forward.
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
    if (argc == 4 && strcmp(argv[2], "relay") == 0)
        return relay(argv[3]);
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
            sh_signal(pid, SIGKILL);
        return 0;
    }
    return 2;
}

/* ---- The stand-in for QEMU. ---- */

/** Write all of `text` to `fd`, or end the stand-in. */
static void put(host_socket fd, const char *text)
{
    size_t len = strlen(text), done = 0;
    int n;
    while (done < len) {
        n = host_socket_write(fd, text + done, len - done);
        if (n <= 0)
            _exit(120);
        done += (size_t)n;
    }
}

/** Connect to the endpoint of QEMU's client chardev. */
static host_socket fake_connect(const char *path)
{
    host_socket socket = host_qemu_connect(path);
    if (socket < 0)
        _exit(121);
    return socket;
}

/* The stand-in prints ticks on its console once it is told to, and the file
 * that its SIGTERM handler writes, in the mode `tick`. */
static int fake_ticking;
static char fake_ended[PATH_MAX];

/** Write `fake: terminated` to the file fake_ended names, and exit. */
static void fake_term(int sig)
{
    int fd = open(fake_ended, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd >= 0) {
        if (write(fd, "fake: terminated\n", 17) != 17)
            _exit(119);
        close(fd);
    }
    _exit(128 + sig);
}

/** Have SIGTERM write ETUDE_TEST_MARK.ended in the mode `mode` of `tick`. */
static void fake_arm(const char *mode)
{
    if (strcmp(mode, "tick") != 0 || getenv("ETUDE_TEST_MARK") == NULL)
        return;
    snprintf(fake_ended, sizeof(fake_ended), "%s.ended", getenv("ETUDE_TEST_MARK"));
    signal(SIGTERM, fake_term);
}

/** Give the milliseconds the stand-in waits for its sockets: TICK_MS once it
 * ticks, or -1 for no limit. */
static int fake_wait_ms(void)
{
    return fake_ticking ? TICK_MS : -1;
}

/** Write a tick on the console `con`, once the stand-in ticks and while the
 * console is open: `tick N`, N the monotonic clock in microseconds, as the
 * library's executor reads it. */
static void fake_tick(host_socket con)
{
    char text[64];

    if (!fake_ticking || con < 0)
        return;
    snprintf(text, sizeof(text), "tick %llu\r\n", (unsigned long long)host_clock_us());
    put(con, text);
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
    host_socket fd;
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
        n = host_socket_read(in->fd, &c, 1);
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

/** Give the command a QMP line executes, in `name`, and its id, the line's
 * last `id`, since its arguments may have one of their own. */
static long fake_command(const char *line, char *name, size_t size)
{
    const char *at = strstr(line, "\"execute\":\""), *id = NULL, *p;
    size_t n = 0;

    for (p = strstr(line, "\"id\":"); p != NULL; p = strstr(p + 1, "\"id\":"))
        id = p;
    name[0] = '\0';
    if (at == NULL)
        return -1;
    for (at += 11; *at != '\0' && *at != '"' && n + 1 < size; at++)
        name[n++] = *at;
    name[n] = '\0';
    return id != NULL ? strtol(id + 5, NULL, 10) : -1;
}

/** Send QMP's event SHUTDOWN of the guest or not, for `reason`. */
static void fake_shutdown(host_socket qmp, int guest, const char *reason)
{
    char text[256];

    snprintf(text, sizeof(text),
             "{\"timestamp\": {\"seconds\": 1, \"microseconds\": 2}, \"event\": \"SHUTDOWN\", "
             "\"data\": {\"guest\": %s, \"reason\": \"%s\"}}\r\n",
             guest ? "true" : "false", reason);
    put(qmp, text);
}

/* The stand-in's processors, and how many of them, from the first, its
 * console's `nmi`, `stop` or `stop one` has halted with interrupts off. */
static unsigned fake_cpus = 1;
static unsigned fake_halted;

/** Put in `text` the human monitor's `info registers -a` of the stand-in's
 * processors, escaped for a JSON string, as QEMU gives it for i686: a section
 * from `CPU#` for each, with HLT=1 and the flags' IF set unless it halted. */
static void fake_registers(char *text, size_t size)
{
    size_t n = 0;
    unsigned i;

    text[0] = '\0';
    for (i = 0; i < fake_cpus && n < size; i++)
        n += (size_t)snprintf(text + n, size - n,
                              "CPU#%u\\r\\nEAX=00000000\\r\\nEIP=00101000 EFL=%s [-------] "
                              "CPL=0 II=0 A20=1 SMM=0 HLT=1\\r\\n",
                              i, i < fake_halted ? "00000002" : "00000202");
}

/** Answer one QMP line of the launcher's or the scenario's; exit at quit. */
static void fake_qmp(host_socket qmp, const char *line, const char *accel, const char *mode)
{
    char name[64], text[1024], regs[768];
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
    else if (strcmp(name, "human-monitor-command") == 0) {
        fake_registers(regs, sizeof(regs));
        snprintf(text, sizeof(text), "{\"return\": \"%s\", \"id\": %ld}\r\n", regs, id);
    } else if (strcmp(name, "qmp_capabilities") == 0 || strcmp(name, "cont") == 0 ||
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
static void fake_peer(host_socket con, const char *args)
{
    char *text, want[300], got[300], said[400];
    struct sockaddr_in a = loopback(strtol(args, &text, 10));
    size_t n = 0;
    host_socket fd = sh_socket(SOCK_STREAM);

    snprintf(want, sizeof(want), "%s\n", *text == ' ' ? text + 1 : text);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        host_socket_write(fd, want, strlen(want)) == (ssize_t)strlen(want))
        while (n + 1 < sizeof(got) && host_socket_read(fd, got + n, 1) == 1 && got[n++] != '\n') {
        }
    got[n] = '\0';
    if (fd >= 0)
        host_socket_close(fd);
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
static long fake_guest(host_socket con, const char *line)
{
#ifdef _WIN32
    long child = scenario_guest_start(self, con);
    FILE *file = fopen(getenv("ETUDE_FAKE_GUEST_PID"), "w");
    (void)line;
    if (file != NULL) {
        fprintf(file, "%ld\n", child);
        fclose(file);
    }
    return child;
#else
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
#endif
}

/**
 * Act on the line that the console `con` typed: `write TEXT` and `read` the
 * first disk, at `disk` or none at NULL, `poweroff`, `nmi`, which prints the
 * kernel's records of an NMI and its panic and halts the processors, `stop`,
 * which halts them without a record, `stop one`, which halts the first,
 * `die`, `closecon`, `closeqmp`, `peer PORT TEXT`, `tick`, which starts the
 * ticks, and `exec /etude-guest`. A console or QMP that it closes has its
 * descriptor set to -1.
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
    } else if (strcmp(con->line, "nmi") == 0) {
        put(con->fd, "<2>[    1.000000] isr: hardware fault: non-maskable interrupt (vector 2, "
                     "pc 0x00101000)\r\n<2>[    1.000000] panic: stopped: the machine reported "
                     "a fault\r\n");
        fake_halted = fake_cpus;
    } else if (strcmp(con->line, "stop") == 0 || strcmp(con->line, "stop one") == 0) {
        put(con->fd, "fake: halted\r\n");
        fake_halted = con->line[4] == '\0' ? fake_cpus : 1;
    } else if (strcmp(con->line, "die") == 0) {
        put(con->fd, "fake: dying\r\n");
        sh_signal((long)getpid(), SIGKILL);
    } else if (strcmp(con->line, "closecon") == 0) {
        host_socket_close(con->fd);
        con->fd = -1;
    } else if (strcmp(con->line, "closeqmp") == 0) {
        put(con->fd, "fake: no qmp\r\n> ");
        host_socket_close(qmp->fd);
        qmp->fd = -1;
    } else if (strncmp(con->line, "peer ", 5) == 0) {
        fake_peer(con->fd, con->line + 5);
    } else if (strcmp(con->line, "tick") == 0) {
        fake_ticking = 1;
        put(con->fd, "fake: ticking\r\n");
    } else if (strncmp(con->line, "exec /etude-guest", 17) == 0) {
        fake_guest(con->fd, con->line);
        host_socket_close(con->fd);
        con->fd = -1;
    }
}

/** Say whether the manifest's card options reached the stand-in. */
static int fake_network(int argc, char **argv)
{
    const char *net = fake_arg(argc, argv, "-netdev", "user,id=net0,");
    const char *card = fake_arg(argc, argv, "-device", "e1000,netdev=net0,");

    return net != NULL &&
           strcmp(net, "net=10.90.0.0/16,host=10.90.0.2,"
                       "dhcpstart=10.90.0.20,dns=10.90.0.3") == 0 &&
           card != NULL && strcmp(card, "mac=52:54:00:12:34:58") == 0;
}

/** Say whether QEMU's arguments `argv` make the usb row's machine: no 8042,
 * and its disk `disk` the drive of a stick on a USB controller. */
static int fake_usb(int argc, char **argv, const char *disk)
{
    return fake_arg(argc, argv, "-machine", "i8042=off") != NULL &&
           fake_arg(argc, argv, "-device", "qemu-xhci,id=xhci,msi=on") != NULL &&
           fake_arg(argc, argv, "-device", "usb-storage,bus=xhci.0,port=2,drive=stick") != NULL &&
           disk != NULL && strstr(disk, ",format=raw,if=none,id=stick") != NULL;
}

/** Give the status the stand-in exits with when the mode `mode` needs
 * arguments that `argv` lacks: 126 for the network row's, 120 for the usb
 * row's; 0 when it has them. */
static int fake_refused(const char *mode, int argc, char **argv, const char *disk)
{
    if (strcmp(mode, "network") == 0 && !fake_network(argc, argv))
        return 126;
    if (strcmp(mode, "usb") == 0 && !fake_usb(argc, argv, disk))
        return 120;
    return 0;
}

/** Print the value of each `-netdev` of `argv` on the console `fd`, as
 * `fake: netdev VALUE`. */
static void fake_netdevs(host_socket fd, int argc, char **argv)
{
    int i;

    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-netdev") == 0) {
            put(fd, "fake: netdev ");
            put(fd, argv[i + 1]);
            put(fd, "\r\n");
        }
    }
}

/**
 * Say each `-drive` and `-device` argument of the stand-in's command line
 * on console `fd`, as `fake: drive ARG` or `fake: device ARG`.
 */
static void fake_drives(host_socket fd, int argc, char **argv)
{
    int i;

    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-drive") == 0 || strcmp(argv[i], "-device") == 0) {
            put(fd, "fake: ");
            put(fd, argv[i] + 1);
            put(fd, " ");
            put(fd, argv[i + 1]);
            put(fd, "\r\n");
        }
    }
}

/** Write the `n` bytes of `head` and then `text` to the file at `path`, as
 * QEMU writes a recording; a NULL path asks for none. Return 0, or -1 when
 * the file cannot be made. */
static int fake_record(const char *path, const unsigned char *head, size_t n, const char *text)
{
    FILE *f;

    if (path == NULL)
        return 0;
    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    if (n != 0)
        fwrite(head, 1, n, f);
    fputs(text, f);
    fclose(f);
    return 0;
}

/**
 * Be QEMU, as ETUDE_FAKE_QEMU's `mode` says: answer QMP's start, say that
 * the machine runs, in the mode `media` say each of its `-drive` and `-device`
 * arguments on the console, as fake_drives does, print the value of each
 * `-netdev`, say that the shell is ready, and act on what the console types,
 * as fake_console does. Once it ticks, it writes a tick every TICK_MS. In the
 * mode `tick`, SIGTERM writes ETUDE_TEST_MARK.ended before the stand-in ends.
 */
static int fake_qemu(const char *mode, int argc, char **argv)
{
    const char *accel = fake_arg(argc, argv, "-accel", "");
    const char *disk = fake_arg(argc, argv, "-drive", "file=");
    const char *audio = fake_arg(argc, argv, "-audiodev", "wav,id=etude-audio,path=");
    const char *dump =
        fake_arg(argc, argv, "-object", "filter-dump,id=net1-dump,netdev=net1,file=");
    static const unsigned char pcap[4] = {0xd4, 0xc3, 0xb2, 0xa1};
    char path[PATH_MAX];
    struct fake_in con, qmp;
    struct sh_poll p[2];
    int ok = 1, r;

    if ((r = fake_refused(mode, argc, argv, disk)) != 0)
        return r;
    if (accel == NULL)
        return 125;
    if (fake_arg(argc, argv, "-smp", "") != NULL)
        fake_cpus = (unsigned)atoi(fake_arg(argc, argv, "-smp", ""));
    fake_arm(mode);
    if (disk != NULL) {
        snprintf(path, sizeof(path), "%s", disk);
        if (strchr(path, ',') != NULL)
            *strchr(path, ',') = '\0';
        disk = path;
    }
    con.fd = fake_connect(host_qemu_endpoint(argc, argv, "etude-console"));
    qmp.fd = fake_connect(host_qemu_endpoint(argc, argv, "etude-qmp"));
    con.len = qmp.len = 0;
    put(qmp.fd, "{\"QMP\": {\"version\": {}, \"capabilities\": []}}\r\n");
    while (ok) {
        r = fake_read(&qmp);
        if (r < 0)
            _exit(122);
        fake_qmp(qmp.fd, qmp.line, accel, mode);
        ok = strstr(qmp.line, "\"cont\"") == NULL;
    }
    if (fake_record(audio, NULL, 0, "recorded samples") != 0 ||
        fake_record(dump, pcap, sizeof(pcap), "captured frames") != 0)
        return 127;
    if (strcmp(mode, "media") == 0)
        fake_drives(con.fd, argc, argv);
    fake_netdevs(con.fd, argc, argv);
    put(con.fd, "fake: ready\r\nType 'help' for available commands.\r\n> ");
    for (;;) {
        p[0].fd = con.fd;
        p[1].fd = qmp.fd;
        p[0].events = p[1].events = SH_POLLIN;
        r = sh_poll(p, 2, fake_wait_ms());
        fake_tick(con.fd);
        if (r < 0)
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
            host_socket_close(con.fd);
            con.fd = -1;
            continue;
        }
        if (r == 0)
            continue;
        fake_console(&con, &qmp, disk, mode);
        if (con.fd < 0 && qmp.fd < 0)
            host_wait_forever();
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

/** Run the reboot row: a write, a host read of the copy, and a readback. The
 * real row's disk is on AHCI, the kernel's disk0. */
static int scenario_reboot(struct etude_case *c, int real)
{
    const char *nonce = getenv("ETUDE_TEST_NONCE");
    const char *copy, *again, *shell = real ? "\n> " : "> ";
    char *scan[] = {"scanner", "tool", "scan", NULL, (char *)nonce, NULL};
    struct etude_case_machine *m;
    struct etude_termination end;
    char line[128], *reply = NULL;
    uint64_t at, said = 0, past = 0;

    if (etude_step(c, "write") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    if (!real)
        etude_assert(c, "case-id",
                     strncmp(etude_case_id(c), "test.scenario.", 14) == 0 &&
                         strcmp(etude_case_id(c) + 14, getenv("ETUDE_TEST_ROW")) == 0,
                     etude_case_id(c));
    copy = etude_private_copy(c, "disk");
    if (real)
        etude_assert(c, "on-ahci",
                     etude_console_wait(m, "disk: disk0: ahci 0: ", 0, &at) == ETUDE_OK,
                     "the disk is not disk0, on port 0 of the AHCI controller");
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
    if (!real) {
        expect(c, "find-said", etude_console_find(m, "powering off", at, &said), ETUDE_OK);
        expect(c, "find-past", etude_console_find(m, "powering off", said, &past), ETUDE_FAILED);
        etude_assert(c, "find-at", said > at && past == 0, "not past the text");
        expect(c, "find-absent", etude_console_find(m, "never printed", 0, &past), ETUDE_FAILED);
    }
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

/**
 * Run the overlay row: the real kernel writes its disk root through a qcow2
 * overlay over the disk; `qemu-img map` lists a cluster the overlay holds,
 * `qemu-img convert` gives a raw disk with the write, and the next boot of the
 * same overlay reads it back.
 */
static int scenario_overlay(struct etude_case *c)
{
    const char *nonce = getenv("ETUDE_TEST_NONCE");
    const char *over, *again, *raw = etude_evidence(c, "over.raw");
    char *map[] = {"qemu-img", "map", "--output=json", NULL, NULL};
    char *convert[] = {"qemu-img", "convert", "-f", "qcow2", "-O", "raw", NULL, NULL, NULL};
    char *scan[] = {"scanner", "tool", "scan", NULL, (char *)nonce, NULL};
    struct etude_case_machine *m;
    struct etude_termination end;
    char line[128], raw_path[4096];
    uint64_t at;

    if (raw == NULL || etude_step(c, "write") != ETUDE_OK ||
        etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    snprintf(raw_path, sizeof(raw_path), "%s", raw);
    over = etude_private_copy(c, "over");
    at = wait_for(c, m, "stopped", "\n> ", wait_for(c, m, "stopped", "Type 'help'", 0));
    snprintf(line, sizeof(line), "write /marker %s\r", nonce);
    etude_type(m, line);
    wait_for(c, m, "stopped", "\n> ", at);
    etude_type(m, "poweroff\r");
    expect(c, "stopped", etude_wait_end(m, &end), ETUDE_OK);
    etude_assert(c, "shutdown", end.end == ETUDE_END_SHUTDOWN, "not a shutdown");
    if (etude_step(c, "check") != ETUDE_OK)
        return 1;
    map[3] = (char *)over;
    expect(c, "map-ran", etude_host(c, map, "map", &end), ETUDE_OK);
    etude_assert(c, "map-written",
                 end.end == ETUDE_END_EXITED && end.value == 0 &&
                     file_has(etude_evidence(c, "map"), "\"depth\": 0"),
                 "the map lists no cluster of the overlay");
    convert[6] = (char *)over;
    convert[7] = raw_path;
    expect(c, "converted", etude_host(c, convert, "convert", &end), ETUDE_OK);
    scan[3] = raw_path;
    etude_assert(c, "raw-has",
                 end.end == ETUDE_END_EXITED && end.value == 0 &&
                     etude_host(c, scan, "scan", &end) == ETUDE_OK && end.end == ETUDE_END_EXITED &&
                     end.value == 0,
                 "the raw disk lacks the write");
    unlink(raw_path);
    if (etude_step(c, "readback") != ETUDE_OK ||
        etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    again = etude_private_copy(c, "over");
    etude_assert(c, "same-overlay", over != NULL && again == over, "another overlay");
    at = wait_for(c, m, "readback", "\n> ", wait_for(c, m, "readback", "Type 'help'", 0));
    etude_type(m, "cat /marker\r");
    if (etude_console_wait(m, nonce, at, &at) == ETUDE_OK)
        etude_assert(c, "readback", 1, NULL);
    else
        etude_assert(c, "readback", 0, "the write was not read back");
    etude_type(m, "poweroff\r");
    expect(c, "second-stop", etude_wait_end(m, &end), ETUDE_OK);
    return 0;
}

/** Read two boots' recordings with a host tool and keep failed evidence. */
static int scenario_audio(struct etude_case *c, int keep)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    const char *first = NULL, *path;
    char *scan[] = {"scanner", "tool", "scan", NULL, "recorded samples", NULL};
    int i;

    for (i = 0; i < 2; i++) {
        if (etude_step(c, i == 0 ? "boot" : "reboot") != ETUDE_OK ||
            etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
            return 1;
        wait_for(c, m, "audio-host", "> ", 0);
        etude_type(m, "poweroff\n");
        if (etude_wait_end(m, &end) != ETUDE_OK)
            return 1;
        path = etude_audio(m);
        etude_assert(c, i == 0 ? "audio-first" : "audio-second", path != NULL, "no recording path");
        if (path == NULL)
            return 1;
        scan[3] = (char *)path;
        if (etude_host(c, scan, i == 0 ? "scan-one" : "scan-two", &end) != ETUDE_OK ||
            end.value != 0)
            return 1;
        if (i == 0)
            first = path;
        else
            etude_assert(c, "audio-distinct",
                         strcmp(first, path) != 0 && file_has(first, "recorded samples"),
                         "recordings were reused");
    }
    etude_assert(c, "audio-host", 1, NULL);
    return keep;
}

/** Read a boot's capture of its second card with a host tool and keep failed
 * evidence. */
static int scenario_capture(struct etude_case *c, int keep)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    const char *path;
    char *scan[] = {"scanner", "tool", "scan", NULL, "captured frames", NULL};

    if (etude_step(c, "boot") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    wait_for(c, m, "capture-host", "> ", 0);
    etude_type(m, "poweroff\n");
    if (etude_wait_end(m, &end) != ETUDE_OK)
        return 1;
    path = etude_capture(m, "net1");
    etude_assert(c, "capture-file", path != NULL && pcap_has(path, "captured frames"),
                 "no capture of the second card");
    if (path == NULL)
        return 1;
    scan[3] = (char *)path;
    if (etude_host(c, scan, "scan", &end) != ETUDE_OK || end.value != 0)
        return 1;
    etude_assert(c, "capture-host", 1, NULL);
    return keep;
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
                     end.value == (strcmp(how, "die") == 0 ? sh_dead_status() : 0),
                 "taken for another end");
    return 0;
}

/**
 * Put in `ticks` the times of the stand-in's ticks on the console of the
 * first boot of fixture, whose evidence is whole once the machine has ended,
 * and give their count.
 */
static size_t read_ticks(struct etude_case *c, uint64_t *ticks, size_t max)
{
    const char *path = etude_evidence(c, "fixture.1.console");
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    char line[256], *at;
    size_t n = 0;

    while (f != NULL && fgets(line, sizeof(line), f) != NULL)
        if ((at = strstr(line, "tick ")) != NULL && n < max)
            ticks[n++] = strtoull(at + 5, NULL, 10);
    if (f != NULL)
        fclose(f);
    return n;
}

/**
 * Judge the `done` stops of a suspension of STALLS cycles: each held for
 * STALL_US at least; and, given the `n` ticks of the stand-in in `ticks`, no
 * tick inside a stop, and a tick after each stop and before the next, so
 * that the stand-in ran again after every stop.
 */
static void judge_stops(struct etude_case *c, const struct etude_stop *stops, unsigned done,
                        const uint64_t *ticks, size_t n)
{
    unsigned i, held = 0, idle = 0, again = 0;
    uint64_t next;
    size_t t;
    int in, after;

    for (i = 0; i < done && i < STALLS; i++) {
        next = i + 1 < done ? stops[i + 1].stopped_us : UINT64_MAX;
        in = after = 0;
        held += stops[i].resumed_us >= stops[i].stopped_us + STALL_US;
        for (t = 0; t < n; t++) {
            in |= ticks[t] > stops[i].stopped_us && ticks[t] < stops[i].resumed_us;
            after |= ticks[t] > stops[i].resumed_us && ticks[t] < next;
        }
        idle += !in;
        again += after;
    }
    etude_assert(c, "held", done == STALLS && held == STALLS, "a stop was cut short");
    if (ticks == NULL)
        return;
    etude_assert(c, "no-run", done == STALLS && idle == STALLS, "the stand-in ticked in a stop");
    etude_assert(c, "resumed", done == STALLS && again == STALLS,
                 "the stand-in did not tick after a stop");
}

/**
 * Run the suspension rows. In suspend and real-suspend the machine runs the
 * STALLS cycles of its suspension and powers off, and under the stand-in it
 * ticks all the while. In suspend-stop and suspend-kill the test stops or
 * kills the scenario during the one long stop, which the witness announces.
 * In suspend-bound the step's bound ends that stop, and the machine then
 * answers QMP and powers off in the next step.
 */
static int scenario_suspend(struct etude_case *c, const char *row)
{
    static struct etude_stop stops[STALLS];
    static uint64_t ticks[TICKS_MAX];
    int real = strcmp(row, "real-suspend") == 0, bound = strcmp(row, "suspend-bound") == 0;
    struct etude_case_machine *m;
    struct etude_termination end;
    enum etude_status st;
    unsigned done = 0;
    char *reply = NULL;
    uint64_t at;

    if (etude_step(c, "stall") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    at = wait_for(c, m, "suspended", "Type 'help'", 0);
    at = wait_for(c, m, "suspended", real ? "\n> " : "> ", at);
    if (!real) {
        etude_type(m, "tick\n");
        wait_for(c, m, "suspended", "tick ", at);
    }
    st = etude_suspend(m, "stalls", stops, STALLS, &done);
    if (strcmp(row, "suspend-stop") == 0 || strcmp(row, "suspend-kill") == 0) {
        expect(c, "suspend-stopped", st, ETUDE_STOPPED);
        return 0;
    }
    if (bound) {
        expect(c, "suspend-bounded", st, ETUDE_BOUND);
        if (etude_step(c, "after") != ETUDE_OK)
            return 1;
        expect(c, "answers", etude_qmp(m, "query-status", NULL, &reply), ETUDE_OK);
        free(reply);
    } else {
        expect(c, "suspended", st, ETUDE_OK);
    }
    etude_type(m, real ? "poweroff\r" : "poweroff\n");
    expect(c, "stopped", etude_wait_end(m, &end), ETUDE_OK);
    etude_assert(c, "shutdown", end.end == ETUDE_END_SHUTDOWN, "not a shutdown");
    if (!bound)
        judge_stops(c, stops, done, real ? NULL : ticks,
                    real ? 0 : read_ticks(c, ticks, TICKS_MAX));
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
    uint64_t at;

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
    etude_assert(c, "no-capture", etude_capture(m, "net0") == NULL, "given");
    expect(c, "no-text", etude_console_find(m, "", 0, &at), ETUDE_REFUSED);
    return 0;
}

/**
 * Run the detail row: two failures whose details a report refuses as they
 * are given, one with spaces around it and one whose record is cut at the
 * line's limit just after a space.
 */
static int scenario_detail(struct etude_case *c)
{
    char cut[300];

    memset(cut, 'x', sizeof(cut) - 1);
    cut[sizeof(cut) - 1] = '\0';
    cut[DETAIL_CUT] = ' ';
    etude_assert(c, "padded", 0, "  padded detail  ");
    etude_assert(c, "cut", 0, cut);
    return 0;
}

/** Run the rows whose machine does not boot: a failed accelerator, a
 * damaged kernel, and an unsafe memory profile. */
static int scenario_noboot(struct etude_case *c, const char *row)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    enum etude_status st;

    if (etude_step(c, "boot") != ETUDE_OK)
        return 1;
    st = etude_boot(c, "fixture", NULL, &m, &end);
    if (strcmp(row, "damaged") == 0) {
        expect(c, "refused", st, ETUDE_REFUSED);
        etude_assert(c, "damaged", etude_artifact(c, "kernel") == NULL, "the damage was taken");
    } else if (strcmp(row, "memory") == 0) {
        expect(c, "refused", st, ETUDE_FAILED);
        etude_assert(c, "memory-profile", m == NULL && end.end == ETUDE_END_QEMU_EXITED,
                     "an unsafe memory profile started");
    } else {
        expect(c, "refused", st, ETUDE_FAILED);
        etude_assert(c, "damaged", end.end == ETUDE_END_ACCEL_FAILED && m == NULL,
                     "not a failed accelerator");
    }
    return 0;
}

/**
 * Run the memory rows of two machines, a boundary machine and an ordinary
 * one. In memory-pair the case reserves the boundary machine's memory alone,
 * and a boot of either is refused before QEMU starts; in memory-share and
 * memory-qmp it reserves both, and the boundary machine boots. In memory-qmp
 * the boundary machine then answers a query and the human monitor's `info`,
 * and QMP's `object-add` and the human monitor's `migrate` are refused; its
 * console accepts `exec /memprobe total` once, and `help` and a second probe
 * are refused.
 */
static int scenario_memory(struct etude_case *c, const char *row)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    enum etude_status st;
    char *reply;

    if (etude_step(c, "boot") != ETUDE_OK)
        return 1;
    st = etude_boot(c, "fixture", NULL, &m, &end);
    if (strcmp(row, "memory-share") == 0 || strcmp(row, "memory-qmp") == 0) {
        expect(c, "admitted", st, ETUDE_OK);
        if (st != ETUDE_OK || strcmp(row, "memory-qmp") != 0)
            return 0;
        expect(c, "qmp-query", etude_qmp(m, "query-status", NULL, &reply), ETUDE_OK);
        free(reply);
        expect(c, "qmp-info",
               etude_qmp(m, "human-monitor-command", "{\"command-line\": \"info registers -a\"}",
                         &reply),
               ETUDE_OK);
        free(reply);
        expect(c, "qmp-refused",
               etude_qmp(m, "object-add",
                         "{\"qom-type\": \"memory-backend-ram\", \"id\": \"more\", \"size\": "
                         "68719476736}",
                         &reply),
               ETUDE_REFUSED);
        free(reply);
        expect(c, "hmp-refused",
               etude_qmp(m, "human-monitor-command", "{\"command-line\": \"migrate file:more\"}",
                         &reply),
               ETUDE_REFUSED);
        free(reply);
        expect(c, "type-refused", etude_type(m, "help\r"), ETUDE_REFUSED);
        expect(c, "type-probe", etude_type(m, "exec /memprobe total\r"), ETUDE_OK);
        expect(c, "type-again", etude_type(m, "exec /memprobe total\r"), ETUDE_REFUSED);
        return 0;
    }
    expect(c, "refused", st, ETUDE_REFUSED);
    etude_assert(c, "memory-pair", m == NULL && end.end == ETUDE_END_LOST,
                 "a boundary machine started beside another");
    if (etude_step(c, "other") != ETUDE_OK)
        return 1;
    expect(c, "other-refused", etude_boot(c, "other", NULL, &m, &end), ETUDE_REFUSED);
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

/* The arguments of a service's start: the tool `peer`, one of the case's
 * ports, a mode, and the mark of the row in this run, ETUDE_TEST_MARK. */
struct peer_args {
    char port[16], mark[PATH_MAX + 8];
    char *argv[6];
};

/** Make the arguments of the peer in `mode` on the case's port `i`. */
static char *const *peer_args_on(struct etude_case *c, struct peer_args *a, const char *mode,
                                 unsigned i)
{
    snprintf(a->port, sizeof(a->port), "%u", etude_port(c, i));
    snprintf(a->mark, sizeof(a->mark), "mark=%s", getenv("ETUDE_TEST_MARK"));
    a->argv[0] = "tool";
    a->argv[1] = "peer";
    a->argv[2] = a->port;
    a->argv[3] = (char *)mode;
    a->argv[4] = a->mark;
    a->argv[5] = NULL;
    return a->argv;
}

/** Make the arguments of the peer in `mode` on the case's first port. */
static char *const *peer_args(struct etude_case *c, struct peer_args *a, const char *mode)
{
    return peer_args_on(c, a, mode, 0);
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
 * a stop of every process of the case during a wait (`stop-all`), and a
 * service that dies unseen (`unseen`).
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
    if (strcmp(row, "stop-all") == 0) {
        etude_console_wait(b, "never printed", 0, &at);
        return 0;
    }
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
    const char *capture;
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
    capture = etude_capture(a, "net0");
    etude_assert(c, "alpha-captured", capture != NULL && pcap_has(capture, "hello-tcp-echo"),
                 "no frame of tcptest's in its capture");
    if (etude_service_wait(s, "peer: closed", 0, &at) == ETUDE_OK)
        expect(c, "both-closed", etude_service_wait(s, "peer: closed", at, &at), ETUDE_OK);
    else
        etude_assert(c, "both-closed", 0, "no connection closed");
    expect(c, "service-stopped", etude_service_stop(s, &end), ETUDE_OK);
    return 0;
}

/**
 * Run the media rows: a machine whose disks are an empty sparse disk of 4 TiB
 * on NVMe, a CD whose medium is the artifact `disk` and an empty disk of 1 MiB
 * on AHCI without flushes. Under the stand-in the machine boots twice, each
 * time with the same empty disk, and says the arguments it was given; under
 * the real QEMU the kernel finds the disk's size.
 */
static int scenario_media(struct etude_case *c, int real)
{
    static const char *const ids[2][3] = {{"sparse", "drives", "stopped"},
                                          {"same-disk", "drives-again", "second-stop"}};
    const char *big = NULL, *again, *cd, *blank, *shell = real ? "\n> " : "> ";
    struct etude_case_machine *m;
    struct etude_termination end;
    char text[PATH_MAX + 256];
    struct stat st;
    uint64_t at = 0;
    int boot;

    for (boot = 0; boot < (real ? 1 : 2); boot++) {
        if (etude_step(c, boot == 0 ? "boot" : "again") != ETUDE_OK ||
            etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
            return 1;
        again = etude_private_copy(c, "big");
        if (boot == 0)
            big = again;
        cd = etude_artifact(c, "disk");
        blank = etude_private_copy(c, "blank");
        etude_assert(c, ids[boot][0],
                     again != NULL && again == big && stat(big, &st) == 0 &&
                         (uint64_t)st.st_size == BIG_BYTES &&
                         (uint64_t)st.st_blocks * 512u < (1u << 20),
                     "no sparse empty disk of 4 TiB, or another at the second boot");
        if (real)
            fix_format(text, sizeof(text), ": namespace 1: 4194304 MiB");
        else
            fix_format(text, sizeof(text),
                       "fake: device ich9-ahci,id=etude-ahci\r\n"
                       "fake: drive file=%s,format=raw,if=none,id=etude-disk0,discard=unmap,"
                       "detect-zeroes=unmap\r\nfake: device nvme,drive=etude-disk0,"
                       "serial=rach2big\r\nfake: drive file=%s,format=raw,if=ide,index=0,"
                       "media=cdrom,readonly=on\r\nfake: drive file=%s,format=raw,if=none,"
                       "id=etude-disk2,cache=unsafe\r\nfake: device ide-hd,drive=etude-disk2,"
                       "bus=etude-ahci.0\r\n",
                       big != NULL ? big : "", cd != NULL ? cd : "", blank != NULL ? blank : "");
        etude_assert(c, ids[boot][1], etude_console_wait(m, text, 0, &at) == ETUDE_OK, text);
        at = wait_for(c, m, ids[boot][2], "Type 'help'", at);
        wait_for(c, m, ids[boot][2], shell, at);
        etude_type(m, real ? "poweroff\r" : "poweroff\n");
        expect(c, ids[boot][2], etude_wait_end(m, &end), ETUDE_OK);
    }
    return 0;
}

/**
 * Under the real QEMU, have machine `m`, whose shell's greeting ends at
 * `at`, reach the peer `s` through the guest forward of 10.0.2.100:7777 in
 * step relay, and be refused the peer at the gateway, 10.0.2.2, which the
 * restricted network does not forward, in step closed. The kernel may log the
 * reset between the words of tcptest's first test, so the refusal is a
 * `FAILED` after them.
 */
static void forward_real(struct etude_case *c, struct etude_case_machine *m,
                         struct etude_case_service *s, uint64_t at)
{
    char line[64];
    uint64_t from;

    if (etude_step(c, "relay") != ETUDE_OK)
        return;
    from = wait_for(c, m, "guest-forward", "\n> ", at);
    etude_type(m, "exec /tcptest peer=10.0.2.100:7777 closed=7778\r");
    expect(c, "guest-forward", etude_console_wait(m, "All TCP tests passed!", from, &at), ETUDE_OK);
    expect(c, "relay-closed", etude_service_wait(s, "peer: closed", 0, &from), ETUDE_OK);
    if (etude_step(c, "closed") != ETUDE_OK)
        return;
    from = wait_for(c, m, "restricted", "\n> ", at);
    snprintf(line, sizeof(line), "exec /tcptest peer=10.0.2.2:%u closed=9\r", etude_port(c, 2));
    etude_type(m, line);
    from = wait_for(c, m, "restricted", "Test 1: connect to echo server... ", from);
    expect(c, "restricted", etude_console_wait(m, "FAILED", from, &at), ETUDE_OK);
}

/**
 * Run the forward rows. The machine fixture has a restricted user network on
 * its first card, which forwards the case's first port by TCP to the guest's
 * port 22, its second by UDP to the guest's port 9999, and the guest's
 * connections to 10.0.2.100:7777 to its third, where the peer serves,
 * through this program as the relay; its second card has a user network
 * without forwards. Under the stand-in QEMU's `-netdev` values say so, and
 * the machine other, which forwards a fourth port the case does not hold,
 * is refused before QEMU starts. Under the real QEMU a connection to the
 * first port is accepted and the second port is bound once the shell is up,
 * and the guest reaches the peer through the guest forward alone.
 */
static int scenario_forward(struct etude_case *c, int real)
{
    struct etude_case_machine *m;
    struct etude_case_service *s = NULL;
    struct etude_termination end;
    struct peer_args pa;
    struct sockaddr_in a;
    char line[PATH_MAX + 512];
    uint64_t at;
    host_socket fd;

    if (etude_step(c, "serve") != ETUDE_OK)
        return 1;
    if (real)
        expect(c, "service-ready",
               etude_service(c, "peer", peer_args_on(c, &pa, "serve", 2), &s, &end), ETUDE_OK);
    if (etude_step(c, "boot") != ETUDE_OK)
        return 1;
    expect(c, "booted", etude_boot(c, "fixture", NULL, &m, &end), ETUDE_OK);
    if (m == NULL)
        return 0;
    if (real) {
        expect(c, "shell", etude_console_wait(m, "Type 'help'", 0, &at), ETUDE_OK);
        a = loopback((long)etude_port(c, 0));
        fd = sh_socket(SOCK_STREAM);
        etude_assert(c, "tcp-forward",
                     fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0,
                     "nothing accepts a connection at the first port");
        if (fd >= 0)
            host_socket_close(fd);
        a = loopback((long)etude_port(c, 1));
        fd = sh_socket(SOCK_DGRAM);
        etude_assert(c, "udp-forward",
                     fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 && sh_bound_error(),
                     "nothing has bound the second port");
        if (fd >= 0)
            host_socket_close(fd);
        if (s == NULL)
            return 1;
        forward_real(c, m, s, at);
        expect(c, "service-stopped", etude_service_stop(s, &end), ETUDE_OK);
        return 0;
    }
    fix_format(line, sizeof(line),
               "fake: netdev user,id=net0,restrict=on,hostfwd=tcp:127.0.0.1:%u-:22,"
               "hostfwd=udp:127.0.0.1:%u-:9999,guestfwd=tcp:10.0.2.100:7777-cmd:"
               "'%s.scratch/bin/scanner' 'tool' 'relay' %u\r",
               etude_port(c, 0), etude_port(c, 1), getenv("ETUDE_TEST_MARK"), etude_port(c, 2));
    expect(c, "forwarded", etude_console_wait(m, line, 0, &at), ETUDE_OK);
    expect(c, "second-card", etude_console_wait(m, "fake: netdev user,id=net1\r", 0, &at),
           ETUDE_OK);
    if (etude_step(c, "other") != ETUDE_OK)
        return 1;
    expect(c, "unheld-refused", etude_boot(c, "other", NULL, &m, &end), ETUDE_FAILED);
    etude_assert(c, "unheld-unstarted", m == NULL && end.end == ETUDE_END_QEMU_EXITED,
                 "a forward of a port the case does not hold started");
    return 0;
}

/** Run row `row` of a case with services or a capture row, or give 1 for an
 * unknown row. */
static int scenario_services(struct etude_case *c, const char *row)
{
    if (strncmp(row, "capture-", 8) == 0)
        return scenario_capture(c, strcmp(row, "capture-keep") == 0);
    if (strcmp(row, "pair") == 0)
        return scenario_pair(c);
    if (strcmp(row, "unready") == 0)
        return scenario_unready(c);
    if (strcmp(row, "broken") == 0 || strcmp(row, "bound-pair") == 0 ||
        strcmp(row, "stop-pair") == 0 || strcmp(row, "kill-pair") == 0 ||
        strcmp(row, "stop-all") == 0 || strcmp(row, "unseen") == 0)
        return scenario_fail(c, row);
    if (strcmp(row, "hold") == 0)
        return scenario_hold(c);
    if (strcmp(row, "real-peer") == 0)
        return scenario_real_peer(c);
    return 1;
}

/**
 * Run the fault row on two processors: the first boot panics after an NMI,
 * which etude_fault finds on the console and not before it, and powers off
 * after; the second halts without a record, which etude_fault finds in the
 * registers once both processors have halted and not before. Each boot's
 * end is recorded once, and no other end, a second fault or a later
 * shutdown, replaces it.
 */
static int scenario_fault(struct etude_case *c)
{
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at;

    if (etude_step(c, "panic") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    at = wait_for(c, m, "fault-panic", "> ", wait_for(c, m, "fault-panic", "Type 'help'", 0));
    expect(c, "early-panic", etude_fault(m, ETUDE_END_PANIC), ETUDE_FAILED);
    expect(c, "early-halt", etude_fault(m, ETUDE_END_HALTED), ETUDE_FAILED);
    etude_type(m, "nmi\n");
    wait_for(c, m, "fault-panic", "panic: stopped", at);
    expect(c, "fault-panic", etude_fault(m, ETUDE_END_PANIC), ETUDE_OK);
    expect(c, "fault-once", etude_fault(m, ETUDE_END_HALTED), ETUDE_REFUSED);
    etude_type(m, "poweroff\n");
    expect(c, "fault-ended", etude_wait_end(m, &end), ETUDE_OK);
    if (etude_step(c, "halt") != ETUDE_OK || etude_boot(c, "fixture", NULL, &m, &end) != ETUDE_OK)
        return 1;
    at = wait_for(c, m, "fault-halt", "> ", wait_for(c, m, "fault-halt", "Type 'help'", 0));
    expect(c, "not-reset", etude_fault(m, ETUDE_END_RESET), ETUDE_REFUSED);
    etude_type(m, "stop one\n");
    at = wait_for(c, m, "fault-halt", "fake: halted", at);
    expect(c, "half-halted", etude_fault(m, ETUDE_END_HALTED), ETUDE_FAILED);
    etude_type(m, "stop\n");
    wait_for(c, m, "fault-halt", "fake: halted", at);
    expect(c, "halt-no-panic", etude_fault(m, ETUDE_END_PANIC), ETUDE_FAILED);
    expect(c, "fault-halt", etude_fault(m, ETUDE_END_HALTED), ETUDE_OK);
    return 0;
}

/** Say whether `row` is one of the rows that scenario_reboot runs. */
static int reboot_row(const char *row)
{
    return strcmp(row, "reboot") == 0 || strcmp(row, "usb") == 0 ||
           strcmp(row, "real-reboot") == 0 || strcmp(row, "network") == 0;
}

/** Say whether `row` is one of the rows that scenario_suspend runs. */
static int suspend_row(const char *row)
{
    return strncmp(row, "suspend", 7) == 0 || strcmp(row, "real-suspend") == 0;
}

/** Run row `row` when its scenario takes the case alone; -1 for another row. */
static int scenario_alone(struct etude_case *c, const char *row)
{
    static const struct {
        const char *name;
        int (*run)(struct etude_case *);
    } rows[] = {{"real-overlay", scenario_overlay},
                {"detail", scenario_detail},
                {"bound", scenario_bound},
                {"fault", scenario_fault},
                {"stop", scenario_stop},
                {"misuse", scenario_misuse}};
    size_t i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        if (strcmp(row, rows[i].name) == 0)
            return rows[i].run(c);
    return -1;
}

int etude_scenario(struct etude_case *c)
{
    const char *row = getenv("ETUDE_TEST_ROW");
    int status;

    if (row == NULL)
        return 1;
    if (strncmp(row, "audio-", 6) == 0)
        return scenario_audio(c, strcmp(row, "audio-keep") == 0);
    if (reboot_row(row))
        return scenario_reboot(c, row[0] == 'r' && row[1] == 'e' && row[2] == 'a');
    if (strcmp(row, "lost") == 0 || strcmp(row, "die") == 0 || strcmp(row, "closecon") == 0 ||
        strcmp(row, "closeqmp") == 0)
        return scenario_lost(c, row);
    if ((status = scenario_alone(c, row)) != -1)
        return status;
    if (strcmp(row, "wrong") == 0 || strcmp(row, "damaged") == 0 || strcmp(row, "memory") == 0)
        return scenario_noboot(c, row);
    if (strcmp(row, "guest") == 0 || strcmp(row, "real-guest") == 0)
        return scenario_guest(c, row[0] == 'r');
    if (strncmp(row, "memory-", 7) == 0)
        return scenario_memory(c, row);
    if (strcmp(row, "media") == 0 || strcmp(row, "real-media") == 0)
        return scenario_media(c, row[0] == 'r');
    if (strcmp(row, "forward") == 0 || strcmp(row, "real-forward") == 0)
        return scenario_forward(c, row[0] == 'r');
    if (suspend_row(row))
        return scenario_suspend(c, row);
    return scenario_services(c, row);
}

/* ---- The rows. ---- */

/* One attempt, as the test plans and judges it. */
struct row {
    const char *name;    /* the row, ETUDE_TEST_ROW, and the case's last part */
    const char *mode;    /* the stand-in's mode, or NULL for a real QEMU */
    const char *steps;   /* the case's steps, `id:machine` or `id`, by spaces */
    const char *asserts; /* its assertions, by spaces */
    const char *disk;    /* the machine's disk: "disk", "over" for an overlay
                            over it, or NULL */
    int helper;          /* the machine runs etude-guest */
    uint64_t bound_us;   /* each step's bound */
    int want_status;     /* the scenario's exit status */
    int want_end;        /* its report ends with `end` */
    int want_kept;       /* it keeps its scratch */
    int pair;            /* the case has machines alpha and beta, the service
                            peer and a port */
    const char *odd;     /* a step whose bound is `odd_us`, or NULL */
    uint64_t odd_us;
    const char *evidence; /* `FILE=TEXT` pairs by `;`, which the scratch's
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

/* The machines of the memory rows: a boundary machine, x86-64 alone, and an
 * ordinary one. */
static const char memory_machines[] =
    "{\"machine_id\": \"fixture\", \"qemu\": {\"kernel\": \"kernel\", \"initrd\": \"initrd\", "
    "\"disks\": [], \"cpu\": \"max-nopdpe1gb\", \"processors\": 1, \"memory_bytes\": "
    "77309411328, \"memory_profile\": \"bigmem-boundary-v1\", \"firmware\": \"direct\", "
    "\"devices\": [], \"boot_options\": [\"rach2.rescue\"]}, \"console\": \"serial\", \"qmp\": "
    "true, \"helper\": false}, {\"machine_id\": \"other\", \"qemu\": {\"kernel\": \"kernel\", "
    "\"initrd\": null, \"disks\": [], \"cpu\": \"max\", \"processors\": 1, \"memory_bytes\": "
    "134217728, \"firmware\": \"direct\", \"devices\": [], \"boot_options\": []}, \"console\": "
    "\"serial\", \"qmp\": true, \"helper\": false}";

/** Say whether row `r` is a memory row of two machines, whose case runs on
 * x86-64 at a site that permits the boundary profile. */
static int boundary_pair(const struct row *r)
{
    return strncmp(r->name, "memory-", 7) == 0;
}

/** Say whether row `r` is a media row, whose machine has the RAM image and
 * the disks MEDIA_DISKS. */
static int media(const struct row *r)
{
    return strcmp(r->name, "media") == 0 || strcmp(r->name, "real-media") == 0;
}

/* The disk of a row whose machine boots a qcow2 overlay over the disk. */
static const char overlay_disk[] =
    "[{\"name\": \"over\", \"base\": \"disk\", \"cluster_bytes\": 4096, \"discard\": true, "
    "\"serial\": \"ETUDE-OVERLAY\"}]";

/** Say whether row `r`'s machine boots an overlay, which `qemu-img` reads. */
static int overlay_row(const struct row *r)
{
    return r->disk != NULL && strcmp(r->disk, "over") == 0;
}

/** Give the `disks` of row `r`'s machine, as JSON; a real row's disk is on
 * AHCI, and an overlay on IDE. */
static const char *row_disks(const struct row *r)
{
    if (media(r))
        return MEDIA_DISKS;
    if (r->disk == NULL)
        return "[]";
    if (overlay_row(r))
        return overlay_disk;
    return r->mode == NULL ? "[{\"name\": \"disk\", \"bus\": \"ahci\"}]" : "[\"disk\"]";
}

/* The machines of the forward rows: fixture, whose first card has a
 * restricted network that forwards the case's first two ports to the guest
 * and the guest's connections to 10.0.2.100:7777 to its third, and other,
 * which forwards a fourth that the case does not hold. The first `%s` is
 * fixture's initrd. */
static const char forward_machines[] =
    "{\"machine_id\": \"fixture\", \"qemu\": {\"kernel\": \"kernel\", \"initrd\": %s, "
    "\"disks\": [], \"cpu\": \"max\", \"processors\": 1, \"memory_bytes\": 134217728, "
    "\"firmware\": \"direct\", \"devices\": [{\"model\": \"rtl8139\", \"restrict\": true, "
    "\"forwards\": [{\"protocol\": \"tcp\", \"port\": 0, \"guest_port\": 22}, "
    "{\"protocol\": \"udp\", \"port\": 1, \"guest_port\": 9999}], \"guest_forwards\": "
    "[{\"address\": \"10.0.2.100\", \"guest_port\": 7777, \"port\": 2, \"relay\": "
    "[\"scanner\", \"tool\", \"relay\"]}]}, \"e1000\"], \"boot_options\": []}, "
    "\"console\": \"serial\", \"qmp\": true, \"helper\": false}, {\"machine_id\": "
    "\"other\", \"qemu\": {\"kernel\": \"kernel\", \"initrd\": null, \"disks\": [], "
    "\"cpu\": \"max\", \"processors\": 1, \"memory_bytes\": 134217728, \"firmware\": "
    "\"direct\", \"devices\": [{\"model\": \"rtl8139\", \"forwards\": [{\"protocol\": "
    "\"tcp\", \"port\": 3, \"guest_port\": 22}]}], \"boot_options\": []}, \"console\": "
    "\"serial\", \"qmp\": true, \"helper\": false}";

/* The service of the forward rows, the peer that the guest forward reaches. */
static const char forward_services[] =
    ", \"services\": [{\"service_id\": \"peer\", \"program\": \"scanner\", \"ready\": \"peer: "
    "listening\", \"does\": \"echo what the guest forward relays to the case's third port\"}]";

/* The case's ports in a forward row, each free when the row starts: the
 * first for TCP and the second for UDP. The third is the peer's, row_port. */
static unsigned forward_ports[2];

/** Say whether row `r` is a forward row, whose case has three ports. */
static int forward_row(const struct row *r)
{
    return strcmp(r->name, "forward") == 0 || strcmp(r->name, "real-forward") == 0;
}

/** Give a port of 127.0.0.1 that is free now for sockets of `type`, or 0. */
static unsigned free_port(int type)
{
    struct sockaddr_in a = loopback(0);
    sh_socklen len = sizeof(a);
    unsigned port = 0;
    host_socket fd = sh_socket(type);

    if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &len) == 0)
        port = ntohs(a.sin_port);
    if (fd >= 0)
        host_socket_close(fd);
    return port;
}

/**
 * Give in `holds` the workload's suspensions of row `r`, and in `witness` the
 * counter witness of their stops, as members of JSON objects; both are empty
 * for a row with none. The suspend and real-suspend rows stop fixture STALLS
 * times; the other suspension rows stop it once, for LONG_STOP_US.
 */
static void row_suspensions(const struct row *r, char *holds, size_t hsize, char *witness,
                            size_t wsize)
{
    int all = strcmp(r->name, "suspend") == 0 || strcmp(r->name, "real-suspend") == 0;

    holds[0] = witness[0] = '\0';
    if (!suspend_row(r->name))
        return;
    fix_format(holds, hsize,
               ", \"suspensions\": [{\"suspension_id\": \"stalls\", \"machine\": \"fixture\", "
               "\"cycles\": %u, \"run_us\": %u, \"stop_us\": %u, \"witness\": \"stalls\"}]",
               all ? STALLS : 1, all ? STALL_RUN_US : 1000u, all ? STALL_US : LONG_STOP_US);
    fix_format(witness, wsize,
               ", {\"witness_id\": \"stalls\", \"kind\": \"counter\", \"means\": \"a stop was "
               "seen\", \"limit\": %u}",
               STALLS);
}

/** Write row `r`'s case, as one family's manifest, to `path`. A memory row
 * of two machines reserves the boundary machine's memory, and in
 * memory-share and memory-qmp the other's as well. */
static int write_manifest(const char *path, const struct row *r)
{
    char steps[2048], asserts[4096], machine[4096], text[16384], holds[512], witness[256];
    unsigned long long reserved = 268435456;

    json_list(steps, sizeof(steps), r->steps, 1);
    json_list(asserts, sizeof(asserts), r->asserts, 0);
    row_suspensions(r, holds, sizeof(holds), witness, sizeof(witness));
    if (boundary_pair(r)) {
        fix_format(machine, sizeof(machine), "%s", memory_machines);
        reserved = strcmp(r->name, "memory-pair") != 0 ? 4563402752ull : 4294967296ull;
    } else if (forward_row(r)) {
        fix_format(machine, sizeof(machine), forward_machines,
                   r->mode == NULL ? "\"initrd\"" : "null");
        reserved = 536870912;
    } else if (strcmp(r->name, "usb") == 0) {
        fix_format(
            machine, sizeof(machine),
            "{\"machine_id\": \"fixture\", \"qemu\": {\"kernel\": \"kernel\", \"initrd\": null, "
            "\"disks\": [{\"name\": \"disk\", \"bus\": \"device\", \"id\": \"stick\"}], "
            "\"cpu\": \"max\", \"processors\": 1, \"memory_bytes\": 134217728, \"firmware\": "
            "\"direct\", \"devices\": [{\"model\": \"qemu-xhci\", \"id\": \"xhci\", "
            "\"properties\": {\"msi\": \"on\"}}, {\"model\": \"usb-storage\", \"bus\": "
            "\"xhci.0\", \"port\": \"2\", \"drive\": \"stick\"}], \"boot_options\": [], "
            "\"i8042\": false}, \"console\": \"serial\", \"qmp\": true, \"helper\": false}");
    } else {
        fix_format(
            machine, sizeof(machine),
            "{\"machine_id\": \"fixture\", \"qemu\": {\"kernel\": \"kernel\", \"initrd\": %s, "
            "\"disks\": %s, \"cpu\": \"max\", \"processors\": %d, \"memory_bytes\": 134217728, "
            "%s\"firmware\": \"direct\", \"devices\": %s, \"boot_options\": %s%s}, \"console\": "
            "\"serial\", \"qmp\": %s, \"helper\": %s}",
            r->helper || media(r) ? "\"initrd\"" : "null", row_disks(r),
            strcmp(r->name, "fault") == 0 ? 2 : 1,
            strcmp(r->name, "memory") == 0       ? "\"memory_profile\": \"bigmem-boundary-v1\", "
            : strncmp(r->name, "audio-", 6) == 0 ? "\"audio\": \"hda\", "
                                                 : "",
            strcmp(r->name, "network") == 0
                ? "[{\"model\":\"e1000\",\"net\":\"10.90.0.0/16\",\"host\":\"10.90.0.2\","
                  "\"dhcpstart\":\"10.90.0.20\",\"dns\":\"10.90.0.3\","
                  "\"mac\":\"52:54:00:12:34:58\"},\"rtl8139\"]"
            : strncmp(r->name, "capture-", 8) == 0 ? "[\"rtl8139\", \"vga\", \"e1000\"]"
                                                   : "[]",
            r->mode == NULL && r->disk != NULL ? "[\"rach2.fsdisk\"]" : "[]",
            strncmp(r->name, "capture-", 8) == 0 ? ", \"captures\": [\"net1\"]" : "",
            strcmp(r->name, "misuse") == 0 ? "false" : "true", r->helper ? "true" : "false");
    }
    fix_format(
        text, sizeof(text),
        "{\"schema_version\": 1, \"family_id\": \"test.scenario\", \"summary\": \"rows\", "
        "\"cases\": [{\"case_id\": \"test.scenario.%s\", \"behavior\": \"a row\", \"units\": "
        "[\"none\"], \"domain\": \"machine\", \"architectures\": [\"i686\", \"x86_64\"], "
        "\"workload\": {\"arguments\": [], \"inputs\": {}, \"repetitions\": 1, \"seed\": null%s}, "
        "\"requirements\": {\"host_api\": \"c\", \"tools\": %s, \"features\": {}}, "
        "\"execution\": {\"program\": \"scenario\", \"stdin\": null, \"role\": \"ordinary\", "
        "\"privileges\": \"an ordinary user\", \"isolation\": [{\"fixture\": \"scratch\"}, "
        "{\"fixture\": \"disk\", \"names\": [\"disk\"]}], \"children\": \"none\", \"cleanup\": "
        "\"the library\"}, \"steps\": %s, \"machines\": [%s]%s, \"assertions\": %s, "
        "\"branches\": [], \"termination\": {\"end\": \"exited\", \"value\": 0}, \"progress\": "
        "[{\"witness_id\": \"stage\", \"kind\": \"step\", \"means\": \"a step "
        "ended\", \"limit\": 8}, {\"witness_id\": \"waiting\", \"kind\": \"quiet\", \"means\": "
        "\"a wait\", \"limit\": null, \"bound\": {\"floor_us\": 1, \"initial_us\": 2, "
        "\"ceiling_us\": 3}}%s], \"bounds\": {\"case\": {\"floor_us\": 1, \"initial_us\": 2, "
        "\"ceiling_us\": 3}, \"gap\": {\"floor_us\": 1, \"initial_us\": 2, \"ceiling_us\": 3}, "
        "\"stages\": {}}, \"resources\": {\"cpu_millis\": 1000, \"cpu_ceiling_millis\": 1000, "
        "\"memory_bytes\": %llu, \"disk_bytes\": 67108864, \"locks\": [], \"ports\": %d, "
        "\"loop_devices\": 0}, \"prepare\": [{\"name\": \"kernel\", \"node\": \"k\", "
        "\"per_architecture\": true}, {\"name\": \"disk\", \"node\": \"d\", \"per_architecture\": "
        "true}, {\"name\": \"initrd\", \"node\": \"i\", \"per_architecture\": true}, {\"name\": "
        "\"scanner\", \"node\": \"s\", \"per_architecture\": false}, {\"name\": \"invocation\", "
        "\"node\": \"v\", \"per_architecture\": false}, {\"name\": \"killer\", \"node\": \"x\", "
        "\"per_architecture\": false}], \"timing\": [], \"native\": {\"plan\": \"none\", "
        "\"class\": \"boot\", \"reason\": \"a test\"}}]}",
        r->name, holds, overlay_row(r) ? "[\"qemu-img\"]" : "[]", steps, machine,
        forward_row(r) ? forward_services : "", asserts, witness, reserved, forward_row(r) ? 3 : 0);
    return write_file(path, text, strlen(text));
}

/** Write row `r`'s case of two machines and a service to `path`: under the
 * stand-in each machine has a disk of its own, and under the real QEMU each
 * boots the test image with a network card, alpha's recorded. */
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
        "\"firmware\": \"direct\", \"devices\": %s, \"boot_options\": []%s}, \"console\": "
        "\"serial\", \"qmp\": true, \"helper\": false}, {\"machine_id\": \"beta\", \"qemu\": "
        "{\"kernel\": \"kernel\", \"initrd\": %s, \"disks\": %s, \"cpu\": \"max\", "
        "\"processors\": 1, \"memory_bytes\": 134217728, \"firmware\": \"direct\", "
        "\"devices\": %s, \"boot_options\": []}, \"console\": \"serial\", \"qmp\": true, "
        "\"helper\": false}]",
        real ? "\"initrd\"" : "null", real ? "[]" : "[\"disk\"]", real ? "[\"rtl8139\"]" : "[]",
        real ? ", \"captures\": [\"net0\"]" : "", real ? "\"initrd\"" : "null",
        real ? "[]" : "[\"disk2\"]", real ? "[\"rtl8139\"]" : "[]");
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
        "\"memory_bytes\": 536870912, \"disk_bytes\": 67108864, \"locks\": [], \"ports\": 1, "
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
    char qemu_img[PATH_MAX]; /* the overlay row's tool, or "" */
    const char *arch;
    char kernel[65], disk[65], initrd[65], scanner[65], inv[65], killer[65], broken[65];
};

/** Write row `r`'s invocation to `path`, with the scratch `scratch`. */
static int write_scenario(const char *path, const struct row *r, const struct site *s,
                          const char *manifest, const char *scratch)
{
    char steps[1024] = "", one[160], word[64], text[8192], ports[48] = "";
    char tools[PATH_MAX + 32] = "";
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
    if (forward_row(r))
        fix_format(ports, sizeof(ports), "%u, %u, %u", forward_ports[0], forward_ports[1],
                   row_port);
    if (overlay_row(r))
        fix_format(tools, sizeof(tools), "\"qemu-img\": \"%s\"", s->qemu_img);
    fix_format(text, sizeof(text),
               "{\"schema_version\": 1, \"run_id\": \"test-run\", \"attempt_id\": \"test-%s\", "
               "\"manifest\": \"%s\", \"case_id\": \"test.scenario.%s\", \"architecture\": "
               "\"%s\", \"store\": \"%s\", \"artifacts\": {\"kernel\": \"%s\", \"disk\": \"%s\", "
               "\"initrd\": \"%s\", \"scanner\": \"%s\", \"invocation\": \"%s\", \"killer\": "
               "\"%s\", \"disk2\": \"%s\", \"broken\": \"%s\"}, \"tools\": {%s}, \"site\": "
               "{\"accel\": \"tcg\", \"boundary_lazy\": %s, \"qemu_i686\": \"%s\", "
               "\"qemu_x86_64\": \"%s\", \"ovmf_i686\": null, \"ovmf_x86_64\": null}, "
               "\"steps\": {%s}, \"ports\": [%s], \"cleanup_us\": 30000000, \"scratch\": \"%s\", "
               "\"grace_us\": %u, "
               "\"console_max_bytes\": 16777216, \"output_max_bytes\": 1048576, "
               "\"resend_us\": 200000}",
               r->name, manifest, r->name, boundary_pair(r) ? "x86_64" : s->arch, s->store_dir,
               s->kernel, s->disk, s->initrd, s->scanner, s->inv, s->killer, s->disk, s->broken,
               tools, boundary_pair(r) ? "true" : "false", s->qemu_i686, s->qemu_x86_64, steps,
               ports, scratch, r->mode != NULL ? 300000u : 5000000u);
    return write_file(path, text, strlen(text));
}

/* What one attempt gave. */
struct outcome {
    char out[65536]; /* its standard output */
    size_t len;
    int status; /* its exit status, or -1 */
    int kept;   /* it said that it kept its scratch */
};

/* The controller's bound TCP socket and the Unix listener that passes it. */
struct peer_fixture {
    host_socket tcp, handoff;
    char path[PATH_MAX];
};

/** Pass `peer`'s TCP socket to one service that has connected to it. */
static void peer_handoff(struct peer_fixture *peer)
{
#ifdef _WIN32
    host_socket connection = (host_socket)accept((SOCKET)peer->handoff, NULL, NULL);
    CHECK(connection >= 0 && sh_send_socket(connection, peer->tcp) == 0,
          "the peer receives its bound socket");
    if (connection >= 0)
        host_socket_close(connection);
#else
    struct msghdr msg;
    struct cmsghdr *c;
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    int passed_fd = (int)peer->tcp;
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
    c->cmsg_len = CMSG_LEN(sizeof(passed_fd));
    memcpy(CMSG_DATA(c), &passed_fd, sizeof(passed_fd));
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
#endif
}

/* One attempt that runs: its process and the pipe of its output. */
struct attempt {
    const struct row *r;
    long pid;
    int fd;
#ifdef _WIN32
    struct sh_child child;
#endif
    int stopped;
    char err[PATH_MAX];
    struct outcome *o;
    struct peer_fixture *peer;
};

/** Start the scenario of row `r` with invocation `inv` and the environment
 * `env`. */
static void attempt_start(struct attempt *a, const struct row *r, const char *inv, char **env,
                          struct outcome *o)
{
    char *argv[] = {self, (char *)inv, NULL};
#ifndef _WIN32
    int out[2], fd;
#endif

    memset(a, 0, sizeof(*a));
    memset(o, 0, sizeof(*o));
    a->r = r;
    a->o = o;
    a->fd = -1;
    o->status = -1;
    fix_format(a->err, sizeof(a->err), "%s/%s.err", fix_dir(), r->name);
#ifdef _WIN32
    if (sh_start(&a->child, self, argv, env, a->err, 1) == 0) {
        a->pid = a->child.pid;
        a->fd = 0;
    }
#else
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
#endif
}

/**
 * Read the attempt's output until it has `want`, which gives 1, or ends,
 * which gives 0; when `stop_at` is not NULL, send the row's stop signal once
 * the output has it.
 */
static int attempt_read(struct attempt *a, const char *want, const char *stop_at)
{
    struct outcome *o = a->o;
    struct sh_poll p[2];
    ssize_t n;

    while (a->fd >= 0) {
        if (want != NULL && strstr(o->out, want) != NULL)
            return 1;
#ifdef _WIN32
        p[0].fd = a->peer->handoff;
        p[0].events = SH_POLLIN;
        if (p[0].fd >= 0 && sh_poll(p, 1, 0) > 0 && p[0].revents)
            peer_handoff(a->peer);
        n = sh_read(&a->child, o->out + o->len, sizeof(o->out) - 1 - o->len);
        if (n == 0) {
            host_sleep_ms(10);
            continue;
        }
#else
        p[0].fd = a->fd;
        p[1].fd = a->peer->handoff;
        p[0].events = p[1].events = SH_POLLIN;
        if (sh_poll(p, 2, -1) <= 0)
            continue;
        if (p[1].revents & SH_POLLIN)
            peer_handoff(a->peer);
        if (p[0].revents == 0)
            continue;
        n = read(a->fd, o->out + o->len, sizeof(o->out) - 1 - o->len);
#endif
        if (n <= 0)
            break;
        o->len += (size_t)n;
        o->out[o->len] = '\0';
        if (stop_at != NULL && !a->stopped && strstr(o->out, stop_at) != NULL) {
            a->stopped = 1;
            sh_signal(a->pid, a->r->stop_sig != 0 ? a->r->stop_sig : SIGTERM);
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
#ifndef _WIN32
    int st;
#endif

    attempt_read(a, NULL, stop_at);
#ifdef _WIN32
    o->status = sh_wait(&a->child);
#else
    if (a->fd >= 0)
        close(a->fd);
    a->fd = -1;
    if (a->pid > 0 && waitpid(a->pid, &st, 0) == a->pid && WIFEXITED(st))
        o->status = WEXITSTATUS(st);
#endif
    a->fd = -1;
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

/** Whether the evidence in scratch `scratch` has a file whose name ends in
 * `suffix`. */
static int evidence_keeps(const char *scratch, const char *suffix)
{
    char path[PATH_MAX];
    struct dirent *e;
    DIR *d;
    size_t n, k = strlen(suffix);
    int found = 0;

    fix_format(path, sizeof(path), "%s/evidence", scratch);
    d = opendir(path);
    while (d != NULL && (e = readdir(d)) != NULL) {
        n = strlen(e->d_name);
        if (n >= k && strcmp(e->d_name + n - k, suffix) == 0)
            found = 1;
    }
    if (d != NULL)
        closedir(d);
    return found;
}

/** Check that the scratch `scratch` of row `r`, which passed, has nothing left
 * but the machines' consoles in `evidence`, which the executor removes. */
static void judge_left(const struct row *r, const char *scratch)
{
    char path[PATH_MAX];
    struct dirent *e;
    DIR *d = opendir(scratch);
    size_t n;

    while (d != NULL && (e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 &&
            strcmp(e->d_name, "evidence") != 0) {
            printf("etude-scenario-test: fail: %s: its scratch keeps %s\n", r->name, e->d_name);
            fails++;
        }
    if (d != NULL)
        closedir(d);
    fix_format(path, sizeof(path), "%s/evidence", scratch);
    d = opendir(path);
    while (d != NULL && (e = readdir(d)) != NULL) {
        n = strlen(e->d_name);
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 &&
            (n < 8 || strcmp(e->d_name + n - 8, ".console") != 0)) {
            printf("etude-scenario-test: fail: %s: its evidence keeps %s\n", r->name, e->d_name);
            fails++;
        }
    }
    if (d != NULL)
        closedir(d);
}

/** Check outcome `o` of row `r`: each assertion passed, the end record and
 * the exit status are as the row says, the machine records of the reboot and
 * fault rows give each boot's end once, and the scratch is kept or not, with
 * the consoles in either case. */
/** Check the detail row's records: each detail without the spaces around it
 * or the space where its record was cut, as a report requires. */
static void judge_detail(const struct outcome *o)
{
    char rec[300];

    CHECK(has_record(o, "assert padded fail padded detail"), "detail padded");
    memcpy(rec, CUT_HEAD, sizeof(CUT_HEAD) - 1);
    memset(rec + sizeof(CUT_HEAD) - 1, 'x', DETAIL_CUT);
    rec[sizeof(CUT_HEAD) - 1 + DETAIL_CUT] = '\0';
    CHECK(has_record(o, rec), "detail cut");
}

static void judge(const struct row *r, const struct outcome *o, const char *scratch)
{
    char word[64], rec[160], copies[4352];
    const char *p = r->asserts, *first;
    struct stat st;
    size_t n;

    snprintf(rec, sizeof(rec), "begin test.scenario.%s", r->name);
    CHECK(has_record(o, rec), r->name);
    if (strcmp(r->name, "detail") == 0) {
        judge_detail(o);
        p = "";
    }
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
    CHECK(strstr(o->out, " fail ") == NULL || strcmp(r->name, "detail") == 0, r->name);
    if (strcmp(r->name, "reboot") == 0)
        CHECK(has_record(o, "machine fixture.1 shutdown") &&
                  has_record(o, "machine fixture.2 shutdown"),
              r->name);
    if (strcmp(r->name, "fault") == 0) {
        first = strstr(o->out, "@etude 1 machine fixture.1 ");
        CHECK(has_record(o, "machine fixture.2 halted"), r->name);
        CHECK(first != NULL && first == strstr(o->out, "@etude 1 machine fixture.1 panic\n") &&
                  strstr(first + 1, "@etude 1 machine fixture.1 ") == NULL,
              r->name);
    }
    CHECK((strstr(o->out, "@etude 1 end ") != NULL) == r->want_end, r->name);
    CHECK(o->status == r->want_status, r->name);
    CHECK(o->kept == r->want_kept, r->name);
    snprintf(copies, sizeof(copies), "%s/copies", scratch);
    CHECK((stat(copies, &st) == 0) == (r->want_kept && r->disk != NULL), r->name);
    if (strcmp(r->name, "audio-pass") == 0)
        CHECK(!evidence_keeps(scratch, ".wav"), "audio passing evidence removed");
    if (strcmp(r->name, "capture-pass") == 0)
        CHECK(!evidence_keeps(scratch, ".pcap"), "capture passing evidence removed");
    if (r->want_kept || r->evidence != NULL)
        judge_evidence(r, scratch);
    if (!r->want_kept)
        judge_left(r, scratch);
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
    sh_socklen len = sizeof(a);

#ifdef _WIN32
    unsigned short handoff_port;
    FILE *file;
    peer->tcp = sh_socket(SOCK_STREAM);
    peer->handoff = (host_socket)etude_win_listen(&handoff_port);
    if (peer->tcp < 0 || peer->handoff < 0 ||
        bind((SOCKET)peer->tcp, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname((SOCKET)peer->tcp, (struct sockaddr *)&a, &len) != 0)
        return 0;
    fix_format(peer->path, sizeof(peer->path), "%s.peer", mark);
    file = fopen(peer->path, "w");
    if (file == NULL)
        return 0;
    fprintf(file, "%u\n", (unsigned)handoff_port);
    if (fclose(file) != 0)
        return 0;
    return ntohs(a.sin_port);
#else
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
#endif
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
static void row_start(struct row_run *rr, const struct row *r, const struct site *s)
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
    rr->port = row_port = r->pair || forward_row(r) ? reserve_peer(&rr->peer, mark) : 0;
    if (forward_row(r)) {
        forward_ports[0] = free_port(SOCK_STREAM);
        forward_ports[1] = free_port(SOCK_DGRAM);
    }
    fix_format(manifest, sizeof(manifest), "%s/%s.manifest.json", fix_dir(), r->name);
    fix_format(inv, sizeof(inv), "%s/%s.invocation.json", fix_dir(), r->name);
    fix_format(rr->scratch, sizeof(rr->scratch), "%s/%s.scratch", fix_dir(), r->name);
    if ((r->pair && row_port == 0) ||
        (forward_row(r) &&
         (row_port == 0 || forward_ports[0] == 0 || forward_ports[1] == 0 ||
          forward_ports[0] == forward_ports[1] || forward_ports[1] == row_port)) ||
        mkdir(rr->scratch, 0700) != 0 ||
        (r->pair ? write_pair_manifest(manifest, r) : write_manifest(manifest, r)) != 0 ||
        write_scenario(inv, r, s, manifest, rr->scratch) != 0) {
        printf("etude-scenario-test: fail: %s: its files cannot be written\n", r->name);
        fails++;
        return;
    }
    attempt_start(&rr->a, r, inv, rr->env, rr->o);
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
        host_socket_close(rr->peer.tcp);
    if (rr->peer.handoff >= 0)
        host_socket_close(rr->peer.handoff);
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

    row_start(&rr, r, s);
    row_finish(&rr, stop_at, 1);
}

/** Order two PIDs for qsort. */
static int pid_order(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;

    return x < y ? -1 : x > y;
}

struct matching_processes {
    const char *need;
    const char *const *any;
    long *pids;
    int n;
};

/* Every matching argument contains this run's private fixture path. */
static int match_process(long pid, const char *arguments, void *ctx)
{
    struct matching_processes *m = ctx;
    int i;

    for (i = 0; m->any[i] != NULL; i++)
        if (strstr(arguments, m->any[i]) != NULL)
            break;
    if (m->any[i] == NULL || (m->need != NULL && strstr(arguments, m->need) == NULL))
        return 0;
    if (m->n == ROW_PROCS)
        return -1;
    m->pids[m->n++] = pid;
    return 0;
}

/** Find only this run's processes through native argument-vector queries. */
static int find_procs(const char *need, const char *const any[], long pids[ROW_PROCS])
{
    struct matching_processes m = {need, any, pids, 0};

    if (host_process_arguments(match_process, &m) != 0)
        return -1;
    qsort(pids, (size_t)m.n, sizeof(pids[0]), pid_order);
    return m.n;
}

/**
 * Put in `pids`, sorted, the processes of row `name` in this run, found by
 * their arguments: the scenario and its keepers by its invocation, the
 * stand-ins for QEMU and the services by the copies and programs in its
 * scratch, and its services by their mark; give their count, or -1 when
 * native process queries fail. Each of the three contains a path in this
 * run's fixture directory.
 */
static int row_procs(const char *name, long pids[ROW_PROCS])
{
    char inv[PATH_MAX + 64], scratch[PATH_MAX + 64], mark[PATH_MAX + 64];
    const char *any[] = {inv, scratch, mark, NULL};

    fix_format(inv, sizeof(inv), "%s/%s.invocation.json", fix_dir(), name);
    fix_format(scratch, sizeof(scratch), "%s/%s.scratch/", fix_dir(), name);
    fix_format(mark, sizeof(mark), "mark=%s/%s", fix_dir(), name);
    return find_procs(NULL, any, pids);
}

/** Wait until every process of row `name` has ended. */
static void row_gone(const char *name)
{
    long pids[ROW_PROCS];
    int n;

    while ((n = row_procs(name, pids)) > 0)
        /* interval: the row's processes are counted again every 100 ms */
        host_sleep_ms(100);
    if (n < 0) {
        printf("etude-scenario-test: fail: %s: its processes cannot be counted\n", name);
        fails++;
    }
}

/** Send `text` as a line to the peer on `port`, as a machine would. */
static int tell_peer(unsigned port, const char *text)
{
    struct sockaddr_in a = loopback((long)port);
    char buf[64];
    host_socket fd = sh_socket(SOCK_STREAM);
    int ok;

    ok = fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
         host_socket_write(fd, text, strlen(text)) == (ssize_t)strlen(text) &&
         host_socket_read(fd, buf, sizeof(buf)) > 0;
    if (fd >= 0)
        host_socket_close(fd);
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

/* The row whose every process, its keepers among them, gets SIGTERM during
 * its wait, as a stop sent to every process of a run gives it. Its wait ends
 * by the stop or by the loss of its machine, whichever the scenario sees
 * first, so it asserts nothing of the wait. */
static const struct row stop_all_row[] = {
    {"stop-all", "disk", "serve first:alpha second:beta wait", "service-ready", "disk", 0, STEP_US,
     1, 0, 1, 1, NULL, 0, "peer.1.out=peer: listening", SIGTERM},
};

/* What stops a row during its wait. */
#define WAITING "@etude 1 progress waiting 0\n"

/**
 * Run the stop-all row: once it waits, send SIGTERM to each of its
 * processes, its scenario, its keepers, its service and its machines. Each
 * keeper must outlive the signal and still report its process's end, so that
 * the scenario sees the service ended by the stop and not by itself.
 */
static void test_stop_all(const struct site *s)
{
    struct row_run rr;
    char err[PATH_MAX];
    long pids[ROW_PROCS];
    int n, i;

    row_start(&rr, stop_all_row, s);
    if (rr.started && attempt_read(&rr.a, WAITING, NULL)) {
        n = row_procs("stop-all", pids);
        for (i = 0; i < n; i++)
            sh_signal(pids[i], SIGTERM);
        rr.a.stopped = 1;
    }
    CHECK(rr.a.stopped, "stop-all: every process stopped during its wait");
    fix_format(err, sizeof(err), "%s/stop-all.err", fix_dir());
    row_finish(&rr, NULL, 1);
    CHECK(!file_has(err, "ended before the case stopped it"),
          "stop-all: the service's end by the stop was seen");
}

/**
 * Run the hold row, and while its service and machines run, each failing
 * row, the killed one and the stop-all row: each must end every process of
 * its own, and leave the hold row's processes as they were, which then still
 * answer.
 */
static void test_isolation(const struct site *s)
{
    const size_t nfail = sizeof(fail_rows) / sizeof(fail_rows[0]);
    long before[ROW_PROCS], after[ROW_PROCS];
    struct row_run hold, killed;
    int nb, na;
    size_t i;

    row_start(&hold, hold_row, s);
    if (!hold.started || !attempt_read(&hold.a, WAITING, NULL)) {
        printf("etude-scenario-test: fail: hold: its service and machines did not start\n");
        fails++;
        if (hold.started)
            sh_signal(hold.a.pid, SIGKILL);
        row_finish(&hold, NULL, 1);
        row_gone("hold");
        return;
    }
    nb = row_procs("hold", before);
#ifdef _WIN32
    CHECK(nb >= 4, "hold: its scenario, service and machines run in owned jobs");
#else
    CHECK(nb >= 6, "hold: its scenario, keepers, service and machines run");
#endif
    for (i = 0; i <= nfail + 1; i++) {
        const struct row *r = i < nfail ? &fail_rows[i] : i == nfail ? kill_row : stop_all_row;

        if (r == kill_row) {
            row_start(&killed, r, s);
            row_finish(&killed, WAITING, 0);
            CHECK(killed.a.stopped, "kill-pair: killed during its wait");
        } else if (r == stop_all_row) {
            test_stop_all(s);
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
    char dir[PATH_MAX + 2];
    const char *any[] = {dir, NULL};
    long pids[ROW_PROCS];

    fix_format(dir, sizeof(dir), "%s/", fix_dir());
    return find_procs(NULL, any, pids) != 0;
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

/** Give the path of `name` on PATH in `out`, or NULL when it is not there. */
static const char *on_path(const char *name, char *out, size_t size)
{
    return host_program(name, out, size);
}

/* The store of the real rows, under the tree's build/tests, since it has a
 * disk; removed at the test's exit. */
static char real_store[PATH_MAX];

/** Remove the real rows' store. */
static void remove_real_store(void)
{
    if (real_store[0] == '\0')
        return;
    if (fix_remove_tree(real_store) != 0)
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
        on_path("qemu-system-x86_64", s->qemu_x86_64, sizeof(s->qemu_x86_64)) == NULL ||
        on_path("qemu-img", s->qemu_img, sizeof(s->qemu_img)) == NULL) {
        printf("etude-scenario-test: fail: cannot run the boots: no qemu-system-i386, "
               "qemu-system-x86_64 and qemu-img on PATH\n");
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
static const struct row audio_rows[] = {
    {"audio-pass", "disk", "boot:fixture reboot:fixture",
     "audio-first audio-second audio-distinct audio-host", NULL, 0, STEP_US, 0, 1, 0, 0, NULL, 0,
     NULL, 0},
    {"audio-keep", "disk", "boot:fixture reboot:fixture",
     "audio-first audio-second audio-distinct audio-host", NULL, 0, STEP_US, 1, 0, 1, 0, NULL, 0,
     "fixture.1.wav=recorded samples;fixture.2.wav=recorded samples", 0},
};

static const struct row capture_rows[] = {
    {"capture-pass", "disk", "boot:fixture", "capture-file capture-host", NULL, 0, STEP_US, 0, 1, 0,
     0, NULL, 0, NULL, 0},
    {"capture-keep", "disk", "boot:fixture", "capture-file capture-host", NULL, 0, STEP_US, 1, 0, 1,
     0, NULL, 0, "fixture.1.net1.pcap=captured frames", 0},
};

static const struct row stand_in_rows[] = {
    {"reboot", "disk", "write:fixture check readback:fixture",
     "case-id qmp-status qmp-reply qmp-error stopped shutdown find-said find-past find-at "
     "find-absent host-ran host-sees same-copy readback second-stop",
     "disk", 0, STEP_US, 0, 1, 0, 0, NULL, 0,
     "fixture.1.console=fake: wrote;fixture.2.console=fake: disk says", 0},
    {"network", "network", "write:fixture check readback:fixture",
     "case-id qmp-status qmp-reply qmp-error stopped shutdown find-said find-past find-at "
     "find-absent host-ran host-sees same-copy readback second-stop",
     "disk", 0, STEP_US, 0, 1, 0, 0, NULL, 0, NULL, 0},
    {"usb", "usb", "write:fixture check readback:fixture",
     "case-id qmp-status qmp-reply qmp-error stopped shutdown find-said find-past find-at "
     "find-absent host-ran host-sees same-copy readback second-stop",
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
    {"detail", "disk", "host", "padded cut", NULL, 0, STEP_US, 0, 1, 1, 0, NULL, 0, NULL, 0},
    {"misuse", "disk", "host boot:fixture",
     "no-step no-artifact no-copy host-step other-machine no-tool step-back machine-step "
     "other-disk running no-qmp no-capture no-text",
     "disk", 0, STEP_US, 0, 1, 1, 0, NULL, 0, NULL, 0},
    {"wrong", "wrong", "boot:fixture", "refused damaged", "disk", 0, STEP_US, 0, 1, 0, 0, NULL, 0,
     NULL, 0},
    {"memory", "disk", "boot:fixture", "refused memory-profile", "disk", 0, STEP_US, 0, 1, 1, 0,
     NULL, 0, NULL, 0},
    {"memory-pair", "disk", "boot:fixture other:other", "refused memory-pair other-refused", NULL,
     0, STEP_US, 0, 1, 1, 0, NULL, 0, NULL, 0},
    {"memory-share", "disk", "boot:fixture", "admitted", NULL, 0, STEP_US, 0, 1, 0, 0, NULL, 0,
     NULL, 0},
    {"memory-qmp", "disk", "boot:fixture",
     "admitted qmp-query qmp-info qmp-refused hmp-refused "
     "type-refused type-probe type-again",
     NULL, 0, STEP_US, 0, 1, 1, 0, NULL, 0, NULL, 0},
    {"guest", "disk", "run:fixture", "guest-pass guest-exit guest-ran guest-lost guest-lost-end",
     NULL, 1, LOSS_US, 0, 1, 1, 0, NULL, 0, NULL, 0},
    {"media", "media", "boot:fixture again:fixture",
     "sparse drives stopped same-disk drives-again second-stop", NULL, 0, STEP_US, 0, 1, 0, 0, NULL,
     0, NULL, 0},
};

/* The fault row, whose scenario's refusals keep its scratch. */
static const struct row fault_row[] = {
    {"fault", "disk", "panic:fixture halt:fixture",
     "early-panic early-halt fault-panic fault-once fault-ended not-reset half-halted "
     "halt-no-panic fault-halt",
     NULL, 0, STEP_US, 0, 1, 1, 0, NULL, 0, NULL, 0},
};

/* The stop row, which keeps its copy and gives no end record. */
static const struct row stop_row[] = {
    {"stop", "disk", "wait:fixture after:fixture", "stopped still-stopped", "disk", 0, STEP_US, 1,
     0, 1, 0, NULL, 0, NULL, 0},
};

/* The suspension rows: thirty cycles, a stop signal and a kill during a
 * stop, and a step's bound that ends a stop. */
static const struct row suspend_rows[] = {
    {"suspend", "tick", "stall:fixture", "suspended stopped shutdown held no-run resumed", NULL, 0,
     STALL_STEP_US, 0, 1, 0, 0, NULL, 0, NULL, 0},
    {"suspend-stop", "tick", "stall:fixture", "suspend-stopped", NULL, 0, STALL_STEP_US, 1, 0, 1, 0,
     NULL, 0, "fixture.1.capture=query-status: {\"return\": {\"status\": \"running\"", SIGTERM},
    {"suspend-bound", "tick", "stall:fixture after:fixture",
     "suspend-bounded answers stopped shutdown", NULL, 0, LOSS_US, 0, 1, 0, 0, "stall",
     STALL_BOUND_US, NULL, 0},
    {"suspend-kill", "tick", "stall:fixture", "suspend-stopped", NULL, 0, STALL_STEP_US, -1, 0, 0,
     0, NULL, 0, NULL, SIGKILL},
};

/* What stops or kills a suspension row during its first stop. */
#define STALLED "@etude 1 progress stalls 1\n"

/* The damaged row, run on a store whose kernel is damaged. */
static const struct row damaged_row[] = {
    {"damaged", "disk", "boot:fixture", "refused damaged", "disk", 0, STEP_US, 0, 1, 1, 0, NULL, 0,
     NULL, 0},
};

/* The real rows. */
static const struct row real_rows[] = {
    {"real-reboot", NULL, "write:fixture check readback:fixture",
     "on-ahci stopped shutdown host-ran host-sees same-copy readback second-stop", "disk", 0,
     REAL_US, 0, 1, 0, 0, NULL, 0, NULL, 0},
    {"real-overlay", NULL, "write:fixture check readback:fixture",
     "stopped shutdown map-ran map-written converted raw-has same-overlay readback second-stop",
     "over", 0, REAL_US, 0, 1, 0, 0, NULL, 0, NULL, 0},
    {"real-guest", NULL, "run:fixture", "guest-pass guest-exit", NULL, 1, REAL_US, 0, 1, 0, 0, NULL,
     0, NULL, 0},
    {"real-peer", NULL, "serve alpha:alpha beta:beta",
     "service-ready alpha-tcp beta-tcp alpha-captured both-closed service-stopped", NULL, 0,
     REAL_US, 0, 1, 0, 1, NULL, 0, NULL, 0},
    {"real-media", NULL, "boot:fixture", "sparse drives stopped", NULL, 0, REAL_US, 0, 1, 0, 0,
     NULL, 0, NULL, 0},
    {"real-suspend", NULL, "stall:fixture", "suspended stopped shutdown held", "disk", 0, REAL_US,
     0, 1, 0, 0, NULL, 0, NULL, 0},
};

/* The forward rows, under the stand-in and under the real QEMU. */
static const struct row forward_rows[] = {
    {"forward", "disk", "serve boot:fixture other:other",
     "booted forwarded second-card unheld-refused unheld-unstarted", NULL, 0, STEP_US, 0, 1, 1, 0,
     NULL, 0, NULL, 0},
    {"real-forward", NULL, "serve boot:fixture relay closed",
     "service-ready booted shell tcp-forward udp-forward guest-forward relay-closed restricted "
     "service-stopped",
     NULL, 0, REAL_US, 0, 1, 0, 0, "closed", CLOSED_US, NULL, 0},
};

/**
 * Run the suspension rows: thirty cycles judged by the stand-in's ticks; a
 * stop signal during a stop, after which the machine answers QMP in the
 * scenario's cleanup; a step's bound during a stop; and a kill of the
 * scenario during a stop, after which the stopped stand-in must take the
 * SIGTERM of its keeper, which writes the row's marker, and end.
 */
static void test_suspend(const struct site *s)
{
    struct row_run killed;
#ifdef _WIN32
    long pids[ROW_PROCS];
    HANDLE processes[ROW_PROCS];
    DWORD status;
    int count = 0, i, ended_count = 0, killed_scenario = 0;
#else
    char ended[PATH_MAX];
#endif

    run_row(&suspend_rows[0], s, NULL);
    run_row(&suspend_rows[1], s, STALLED);
    run_row(&suspend_rows[2], s, NULL);
    row_start(&killed, &suspend_rows[3], s);
#ifdef _WIN32
    if (killed.started && attempt_read(&killed.a, STALLED, NULL)) {
        count = row_procs("suspend-kill", pids);
        for (i = 0; i < count; i++)
            processes[i] =
                OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pids[i]);
    }
#endif
    row_finish(&killed, STALLED, 0);
    CHECK(killed.a.stopped, "suspend-kill: killed during its stop");
    row_gone("suspend-kill");
#ifdef _WIN32
    for (i = 0; i < count; i++) {
        if (processes[i] != NULL) {
            if (WaitForSingleObject(processes[i], INFINITE) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(processes[i], &status) && status != STILL_ACTIVE) {
                ended_count++;
                if (pids[i] == killed.a.pid)
                    killed_scenario = status == ETUDE_WIN_STOP_STATUS;
            }
            CloseHandle(processes[i]);
        }
    }
    CHECK(count >= 2 && ended_count == count && killed_scenario,
          "suspend-kill: the scenario was killed and every stopped QEMU ended");
#else
    fix_format(ended, sizeof(ended), "%s/suspend-kill.ended", fix_dir());
    CHECK(file_has(ended, "fake: terminated"), "suspend-kill: the stopped QEMU took its SIGTERM");
#endif
}

/** Run the audio rows, the capture rows, the stand-in rows, the fault row,
 * the stop row, the suspension rows, the pair and isolation rows, the forward
 * row and the damaged row. */
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
    for (i = 0; i < sizeof(audio_rows) / sizeof(audio_rows[0]); i++)
        run_row(&audio_rows[i], &s, NULL);
    for (i = 0; i < sizeof(capture_rows) / sizeof(capture_rows[0]); i++)
        run_row(&capture_rows[i], &s, NULL);
    for (i = 0; i < sizeof(stand_in_rows) / sizeof(stand_in_rows[0]); i++)
        run_row(&stand_in_rows[i], &s, NULL);
    run_row(fault_row, &s, NULL);
    run_row(stop_row, &s, WAITING);
    test_suspend(&s);
    run_row(pair_row, &s, NULL);
    test_isolation(&s);
    run_row(&forward_rows[0], &s, NULL);
    CHECK(etude_store_check(s.store, s.disk, NULL) == 0, "the disk artifact is unchanged");
    CHECK(damage(s.store, s.kernel) == 0, "damaged");
    run_row(damaged_row, &s, NULL);
    CHECK(!machines_left(), "every stand-in QEMU and etude-guest has ended");
    etude_store_close(s.store);
}

/** Run the real rows and the real forward row. */
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
    run_row(&forward_rows[1], &s, NULL);
    CHECK(etude_store_check(s.store, s.disk, NULL) == 0, "the real disk artifact is unchanged");
    etude_store_close(s.store);
}

/**
 * Run the test, or this program as the host tool or the stand-in for QEMU,
 * and exit; with one argument, the invocation, enter the library's ordinary
 * scenario entry point. Windows constructors do not receive argc and argv.
 */
int main(int argc, char **argv)
{
    const char *fake = getenv("ETUDE_FAKE_QEMU");

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (host_executable(argv[0], self, sizeof(self)) != 0 || sh_arm_stop(argc == 2) != 0)
        return 1;
#ifdef _WIN32
    if (argc == 4 && strcmp(argv[1], "--scenario-guest") == 0)
        return scenario_guest_actor(argv[2]);
#endif
    for (char *path = self; *path; path++)
        if (*path == '\\')
            *path = '/';
    if (argc == 2)
        return etude_case_main(argc, argv);
    if (argc >= 3 && strcmp(argv[1], "tool") == 0)
        exit(tool(argc, argv));
    if (fake != NULL && *fake != '\0')
        exit(fake_qemu(fake, argc, argv));
    if (host_executable(argv[0], self, sizeof(self)) != 0) {
        printf("etude-scenario-test: fail: %s cannot be found\n", argv[0]);
        exit(1);
    }
    if (setenv("TMPDIR", fix_dir(), 1) != 0)
        exit(1);
    test_stand_in();
    test_real();
    if (fails != 0)
        exit(1);
    fix_passed();
    printf("etude_scenario_hosttest: pass\n");
    exit(0);
}
