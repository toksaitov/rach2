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
 * host-deps: tools/etude/guest/guest.c tools/etude/guest/wire.c
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
 * end. With the real QEMU, a disk root written at the debug shell and
 * powered off is read back by the next boot, and etude-guest runs the test
 * image's /hello.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

/**
 * Be the tool `argv[2]`: `scan FILE TEXT` exits 0 when FILE has TEXT;
 * `report CASE` reports one assertion of CASE as a guest program does; and
 * `killguest FILE` kills the etude-guest whose PID FILE has.
 */
static int tool(int argc, char **argv)
{
    char line[256];
    FILE *f;
    long pid = 0;

    if (argc == 5 && strcmp(argv[2], "scan") == 0)
        return file_has(argv[3], argv[4]) ? 0 : 1;
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
 * Be QEMU, as ETUDE_FAKE_QEMU's `mode` says: answer QMP's start, say that
 * the machine runs and the shell is ready, and act on what the console
 * types: `write TEXT` and `read` the first disk, `poweroff`, `die`,
 * `closecon`, `closeqmp` and `exec /etude-guest`.
 */
static int fake_qemu(const char *mode, int argc, char **argv)
{
    const char *accel = fake_arg(argc, argv, "-accel", "");
    const char *disk = fake_arg(argc, argv, "-drive", "file=");
    char path[PATH_MAX], text[600], read_back[128];
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
        if (strncmp(con.line, "write ", 6) == 0 && disk != NULL) {
            fake_disk(disk, con.line + 6, NULL, 0);
            put(con.fd, "fake: wrote\r\n> ");
        } else if (strcmp(con.line, "read") == 0 && disk != NULL) {
            fake_disk(disk, NULL, read_back, sizeof(read_back));
            snprintf(text, sizeof(text), "fake: disk says %s\r\n> ", read_back);
            put(con.fd, text);
        } else if (strcmp(con.line, "poweroff") == 0) {
            put(con.fd, "poweroff: powering off\r\n");
            if (strcmp(mode, "lost") == 0)
                _exit(0);
            fake_shutdown(qmp.fd, 1, "guest-shutdown");
            _exit(0);
        } else if (strcmp(con.line, "die") == 0) {
            put(con.fd, "fake: dying\r\n");
            kill(getpid(), SIGKILL);
        } else if (strcmp(con.line, "closecon") == 0) {
            close(con.fd);
            con.fd = -1;
        } else if (strcmp(con.line, "closeqmp") == 0) {
            put(con.fd, "fake: no qmp\r\n> ");
            close(qmp.fd);
            qmp.fd = -1;
        } else if (strncmp(con.line, "exec /etude-guest", 17) == 0) {
            fake_guest(con.fd, con.line);
            close(con.fd);
            con.fd = -1;
        }
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
    return 1;
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
};

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

/* Where a row runs QEMU, its store and the digests of its artifacts. */
struct site {
    struct etude_store *store;
    char store_dir[PATH_MAX];
    char qemu_i686[PATH_MAX], qemu_x86_64[PATH_MAX];
    const char *arch;
    char kernel[65], disk[65], initrd[65], scanner[65], inv[65], killer[65];
};

/** Write row `r`'s invocation to `path`, with the scratch `scratch`. */
static int write_scenario(const char *path, const struct row *r, const struct site *s,
                          const char *manifest, const char *scratch)
{
    char steps[1024] = "", one[160], word[64], text[8192];
    const char *p = r->steps;
    size_t n;

    while (*p != '\0') {
        n = strcspn(p, " :");
        snprintf(word, sizeof(word), "%.*s", (int)n, p);
        p += strcspn(p, " ");
        p += *p == ' ';
        fix_format(one, sizeof(one), "%s\"%s\": %llu", steps[0] != '\0' ? ", " : "", word,
                   (unsigned long long)r->bound_us);
        strcat(steps, one);
    }
    fix_format(text, sizeof(text),
               "{\"schema_version\": 1, \"run_id\": \"test-run\", \"attempt_id\": \"test-%s\", "
               "\"manifest\": \"%s\", \"case_id\": \"test.scenario.%s\", \"architecture\": "
               "\"%s\", \"store\": \"%s\", \"artifacts\": {\"kernel\": \"%s\", \"disk\": \"%s\", "
               "\"initrd\": \"%s\", \"scanner\": \"%s\", \"invocation\": \"%s\", \"killer\": "
               "\"%s\"}, \"tools\": {}, \"site\": {\"accel\": \"tcg\", \"qemu_i686\": \"%s\", "
               "\"qemu_x86_64\": \"%s\", \"ovmf_i686\": null, \"ovmf_x86_64\": null}, "
               "\"steps\": {%s}, \"cleanup_us\": 30000000, \"scratch\": \"%s\", \"grace_us\": "
               "%u, \"console_max_bytes\": 16777216, \"output_max_bytes\": 1048576, "
               "\"resend_us\": 200000}",
               r->name, manifest, r->name, s->arch, s->store_dir, s->kernel, s->disk, s->initrd,
               s->scanner, s->inv, s->killer, s->qemu_i686, s->qemu_x86_64, steps, scratch,
               r->mode != NULL ? 300000u : 5000000u);
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

/**
 * Run the scenario of row `r` with invocation `inv` and the environment
 * `env`, read its output, and, when `stop_at` is not NULL, send it SIGTERM
 * once its output has `stop_at`. Past the completion bound it is killed.
 */
static void attempt(const struct row *r, const char *inv, char **env, const char *stop_at,
                    struct outcome *o)
{
    char err[PATH_MAX], *argv[] = {self, (char *)inv, NULL};
    int limit = r->mode != NULL ? FAKE_ATTEMPT_MS : REAL_ATTEMPT_MS;
    uint64_t bound = now_ms() + (uint64_t)limit;
    int out[2], fd, st, stopped = 0;
    struct pollfd p;
    ssize_t n;
    pid_t pid;
    FILE *f;

    memset(o, 0, sizeof(*o));
    o->status = -1;
    fix_format(err, sizeof(err), "%s/%s.err", fix_dir(), r->name);
    if (pipe(out) != 0)
        return;
    pid = fork();
    if (pid == 0) {
        fd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(out[1], 1);
        dup2(fd, 2);
        close(out[0]);
        close(out[1]);
        close(fd);
        execve(self, argv, env);
        _exit(127);
    }
    close(out[1]);
    for (;;) {
        p.fd = out[0];
        p.events = POLLIN;
        if (now_ms() > bound) {
            printf("etude-scenario-test: fail: %s: the attempt did not end within %d ms\n", r->name,
                   limit);
            fails++;
            kill(pid, SIGKILL);
            break;
        }
        if (poll(&p, 1, 1000) <= 0)
            continue;
        n = read(out[0], o->out + o->len, sizeof(o->out) - 1 - o->len);
        if (n <= 0)
            break;
        o->len += (size_t)n;
        o->out[o->len] = '\0';
        if (stop_at != NULL && !stopped && strstr(o->out, stop_at) != NULL) {
            stopped = 1;
            kill(pid, SIGTERM);
        }
    }
    close(out[0]);
    if (waitpid(pid, &st, 0) == pid && WIFEXITED(st))
        o->status = WEXITSTATUS(st);
    f = fopen(err, "r");
    if (f != NULL) {
        char line[512];

        while (fgets(line, sizeof(line), f) != NULL) {
            o->kept |= strncmp(line, "etude-case: kept: ", 18) == 0;
            if (fails > 0 || getenv("ETUDE_TEST_VERBOSE") != NULL)
                printf("    %s: stderr: %s", r->name, line);
        }
        fclose(f);
    }
}

/** Say whether the output `o` has the record `rec`, a line of its own. */
static int has_record(const struct outcome *o, const char *rec)
{
    char line[300];

    snprintf(line, sizeof(line), "@etude 1 %s\n", rec);
    return strstr(o->out, line) != NULL;
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
    if (fails > 0 || getenv("ETUDE_TEST_VERBOSE") != NULL)
        printf("    %s: stdout:\n%s", r->name, o->out);
}

/** Make the environment of row `r`'s scenario: this program's, with the
 * row, the nonce, the stand-in's mode and its files. */
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
    snprintf(bufs[5], PATH_MAX, "ETUDE_TEST_UNUSED=1");
    for (i = 0; i < 6; i++)
        env[n + i] = bufs[i];
    return env;
}

/** Run row `r` on site `s`, and judge it; `stop_at` as attempt() takes it. */
static void run_row(const struct row *r, const struct site *s, const char *stop_at)
{
    char manifest[PATH_MAX], inv[PATH_MAX], scratch[PATH_MAX], *bufs[6];
    char **env = row_env(r, bufs);
    struct outcome *o = malloc(sizeof(*o));
    int i;

    if (o == NULL)
        exit(1);
    fix_format(manifest, sizeof(manifest), "%s/%s.manifest.json", fix_dir(), r->name);
    fix_format(inv, sizeof(inv), "%s/%s.invocation.json", fix_dir(), r->name);
    fix_format(scratch, sizeof(scratch), "%s/%s.scratch", fix_dir(), r->name);
    if (mkdir(scratch, 0700) != 0 || write_manifest(manifest, r) != 0 ||
        write_scenario(inv, r, s, manifest, scratch) != 0) {
        printf("etude-scenario-test: fail: %s: its files cannot be written\n", r->name);
        fails++;
    } else {
        attempt(r, inv, env, stop_at, o);
        judge(r, o, scratch);
    }
    for (i = 0; i < 6; i++)
        free(bufs[i]);
    free(env);
    free(o);
}

/** Say whether a process of the stand-ins' or etude-guest's is left. */
static int machines_left(void)
{
    char cmd[PATH_MAX + 64];

    fix_format(cmd, sizeof(cmd),
               "ps -eo args | grep -F -e '%s -accel' | grep -v -e grep >/dev/null", self);
    return system(cmd) == 0;
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
           stored(s->store, inv, s->inv) || stored(s->store, kill_inv, s->killer);
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
    return stored(s->store, kernel, s->kernel) || stored(s->store, disk, s->disk) ||
           stored(s->store, initrd, s->initrd) || stored(s->store, self, s->scanner) ||
           stored(s->store, inv, s->inv) || stored(s->store, inv, s->killer);
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

/* The stand-in rows: name, mode, steps, assertions, disk, helper, step
 * bound, exit status, end record, kept scratch. */
static const struct row stand_in_rows[] = {
    {"reboot", "disk", "write:fixture check readback:fixture",
     "qmp-status qmp-reply qmp-error stopped shutdown host-ran host-sees same-copy readback "
     "second-stop",
     "disk", 0, STEP_US, 0, 1, 0},
    {"lost", "lost", "lose:fixture", "lost-wait ended not-shutdown", "disk", 0, LOSS_US, 0, 1, 0},
    {"die", "die", "lose:fixture", "lost-wait ended not-shutdown", "disk", 0, LOSS_US, 0, 1, 0},
    {"closecon", "disk", "lose:fixture", "lost-wait not-shutdown", "disk", 0, LOSS_US, 0, 1, 0},
    {"closeqmp", "disk", "lose:fixture", "lost-wait not-shutdown", "disk", 0, LOSS_US, 0, 1, 0},
    {"bound", "disk", "wait:fixture", "bounded after-bound", "disk", 0, SHORT_US, 0, 1, 0},
    {"misuse", "disk", "host boot:fixture",
     "no-step no-artifact no-copy host-step other-machine no-tool step-back machine-step "
     "other-disk running no-qmp",
     "disk", 0, STEP_US, 0, 1, 1},
    {"wrong", "wrong", "boot:fixture", "refused damaged", "disk", 0, STEP_US, 0, 1, 0},
    {"guest", "disk", "run:fixture", "guest-pass guest-exit guest-ran guest-lost guest-lost-end",
     NULL, 1, LOSS_US, 0, 1, 1},
};

/* The stop row, which keeps its copy and gives no end record. */
static const struct row stop_row = {
    "stop", "disk", "wait:fixture after:fixture", "stopped still-stopped", "disk", 0, STEP_US, 1,
    0,      1};

/* The damaged row, run on a store whose kernel is damaged. */
static const struct row damaged_row = {
    "damaged", "disk", "boot:fixture", "refused damaged", "disk", 0, STEP_US, 0, 1, 1};

/* The real rows. */
static const struct row real_rows[] = {
    {"real-reboot", NULL, "write:fixture check readback:fixture",
     "stopped shutdown host-ran host-sees same-copy readback second-stop", "disk", 0, REAL_US, 0, 1,
     0},
    {"real-guest", NULL, "run:fixture", "guest-pass guest-exit", NULL, 1, REAL_US, 0, 1, 0},
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
    run_row(&stop_row, &s, "@etude 1 progress waiting 0\n");
    CHECK(etude_store_check(s.store, s.disk, NULL) == 0, "the disk artifact is unchanged");
    CHECK(damage(s.store, s.kernel) == 0, "damaged");
    run_row(&damaged_row, &s, NULL);
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
