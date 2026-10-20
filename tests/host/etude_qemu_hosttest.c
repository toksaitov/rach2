/**
 * Check etude's QEMU launcher (tools/etude/qemu_profile.c and
 * tools/etude/posix/qemu.c), with this program as a stand-in for QEMU when
 * ETUDE_FAKE_QEMU is set, and with the real QEMU on the kernel and test
 * image of the architecture ARCH names.
 *
 * host-deps: tools/etude/capture.c tools/etude/history_json.c
 * host-deps: tools/etude/qemu_profile.c tools/etude/posix/exec.c
 * host-deps: tools/etude/posix/keeper.c tools/etude/posix/process.c
 * host-deps: tools/etude/posix/qemu.c tools/etude/processor.c
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 * host-seconds: 240
 * host-alone: a stand-in QEMU must quit within a grace of 300 ms
 *
 * The command lines of profiles, and the profiles the launcher refuses, are
 * checked as text. Thirteen stand-in machines then run at once, each with an
 * end of its own, and each must keep its own console and end: a QEMU that fails
 * before it connects is an unexpected exit, and one that reports that it could
 * not start the accelerator is a failed accelerator; so is one whose QMP
 * reports another accelerator in use, through query-accelerators or, for a QEMU
 * without that command, query-kvm. QMP's SHUTDOWN events of the guest give a
 * shutdown, a reset and a panic; one of the host, or an exit without an event,
 * is an unexpected exit; a cancellation of a machine that obeys QMP's quit, of
 * one that ignores both quit and SIGTERM, and of one that never answers QMP is
 * a cancellation. Each leaves no process and no socket behind. With the real
 * QEMU, the kernel of the architecture ARCH names boots under TCG to the debug
 * shell, whose `poweroff` must give a shutdown; a cancellation at the prompt
 * must give a cancellation, and an accelerator this host's QEMU does not have
 * must give a failed accelerator.
 *
 * The memory rows check the reservations of an ordinary profile and of the
 * boundary profile, refuse a changed boundary profile and a site that has not
 * enabled it, and refuse a boundary start one byte short of its reservation
 * before anything runs. With `--boundary` and ARCH=x86_64 the program runs
 * only the 72 GiB boundary boot in the real QEMU, which must report the
 * counts bigmem.sh requires and keep QEMU's resident peak within the
 * reservation.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include "compat/pathmax_host.h"
#include "compat/fixdir_host.c"
#include "etude/qemu.h"

extern char **environ;

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                        \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            printf("etude-qemu-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                            \
        }                                                                                       \
    } while (0)

/* The completion bound of each group of machines. */
#define BOUND_MS 120000

/* The grace periods: short for the stand-ins, longer for a real QEMU. */
#define FAKE_GRACE_US 300000u
#define REAL_GRACE_US 5000000u

/* This program, by an absolute path. */
static char self[PATH_MAX];

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

/** Read one line of QMP into `line`, without its newline; -1 at its end. */
static int fake_line(int fd, char *line, size_t size)
{
    size_t n = 0;
    char c;

    for (;;) {
        if (read(fd, &c, 1) != 1)
            return -1;
        if (c == '\n')
            break;
        if (n + 1 < size)
            line[n++] = c;
    }
    line[n] = '\0';
    return 0;
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

/** Send QMP's event SHUTDOWN, of the guest or not, for `reason`. */
static void fake_shutdown(int qmp, int guest, const char *reason)
{
    char text[256];

    snprintf(text, sizeof(text),
             "{\"timestamp\": {\"seconds\": 1, \"microseconds\": 2}, \"event\": \"SHUTDOWN\", "
             "\"data\": {\"guest\": %s, \"reason\": \"%s\"}}\r\n",
             guest ? "true" : "false", reason);
    put(qmp, text);
}

/** Answer QMP until it asks to quit, then report the quit and exit 0. */
static void fake_obey(int qmp)
{
    char line[512], name[64], text[128];
    long id;

    while (fake_line(qmp, line, sizeof(line)) == 0) {
        id = fake_command(line, name, sizeof(name));
        snprintf(text, sizeof(text), "{\"return\": {}, \"id\": %ld}\r\n", id);
        put(qmp, text);
        if (strcmp(name, "quit") == 0) {
            fake_shutdown(qmp, 0, "host-qmp-quit");
            _exit(0);
        }
    }
    _exit(0);
}

/**
 * Answer QMP as QEMU starts a machine, until its cont: `mode` "old" lacks
 * query-accelerators, and "wrong" reports another accelerator than `accel`
 * in use.
 */
static void fake_start(int qmp, const char *mode, const char *accel)
{
    const char *other = strcmp(accel, "tcg") == 0 ? "kvm" : "tcg";
    char line[512], name[64], text[256];
    long id;

    put(qmp, "{\"QMP\": {\"version\": {\"qemu\": {\"micro\": 0, \"minor\": 0, \"major\": 9}, "
             "\"package\": \"\"}, \"capabilities\": [\"oob\"]}}\r\n");
    while (fake_line(qmp, line, sizeof(line)) == 0) {
        id = fake_command(line, name, sizeof(name));
        if (strcmp(name, "query-accelerators") == 0 && strncmp(mode, "old", 3) == 0)
            snprintf(text, sizeof(text),
                     "{\"id\": %ld, \"error\": {\"class\": \"CommandNotFound\", "
                     "\"desc\": \"The command query-accelerators has not been found\"}}\r\n",
                     id);
        else if (strcmp(name, "query-accelerators") == 0)
            snprintf(text, sizeof(text),
                     "{\"return\": {\"enabled\": \"%s\", \"present\": [\"kvm\", \"tcg\"]}, "
                     "\"id\": %ld}\r\n",
                     strcmp(mode, "wrong") == 0 ? other : accel, id);
        else if (strcmp(name, "query-kvm") == 0)
            snprintf(text, sizeof(text),
                     "{\"return\": {\"enabled\": %s, \"present\": true}, \"id\": %ld}\r\n",
                     strcmp(mode, "old-wrong") == 0 ? "true" : "false", id);
        else
            snprintf(text, sizeof(text), "{\"return\": {}, \"id\": %ld}\r\n", id);
        put(qmp, text);
        if (strcmp(name, "quit") == 0) {
            fake_shutdown(qmp, 0, "host-qmp-quit");
            _exit(0);
        }
        if (strcmp(name, "cont") == 0)
            return;
    }
    _exit(122);
}

/** Read the console until the launcher has typed `go` and a return. */
static void fake_wait_go(int con)
{
    char seen[4] = "";
    char c;

    while (strcmp(seen, "go\r") != 0) {
        if (read(con, &c, 1) != 1)
            _exit(123);
        memmove(seen, seen + 1, 2);
        seen[2] = c;
        seen[3] = '\0';
    }
}

/** Find the path of the `-chardev` socket of id `id` in QEMU's arguments. */
static const char *fake_socket(int argc, char **argv, const char *id)
{
    char head[64];
    int i;

    snprintf(head, sizeof(head), "socket,id=%s,path=", id);
    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "-chardev") == 0 && strncmp(argv[i + 1], head, strlen(head)) == 0)
            return argv[i + 1] + strlen(head);
    _exit(124);
}

/**
 * Be QEMU, as ETUDE_FAKE_QEMU's `mode` says: connect to the console and
 * QMP, answer QMP's start, write to the console, and end.
 */
static int fake_qemu(const char *mode, int argc, char **argv)
{
    const char *accel = NULL, *reason = NULL;
    int i, con, qmp, guest = 1, stopped = 0;
    const char *audio = NULL;
    FILE *wav;

    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-accel") == 0)
            accel = argv[i + 1];
        stopped |= strcmp(argv[i], "-S") == 0;
        if (strcmp(argv[i], "-audiodev") == 0)
            audio = strstr(argv[i + 1], "path=");
    }
    if (accel == NULL || !stopped)
        return 125;
    if (strcmp(mode, "early") == 0) {
        fprintf(stderr, "%s: -cpu bogus: unable to find CPU model 'bogus'\n", argv[0]);
        return 1;
    }
    con = fake_connect(fake_socket(argc, argv, "etude-console"));
    qmp = fake_connect(fake_socket(argc, argv, "etude-qmp"));
    if (strcmp(mode, "refuse") == 0) {
        fprintf(stderr, "%s: -accel %s: failed to initialize %s: No such device\n", argv[0], accel,
                accel);
        return 1;
    }
    if (strcmp(mode, "silent") == 0)
        for (;;)
            pause();
    fake_start(qmp, mode, accel);
    if (audio != NULL) {
        wav = fopen(audio + 5, "wb");
        if (wav == NULL)
            return 127;
        fputs("recorded samples", wav);
        fclose(wav);
    }
    put(con, "fake: running\r\n");
    if (strcmp(mode, "crash") == 0) {
        put(con, "fake: crashing\r\n");
        return 3;
    }
    if (strcmp(mode, "obey") == 0)
        fake_obey(qmp);
    if (strcmp(mode, "deaf") == 0) {
        signal(SIGTERM, SIG_IGN);
        put(con, "fake: deaf\r\n");
        for (;;)
            pause();
    }
    if (strcmp(mode, "shutdown") == 0 || strcmp(mode, "old") == 0)
        reason = "guest-shutdown";
    else if (strcmp(mode, "reset") == 0)
        reason = "guest-reset";
    else if (strcmp(mode, "panic") == 0)
        reason = "guest-panic";
    else if (strcmp(mode, "host") == 0) {
        reason = "host-signal";
        guest = 0;
    } else {
        return 126;
    }
    fake_wait_go(con);
    put(con, "fake: got go\r\n");
    fake_shutdown(qmp, guest, reason);
    return 0;
}

/* ---- The controller. ---- */

/** Give the offset just past `text` in what `o` kept, or -1 when absent. */
static long find(const struct etude_output *o, const char *text, size_t from)
{
    size_t len = strlen(text), i;

    for (i = from; i + len <= o->kept; i++)
        if (memcmp(o->data + i, text, len) == 0)
            return (long)(i + len);
    return -1;
}

/* One machine of a group, and what the test does with it. */
struct row {
    const char *name;
    const char *mode; /* the stand-in's, or NULL for a real QEMU */
    enum etude_accel accel;
    const char *arch;
    enum etude_end want;
    int act; /* 'g' types go at the start, 'c' cancels when
                running, 'C' cancels at once, 'p' types poweroff
                at the prompt, 'P' cancels at the prompt, 'b' runs the boundary probe */
    int acted;
    struct etude_qemu_profile profile;
    struct etude_qemu_site site;
    struct etude_machine *m;
    char env[64];
    char **envp;
    char console[64];
    char kernel[PATH_MAX], initrd[PATH_MAX];
};

/* A row's initializer: its name, mode, accelerator, architecture, wanted end
 * and action, with the rest empty. */
#define ROW(name, mode, accel, arch, want, act) \
    {name, mode, accel, arch, want, act, 0, {0}, {0}, NULL, "", NULL, "", "", ""}

/** Act on a row whose machine has changed, as its `act` says. */
static void row_act(struct row *r)
{
    const struct etude_machine_result *res = etude_machine_result(r->m);
    long shell;

    if (r->act == 'a' && r->acted == 1 && find(&res->console, "Done.", 0) >= 0) {
        r->acted = 2;
        CHECK(etude_machine_type(r->m, "poweroff\r", 9) == 0, r->name);
    }
    if (r->act == 'b' && r->acted == 1 && find(&res->console, "MEMPROBE MemTotal ", 0) >= 0) {
        r->acted = 2;
        etude_machine_cancel(r->m);
    }
    if (r->acted || res->state == ETUDE_MACHINE_ENDED)
        return;
    if (r->act == 'c' && res->state == ETUDE_MACHINE_RUNNING &&
        (find(&res->console, "fake: deaf", 0) >= 0 || strcmp(r->mode, "obey") == 0)) {
        r->acted = 1;
        etude_machine_cancel(r->m);
    }
    if (r->act != 'p' && r->act != 'P' && r->act != 'b' && r->act != 'a')
        return;
    shell = find(&res->console, "Kernel debug shell active", 0);
    if (shell < 0 || find(&res->console, "\n> ", (size_t)shell) < 0)
        return;
    r->acted = 1;
    if (r->act == 'a') {
        static const char scale[] = "audio test scale 200\r";

        CHECK(etude_machine_type(r->m, scale, sizeof(scale) - 1) == 0, r->name);
    } else if (r->act == 'b') {
        CHECK(etude_machine_type(r->m, "exec /memprobe 5120\r", 19) != 0 && errno == EPERM,
              "boundary refuses an allocating workload");
        CHECK(etude_machine_type(r->m, "exec /memprobe total\r", 21) == 0, r->name);
    } else if (r->act == 'p')
        CHECK(etude_machine_type(r->m, "poweroff\r", 9) == 0, r->name);
    else
        etude_machine_cancel(r->m);
}

/**
 * Fill `spec` for the machine of row `r`, with the site's programs `q32` and
 * `q64`: QEMU's environment is this program's with the stand-in's mode.
 */
static void row_spec(struct row *r, struct etude_machine_spec *spec, const char *q32,
                     const char *q64, uint64_t grace)
{
    size_t n = 0;

    r->site.accel = r->accel;
    r->site.qemu_i686 = q32;
    r->site.qemu_x86_64 = q64;
    r->profile.architecture = r->arch;
    r->profile.cpu = "max";
    r->profile.processors = 1;
    r->profile.memory_bytes = 128u << 20;
    r->profile.firmware = ETUDE_FIRMWARE_DIRECT;
    if (r->profile.kernel == NULL)
        r->profile.kernel = "kernel";
    while (environ[n] != NULL)
        n++;
    r->envp = calloc(n + 2, sizeof(*r->envp));
    if (r->envp == NULL)
        exit(1);
    memcpy(r->envp, environ, n * sizeof(*r->envp));
    snprintf(r->env, sizeof(r->env), "ETUDE_FAKE_QEMU=%s", r->mode != NULL ? r->mode : "");
    r->envp[n] = r->env;
    snprintf(r->console, sizeof(r->console), "console-%s", r->name);
    memset(spec, 0, sizeof(*spec));
    spec->attempt_id = r->name;
    spec->profile = &r->profile;
    spec->site = &r->site;
    spec->envp = r->envp;
    spec->console_path = fix_path(r->console);
    spec->console_max = 1u << 20;
    spec->grace_us = grace;
    if (r->act == 'b') {
        static const char *const rescue[] = {"rach2.rescue", NULL};

        r->site.boundary_lazy = 1;
        r->profile.cpu = "max-nopdpe1gb";
        r->profile.memory_profile = "bigmem-boundary-v1";
        r->profile.memory_bytes = ETUDE_BIGMEM_CONFIGURED;
        r->profile.boot_options = rescue;
        spec->memory_reserved_bytes = ETUDE_BIGMEM_RESIDENT + ETUDE_BIGMEM_OVERHEAD;
    }
}

/** Start the machine of row `r`, and type or cancel at once as it says. */
static int row_start(struct etude_qemu *q, struct row *r, const char *q32, const char *q64,
                     uint64_t grace)
{
    struct etude_machine_spec spec;

    row_spec(r, &spec, q32, q64, grace);
    if (etude_qemu_start(q, &spec, &r->m) != 0) {
        printf("etude-qemu-test: fail: %s: the start failed: %s\n", r->name, strerror(errno));
        fails++;
        r->m = NULL;
        return -1;
    }
    if (r->act == 'g')
        CHECK(etude_machine_type(r->m, "go\r", 3) == 0, r->name);
    if (r->act == 'C')
        etude_machine_cancel(r->m);
    return 0;
}

/** Read the console's file of row `r` and require it to hold what was kept. */
static void check_console_file(const struct row *r, const struct etude_machine_result *res)
{
    char buf[1 << 16];
    FILE *f = fopen(fix_path(r->console), "rb");
    size_t n = 0;

    CHECK(f != NULL, r->name);
    if (f == NULL)
        return;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (res->console.total <= sizeof(buf)) {
        CHECK(n == res->console.total, r->name);
        CHECK(memcmp(buf, res->console.data, n) == 0, r->name);
    }
}

/** Give the monotonic clock in milliseconds. */
static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/**
 * Run the `n` rows at once until every machine has ended, acting on each;
 * past the completion bound the rest are cancelled, and the row fails.
 */
static void run_rows(struct etude_qemu *q, struct row *rows, int n)
{
    uint64_t bound = now_ms() + BOUND_MS;
    int i, left = 0, late = 0;

    for (i = 0; i < n; i++)
        left += rows[i].m != NULL;
    while (left > 0) {
        if (etude_qemu_wait(q, 1000) != NULL)
            left--;
        for (i = 0; i < n; i++)
            if (rows[i].m != NULL)
                row_act(&rows[i]);
        if (!late && now_ms() > bound) {
            printf("etude-qemu-test: fail: the machines did not end within %d ms\n", BOUND_MS);
            fails++;
            late = 1;
            for (i = 0; i < n; i++)
                if (rows[i].m != NULL)
                    etude_machine_cancel(rows[i].m);
        }
    }
}

/** Require row `r`'s machine to have ended as wanted, and free it. */
static void row_check(struct row *r)
{
    static const char *const ends[] = {"exited",      "signaled",     "init_exited", "init_killed",
                                       "panic",       "reset",        "shutdown",    "halted",
                                       "qemu_exited", "accel_failed", "stopped",     "lost"};
    const struct etude_machine_result *res;

    if (r->m == NULL)
        return;
    res = etude_machine_result(r->m);
    if (res->end != r->want) {
        printf("etude-qemu-test: fail: %s: ended %s, not %s; QEMU's standard error: %s\n", r->name,
               ends[res->end], ends[r->want], res->qemu->err.data);
        fails++;
    }
    CHECK(strcmp(res->attempt_id, r->name) == 0, r->name);
    CHECK(res->state == ETUDE_MACHINE_ENDED, r->name);
    CHECK(res->qemu->cleanup == ETUDE_CLEANUP_CONFIRMED, r->name);
    CHECK(res->qmp_bad == 0, r->name);
    CHECK(res->end_console <= res->console.total, r->name);
    CHECK(res->ended_us >= res->started_us, r->name);
    CHECK(res->console_file_error == 0, r->name);
    check_console_file(r, res);
}

/** Free the machine of row `r` and its environment. */
static void row_free(struct row *r)
{
    if (r->m != NULL)
        etude_machine_free(r->m);
    r->m = NULL;
    free(r->envp);
    r->envp = NULL;
}

/** Say whether a file or directory named `prefix`... is in `dir`. */
static int left_in(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    if (d == NULL)
        return 1;
    while ((e = readdir(d)) != NULL)
        found |= strncmp(e->d_name, prefix, strlen(prefix)) == 0;
    closedir(d);
    return found;
}

/** Check what a stand-in row adds to its end: its exit, its accelerator,
 * its reason and its console. */
static void check_details(const struct row *r, const struct etude_machine_result *res)
{
    if (strcmp(r->name, "early") == 0) {
        CHECK(res->qemu->exit_end == ETUDE_END_EXITED && res->qemu->exit_value == 1, "early");
        CHECK(res->accel == NULL && res->running_us == 0, "early");
    } else if (strcmp(r->name, "refuse") == 0) {
        CHECK(res->qemu->exit_value == 1 && res->accel == NULL, "refuse");
    } else if (strcmp(r->name, "wrong") == 0) {
        CHECK(res->accel != NULL && strcmp(res->accel, "tcg") == 0, "wrong");
        CHECK(res->method != NULL && strcmp(res->method, "query-accelerators") == 0, "wrong");
        CHECK(res->running_us == 0 && res->console.total == 0, "wrong");
    } else if (strcmp(r->name, "old") == 0) {
        CHECK(res->method != NULL && strcmp(res->method, "query-kvm") == 0, "old");
        CHECK(res->accel != NULL && strcmp(res->accel, "tcg") == 0, "old");
    } else if (strcmp(r->name, "old-wrong") == 0) {
        CHECK(res->accel != NULL && strcmp(res->accel, "kvm") == 0, "old-wrong");
    } else if (strcmp(r->name, "crash") == 0) {
        CHECK(res->qemu->exit_end == ETUDE_END_EXITED && res->qemu->exit_value == 3, "crash");
        CHECK(find(&res->console, "fake: crashing\r\n", 0) >= 0, "crash");
    } else if (strcmp(r->name, "deaf") == 0) {
        CHECK(res->qemu->exit_end == ETUDE_END_SIGNALED && res->qemu->exit_value == SIGKILL,
              "deaf");
    } else if (strcmp(r->name, "obey") == 0) {
        CHECK(res->qemu->exit_end == ETUDE_END_EXITED && res->qemu->exit_value == 0, "obey");
        CHECK(strcmp(res->reason, "host-qmp-quit") == 0, "obey");
    } else if (strcmp(r->name, "silent") == 0) {
        CHECK(res->running_us == 0, "silent");
    }
}

/** Check a stand-in row that ran and ended at the guest's or the host's
 * event: its start, its console and the accelerator it confirmed. */
static void check_ran(const struct row *r, const struct etude_machine_result *res)
{
    CHECK(res->running_us >= res->started_us && res->running_us != 0, r->name);
    CHECK(find(&res->console, "fake: running\r\nfake: got go\r\n", 0) >= 0, r->name);
    CHECK(res->end_console == res->console.total, r->name);
    CHECK(res->accel != NULL && strcmp(res->accel, etude_accel_name(r->accel)) == 0, r->name);
}

/** Run the thirteen stand-in machines at once and check each one's end. */
static void test_ends(void)
{
    static struct row rows[] = {
        ROW("early", "early", ETUDE_ACCEL_TCG, "i686", ETUDE_END_QEMU_EXITED, 0),
        ROW("refuse", "refuse", ETUDE_ACCEL_KVM, "i686", ETUDE_END_ACCEL_FAILED, 0),
        ROW("wrong", "wrong", ETUDE_ACCEL_KVM, "x86_64", ETUDE_END_ACCEL_FAILED, 0),
        ROW("old", "old", ETUDE_ACCEL_TCG, "i686", ETUDE_END_SHUTDOWN, 'g'),
        ROW("old-wrong", "old-wrong", ETUDE_ACCEL_TCG, "i686", ETUDE_END_ACCEL_FAILED, 0),
        ROW("shutdown", "shutdown", ETUDE_ACCEL_TCG, "x86_64", ETUDE_END_SHUTDOWN, 'g'),
        ROW("reset", "reset", ETUDE_ACCEL_HVF, "i686", ETUDE_END_RESET, 'g'),
        ROW("panic", "panic", ETUDE_ACCEL_WHPX, "x86_64", ETUDE_END_PANIC, 'g'),
        ROW("host", "host", ETUDE_ACCEL_TCG, "i686", ETUDE_END_QEMU_EXITED, 'g'),
        ROW("crash", "crash", ETUDE_ACCEL_TCG, "x86_64", ETUDE_END_QEMU_EXITED, 0),
        ROW("obey", "obey", ETUDE_ACCEL_TCG, "i686", ETUDE_END_STOPPED, 'c'),
        ROW("deaf", "deaf", ETUDE_ACCEL_TCG, "x86_64", ETUDE_END_STOPPED, 'c'),
        ROW("silent", "silent", ETUDE_ACCEL_TCG, "i686", ETUDE_END_STOPPED, 'C'),
    };
    int n = (int)(sizeof(rows) / sizeof(rows[0])), i;
    struct etude_qemu *q = etude_qemu_new();

    CHECK(q != NULL, "launcher");
    if (q == NULL)
        return;
    for (i = 0; i < n; i++)
        row_start(q, &rows[i], self, self, FAKE_GRACE_US);
    run_rows(q, rows, n);
    for (i = 0; i < n; i++) {
        row_check(&rows[i]);
        if (rows[i].m == NULL) {
            row_free(&rows[i]);
            continue;
        }
        check_details(&rows[i], etude_machine_result(rows[i].m));
        if (rows[i].act == 'g')
            check_ran(&rows[i], etude_machine_result(rows[i].m));
        row_free(&rows[i]);
    }
    etude_qemu_free(q);
    CHECK(!left_in(fix_dir(), "etude-qemu."), "the private directories were removed");
}

/** A console file that exists already and a profile QEMU cannot take leave
 * nothing started. */
static void test_refused_starts(void)
{
    struct etude_qemu *q = etude_qemu_new();
    struct etude_machine_spec spec;
    struct etude_machine *m = NULL;
    struct row r;
    FILE *f;

    CHECK(q != NULL, "launcher");
    if (q == NULL)
        return;
    memset(&r, 0, sizeof(r));
    r.name = "exists";
    r.mode = "shutdown";
    r.arch = "i686";
    row_spec(&r, &spec, self, self, FAKE_GRACE_US);
    f = fopen(spec.console_path, "w");
    CHECK(f != NULL, "exists");
    if (f != NULL)
        fclose(f);
    errno = 0;
    CHECK(etude_qemu_start(q, &spec, &m) != 0 && errno == EEXIST && m == NULL, "exists");
    row_free(&r);
    memset(&r, 0, sizeof(r));
    r.name = "badprofile";
    r.mode = "shutdown";
    r.arch = "i386";
    row_spec(&r, &spec, self, self, FAKE_GRACE_US);
    errno = 0;
    CHECK(etude_qemu_start(q, &spec, &m) != 0 && errno == EINVAL && m == NULL, "badprofile");
    CHECK(access(spec.console_path, F_OK) != 0, "badprofile");
    row_free(&r);
    CHECK(etude_qemu_wait(q, 0) == NULL, "nothing started");
    etude_qemu_free(q);
    CHECK(!left_in(fix_dir(), "etude-qemu."), "a refused start removed its directory");
}

/** Check the same boundary counts as bigmem.sh and record QEMU's peak. */
static void boundary_result(const struct row *r)
{
    const struct etude_machine_result *res = etude_machine_result(r->m);
    const char *report = strstr(res->console.data, "memory: usable ");
    unsigned long long usable = 0, unreachable = 0;

    CHECK(r->acted == 2, "boundary reached memprobe total");
    CHECK(report != NULL && sscanf(report, "memory: usable %llu KiB, unreachable %llu KiB", &usable,
                                   &unreachable) == 2,
          "boundary memory report");
    CHECK(unreachable == 9u * 1024 * 1024, "boundary keeps 9 GiB unreachable");
    CHECK(usable + 513 == 63u * 1024 * 1024, "boundary keeps 63 GiB below 64 GiB");
    CHECK(res->qemu->peak_bytes.quality != ETUDE_QUALITY_UNAVAILABLE &&
              res->qemu->peak_bytes.value > 0 &&
              res->qemu->peak_bytes.value <= ETUDE_BIGMEM_RESIDENT + ETUDE_BIGMEM_OVERHEAD,
          "boundary QEMU peak within reservation");
    printf("etude-qemu-test: boundary: configured %llu, resident allowance %llu, "
           "overhead %llu, disk backing 0, QEMU peak %llu bytes\n",
           (unsigned long long)ETUDE_BIGMEM_CONFIGURED, (unsigned long long)ETUDE_BIGMEM_RESIDENT,
           (unsigned long long)ETUDE_BIGMEM_OVERHEAD,
           (unsigned long long)res->qemu->peak_bytes.value);
}

/**
 * Give the path of `name` on PATH in `out`, or NULL when it is not there.
 *
 * tests/run puts its QEMU wrappers first on PATH, in the directory
 * RACH2_QEMU_SHIMS names, and each adds an `-accel` of its own. That
 * directory is skipped, so that QEMU is given the one accelerator the
 * launcher asks for.
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

/** Fill row `r` with the kernel and test image of its architecture's build. */
static int real_files(struct row *r)
{
    const char *dir = strcmp(r->arch, "x86_64") == 0 ? "build/x86_64" : "build";

    snprintf(r->kernel, sizeof(r->kernel), "%s/rach2/kernel", dir);
    snprintf(r->initrd, sizeof(r->initrd), "%s/images/test.krffs", dir);
    r->profile.kernel = r->kernel;
    r->profile.initrd = r->initrd;
    return access(r->kernel, R_OK) == 0 && access(r->initrd, R_OK) == 0;
}

/** Require a real recording to contain a WAV header and sample bytes. */
static void check_real_audio(const struct etude_machine_result *res)
{
    char header[44];
    FILE *wav = res->audio_path != NULL ? fopen(res->audio_path, "rb") : NULL;

    CHECK(wav != NULL, "real audio capture exists after QEMU ends");
    if (wav != NULL) {
        CHECK(fread(header, 1, sizeof(header), wav) == sizeof(header) &&
                  memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0 &&
                  fgetc(wav) != EOF,
              "real audio capture contains WAV samples");
        fclose(wav);
    }
}

/**
 * Boot the kernel of the architecture ARCH names, i686 when it is unset, in
 * the real QEMU under TCG, twice, and ask QEMU for an accelerator it does not
 * have. A host without QEMU or a tree without the kernel fails the rows.
 */
static void test_real(int boundary)
{
#if defined(__APPLE__)
    const enum etude_accel absent = ETUDE_ACCEL_KVM;
#else
    const enum etude_accel absent = ETUDE_ACCEL_HVF;
#endif
    static struct row rows[] = {
        ROW("real-poweroff", NULL, ETUDE_ACCEL_TCG, "i686", ETUDE_END_SHUTDOWN, 'a'),
        ROW("real-cancel", NULL, ETUDE_ACCEL_TCG, "i686", ETUDE_END_STOPPED, 'P'),
        ROW("real-absent", NULL, ETUDE_ACCEL_TCG, "i686", ETUDE_END_ACCEL_FAILED, 0),
        ROW("real-audio-sb16", NULL, ETUDE_ACCEL_TCG, "i686", ETUDE_END_SHUTDOWN, 'a'),
        ROW("real-boundary", NULL, ETUDE_ACCEL_TCG, "x86_64", ETUDE_END_STOPPED, 'b'),
    };
    const char *arch = getenv("ARCH") != NULL && *getenv("ARCH") != '\0' ? getenv("ARCH") : "i686";
    char q32[PATH_MAX], q64[PATH_MAX];
    int n = (int)(sizeof(rows) / sizeof(rows[0])), i;
    const struct etude_machine_result *res;
    struct etude_qemu *q;

    rows[0].profile.audio = "hda";
    rows[3].profile.audio = "sb16";
    rows[2].accel = absent;
    if (boundary) {
        CHECK(strcmp(arch, "x86_64") == 0, "boundary requires ARCH=x86_64");
        if (strcmp(arch, "x86_64") != 0)
            return;
        rows[0] = rows[4];
        n = 1;
    } else {
        n = 4;
    }
    if (strcmp(arch, "i686") != 0 && strcmp(arch, "x86_64") != 0) {
        printf("etude-qemu-test: fail: ARCH is i686 or x86_64, not %s\n", arch);
        fails++;
        return;
    }
    if (on_path("qemu-system-i386", q32, sizeof(q32)) == NULL ||
        on_path("qemu-system-x86_64", q64, sizeof(q64)) == NULL) {
        printf("etude-qemu-test: fail: cannot run the boots: no qemu-system-i386 and "
               "qemu-system-x86_64 on PATH\n");
        fails++;
        return;
    }
    for (i = 0; i < n; i++) {
        rows[i].arch = arch;
        if (!real_files(&rows[i])) {
            printf("etude-qemu-test: fail: cannot run the boots: %s or %s is not built\n",
                   rows[i].kernel, rows[i].initrd);
            fails++;
            return;
        }
    }
    q = etude_qemu_new();
    CHECK(q != NULL, "launcher");
    if (q == NULL)
        return;
    for (i = 0; i < n; i++)
        row_start(q, &rows[i], q32, q64, REAL_GRACE_US);
    run_rows(q, rows, n);
    for (i = 0; i < n; i++) {
        row_check(&rows[i]);
        if (rows[i].m == NULL) {
            row_free(&rows[i]);
            continue;
        }
        res = etude_machine_result(rows[i].m);
        if (rows[i].act == 'a')
            check_real_audio(res);
        if (rows[i].act == 'b')
            boundary_result(&rows[i]);
        if (rows[i].want == ETUDE_END_SHUTDOWN) {
            CHECK(res->accel != NULL && strcmp(res->accel, "tcg") == 0, rows[i].name);
            CHECK(res->by_guest && strcmp(res->reason, "guest-shutdown") == 0, rows[i].name);
            CHECK(find(&res->console, "poweroff: powering off", 0) >= 0, rows[i].name);
        }
        if (rows[i].want == ETUDE_END_ACCEL_FAILED)
            CHECK(res->accel == NULL && res->console.total == 0, rows[i].name);
        row_free(&rows[i]);
    }
    etude_qemu_free(q);
    CHECK(!left_in(fix_dir(), "etude-qemu."), "the real machines' directories were removed");
}

/**
 * Make the command line of profile `p` on `site` with the test's sockets, in
 * `max` arguments and `size` bytes.
 */
static int make_in(const struct etude_qemu_profile *p, const struct etude_qemu_site *site,
                   char **argv, size_t max, char *buf, size_t size)
{
    struct etude_qemu_line out = {argv, max, buf, size};

    return etude_qemu_argv(p, site, "/s/c", "/s/q", "/s/one,two.wav", &out);
}

/** Make the command line of profile `p` on `site` with the test's sockets. */
static int make(const struct etude_qemu_profile *p, const struct etude_qemu_site *site, char **argv,
                char *buf, size_t size)
{
    return make_in(p, site, argv, ETUDE_QEMU_ARGS_MAX, buf, size);
}

/** Say whether an option has exactly `value` as one argument. */
static int has_arg(char *const argv[], const char *option, const char *value)
{
    size_t i;

    for (i = 0; argv[i] != NULL && argv[i + 1] != NULL; i++)
        if (strcmp(argv[i], option) == 0 && strcmp(argv[i + 1], value) == 0)
            return 1;
    return 0;
}

/** Check both audio cards, WAV path quoting, and a missing or unknown card. */
static void test_audio_argv(void)
{
    struct etude_qemu_profile p = {0};
    struct etude_qemu_site site = {ETUDE_ACCEL_TCG, "qemu", "qemu", NULL, NULL, 0};
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];
    struct etude_qemu_line out = {argv, ETUDE_QEMU_ARGS_MAX, buf, sizeof(buf)};
    int i;

    p.architecture = "i686";
    p.kernel = "kernel";
    p.cpu = "max";
    p.processors = 1;
    p.memory_bytes = 128u << 20;
    for (i = 0; i < 2; i++) {
        p.audio = i == 0 ? "hda" : "sb16";
        CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, "audio command line");
        CHECK(has_arg(argv, "-audiodev", "wav,id=etude-audio,path=/s/one,,two.wav"),
              "audio WAV path quoting");
        CHECK(has_arg(argv, "-device",
                      i == 0 ? "intel-hda,id=etude-hda" : "sb16,audiodev=etude-audio"),
              "audio controller");
        if (i == 0)
            CHECK(has_arg(argv, "-device", "hda-duplex,bus=etude-hda.0,audiodev=etude-audio"),
                  "audio HDA codec");
    }
    CHECK(etude_qemu_argv(&p, &site, "/c", "/q", NULL, &out) == EINVAL,
          "audio needs a launcher path");
    p.audio = "invalid";
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == EINVAL, "unknown audio card");
    p.audio = NULL;
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0 &&
              !has_arg(argv, "-device", "sb16,audiodev=etude-audio"),
          "audio absent by default");
}

/** Check isolated recordings, their lifetime, and refusal to replace a file. */
static void test_audio(void)
{
    struct row rows[] = {
        ROW("audio-hda", "shutdown", ETUDE_ACCEL_TCG, "i686", ETUDE_END_SHUTDOWN, 'g'),
        ROW("audio-sb16", "shutdown", ETUDE_ACCEL_TCG, "x86_64", ETUDE_END_SHUTDOWN, 'g')};
    struct etude_qemu *q = etude_qemu_new();
    struct etude_machine_spec spec;
    struct etude_machine *refused = NULL;
    char temporary[PATH_MAX], text[32] = "";
    const char *paths[2];
    FILE *f;
    int i;

    CHECK(q != NULL, "audio launcher");
    if (q == NULL)
        return;
    rows[0].profile.audio = "hda";
    rows[1].profile.audio = "sb16";
    for (i = 0; i < 2; i++) {
        row_spec(&rows[i], &spec, self, self, FAKE_GRACE_US);
        if (i == 1)
            spec.audio_path = fix_path("recording.wav");
        CHECK(etude_qemu_start(q, &spec, &rows[i].m) == 0, "audio starts");
        if (rows[i].m != NULL)
            etude_machine_type(rows[i].m, "go\r", 3);
    }
    run_rows(q, rows, 2);
    for (i = 0; i < 2; i++) {
        row_check(&rows[i]);
        paths[i] = rows[i].m != NULL ? etude_machine_result(rows[i].m)->audio_path : NULL;
        f = paths[i] != NULL ? fopen(paths[i], "rb") : NULL;
        CHECK(f != NULL, "audio survives machine end");
        if (f != NULL) {
            text[fread(text, 1, sizeof(text) - 1, f)] = '\0';
            CHECK(strcmp(text, "recorded samples") == 0, "audio recording bytes");
            fclose(f);
        }
    }
    if (paths[0] != NULL && paths[1] != NULL) {
        CHECK(strcmp(paths[0], paths[1]) != 0, "audio paths differ");
        snprintf(temporary, sizeof(temporary), "%s", paths[0]);
        CHECK(etude_qemu_start(q, &spec, &refused) != 0 && errno == EEXIST,
              "audio refuses an existing recording");
        row_free(&rows[0]);
        CHECK(access(temporary, F_OK) != 0 && errno == ENOENT, "audio temporary removed");
        row_free(&rows[1]);
        CHECK(access(fix_path("recording.wav"), F_OK) == 0, "audio evidence retained");
    } else {
        row_free(&rows[0]);
        row_free(&rows[1]);
    }
    etude_qemu_free(q);
    CHECK(!left_in(fix_dir(), "etude-qemu."), "audio directories removed");
}

/* The devices of the complete command-line fixture. */
static const struct etude_qemu_device argv_devices[] = {{"rtl8139", NULL, NULL, NULL, NULL, NULL},
                                                        {"e1000", NULL, NULL, NULL, NULL, NULL},
                                                        {"vga", NULL, NULL, NULL, NULL, NULL},
                                                        {"ahci", NULL, NULL, NULL, NULL, NULL},
                                                        {0}};

/** Check the command lines of profiles, and the profiles that are refused. */
static void test_argv(void)
{
    static const char *const disks[] = {"/d/one,two.img", "/d/b.img", NULL};
    static const char *const options[] = {"rach2.fsdisk", "quiet=1", NULL};
    static const char *const five[] = {"a", "b", "c", "d", "e", NULL};
    static const char *const want =
        "/q/x86_64 -accel kvm -nodefaults -display none -no-reboot -S "
        "-cpu qemu64,rdrand=off,rdseed=off,xsave=off,avx=off -smp 2 "
        "-m 268435456B -kernel /k/kernel -initrd /k/test.krffs "
        "-drive file=/d/one,,two.img,format=raw,if=ide,index=0,media=disk "
        "-drive file=/d/b.img,format=raw,if=ide,index=1,media=disk "
        "-netdev user,id=net0 -device rtl8139,netdev=net0 -netdev user,id=net1 "
        "-device e1000,netdev=net1 -vga std -device ahci "
        "-chardev socket,id=etude-console,path=/s/c -serial chardev:etude-console "
        "-chardev socket,id=etude-qmp,path=/s/q -mon chardev=etude-qmp,mode=control "
        "-append rach2.fsdisk quiet=1";
    struct etude_qemu_site site = {ETUDE_ACCEL_KVM, "/q/i686", "/q/x86_64", NULL, "/f/ovmf", 0};
    struct etude_qemu_profile p;
    struct etude_qemu_profile b;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096], line[4096];
    enum etude_accel a;
    size_t used = 0;
    int i;

    memset(&p, 0, sizeof(p));
    p.architecture = "x86_64";
    p.kernel = "/k/kernel";
    p.initrd = "/k/test.krffs";
    p.disks = disks;
    p.cpu = "qemu64";
    p.processors = 2;
    p.memory_bytes = 256u << 20;
    p.firmware = ETUDE_FIRMWARE_DIRECT;
    p.devices = argv_devices;
    p.boot_options = options;
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, "argv");
    line[0] = '\0';
    for (i = 0; argv[i] != NULL; i++)
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s", i ? " " : "", argv[i]);
    if (strcmp(line, want) != 0) {
        printf("etude-qemu-test: fail: the command line is\n  %s\nnot\n  %s\n", line, want);
        fails++;
    }
    CHECK(make_in(&p, &site, argv, 10, buf, sizeof(buf)) == E2BIG, "few arguments");
    CHECK(make_in(&p, &site, argv, ETUDE_QEMU_ARGS_MAX, buf, 100) == E2BIG, "few bytes");
    b = p;
    b.firmware = ETUDE_FIRMWARE_OVMF;
    b.initrd = NULL;
    b.boot_options = NULL;
    CHECK(make(&b, &site, argv, buf, sizeof(buf)) == 0 && strcmp(argv[14], "-bios") == 0 &&
              strcmp(argv[15], "/f/ovmf") == 0,
          "ovmf");
    b.cpu = "max-nopdpe1gb";
    CHECK(make(&b, &site, argv, buf, sizeof(buf)) == 0 && strcmp(argv[8], "-cpu") == 0 &&
              strcmp(argv[9], "max,rdrand=on,rdseed=on,xsave=on,avx=on,"
                              "pdpe1gb=off,enforce=on") == 0,
          "a profile with present and absent features");
    b.cpu = p.cpu;
    b.architecture = "i686";
    CHECK(make(&b, &site, argv, buf, sizeof(buf)) == ENOENT, "no i686 ovmf");
    b.firmware = ETUDE_FIRMWARE_SEABIOS;
    CHECK(make(&b, &site, argv, buf, sizeof(buf)) == 0 && strcmp(argv[0], "/q/i686") == 0 &&
              strcmp(argv[14], "-drive") == 0,
          "seabios");
    b.boot_options = options;
    CHECK(make(&b, &site, argv, buf, sizeof(buf)) == EINVAL, "seabios with a command line");
    b.boot_options = NULL;
    b.disks = NULL;
    CHECK(make(&b, &site, argv, buf, sizeof(buf)) == EINVAL, "seabios without a disk");
#define REFUSED(field, value, row)                                     \
    do {                                                               \
        b = p;                                                         \
        b.field = value;                                               \
        CHECK(make(&b, &site, argv, buf, sizeof(buf)) == EINVAL, row); \
    } while (0)
    REFUSED(architecture, "i386", "architecture");
    REFUSED(processors, 0, "no processor");
    REFUSED(processors, 65, "65 processors");
    REFUSED(memory_bytes, (128u << 20) + 4096, "memory not in mebibytes");
    REFUSED(cpu, "qemu64,rdrand=on", "a processor model that is no profile");
    REFUSED(cpu, NULL, "no processor profile");
    REFUSED(disks, five, "five disks");
    REFUSED(kernel, NULL, "direct without a kernel");
    REFUSED(firmware, (enum etude_firmware)7, "an unknown firmware");
    for (i = 0; i < ETUDE_ACCEL_COUNT; i++)
        CHECK(etude_accel_parse(etude_accel_name((enum etude_accel)i), &a) == 0 && (int)a == i,
              "accelerator names");
    CHECK(etude_accel_parse("kvm:tcg", &a) != 0, "a list of accelerators");
    CHECK(etude_qemu_accel_refused("qemu-system-x86_64: -accel kvm: failed to initialize kvm: "
                                   "No such device\n",
                                   ETUDE_ACCEL_KVM),
          "kvm refused");
    CHECK(etude_qemu_accel_refused("warning: x\nqemu-system-x86_64.exe: -accel whpx: "
                                   "WHPX: No accelerator found, hr=80070057",
                                   ETUDE_ACCEL_WHPX),
          "whpx refused");
    CHECK(!etude_qemu_accel_refused("qemu-system-x86_64: -accel kvm: failed\n", ETUDE_ACCEL_HVF),
          "another accelerator's refusal");
    CHECK(!etude_qemu_accel_refused("qemu-system-i386: -cpu bogus: unknown\n", ETUDE_ACCEL_TCG),
          "another option's error");
    CHECK(!etude_qemu_accel_refused(": -accel tcg: at the start of a line\n", ETUDE_ACCEL_TCG),
          "no program before the option");
}

/** Find an argument after `option` whose whole value is `value`. */
static int has_argument(char *const argv[], const char *option, const char *value)
{
    size_t i;

    for (i = 0; argv[i] != NULL && argv[i + 1] != NULL; i++)
        if (strcmp(argv[i], option) == 0 && strcmp(argv[i + 1], value) == 0)
            return 1;
    return 0;
}

/** Check DHCP subnets, two cards and the independent network options. */
static void test_network_argv(void)
{
    struct etude_qemu_device devices[] = {
        {"rtl8139", "192.168.90.0/24", "192.168.90.2", "192.168.90.20", "192.168.90.3",
         "52:54:00:12:34:56"},
        {"e1000", "10.0.3.0/24", NULL, NULL, NULL, "52:54:00:12:34:58"},
        {0}};
    struct etude_qemu_profile p = {0};
    struct etude_qemu_site site = {ETUDE_ACCEL_TCG, "qemu32", "qemu64", NULL, NULL, 0};
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];

    p.architecture = "i686";
    p.kernel = "kernel";
    p.cpu = "max";
    p.processors = 1;
    p.memory_bytes = 128u << 20;
    p.devices = devices;
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, "network argv");
    CHECK(has_argument(argv, "-netdev",
                       "user,id=net0,net=192.168.90.0/24,host=192.168.90.2,"
                       "dhcpstart=192.168.90.20,dns=192.168.90.3"),
          "DHCP network");
    CHECK(has_argument(argv, "-netdev", "user,id=net1,net=10.0.3.0/24"), "second network");
    CHECK(has_argument(argv, "-device", "rtl8139,netdev=net0,mac=52:54:00:12:34:56"),
          "first station address");
    CHECK(has_argument(argv, "-device", "e1000,netdev=net1,mac=52:54:00:12:34:58"),
          "second station address");
    devices[0].model = "e1000";
    devices[0].net = "10.90.0.0/16";
    devices[0].host = "10.90.0.2";
    devices[0].dhcpstart = "10.90.0.20";
    devices[0].dns = "10.90.0.3";
    p.architecture = "x86_64";
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, "wide network argv");
    CHECK(has_argument(argv, "-netdev",
                       "user,id=net0,net=10.90.0.0/16,host=10.90.0.2,"
                       "dhcpstart=10.90.0.20,dns=10.90.0.3"),
          "DHCP /16 network");
    devices[0].net = NULL;
    devices[0].host = "10.0.2.2";
    devices[0].dhcpstart = "10.0.2.20";
    devices[0].dns = "10.0.2.3";
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, "default network with addresses");
    CHECK(has_argument(argv, "-netdev",
                       "user,id=net0,host=10.0.2.2,dhcpstart=10.0.2.20,"
                       "dns=10.0.2.3"),
          "independent addresses");
}

/** Refuse malformed networks, foreign addresses and duplicate stations. */
static void test_network_refused(void)
{
    static const char *const bad_net[] = {"",
                                          "10.0.2.0",
                                          "10.0.2.0/",
                                          "10.0.2.0/-1",
                                          "10.0.2.0/33",
                                          "10.0.2.0/999999999999",
                                          "10.0.2.0/24,host=10.0.2.2",
                                          "10.0.2.1/24",
                                          "256.0.2.0/24",
                                          "10.00.2.0/24",
                                          "10.0.2/24",
                                          "10.0.2.0/024",
                                          NULL};
    static const char *const bad_address[] = {
        "",         "10.0.3.2",   "10.0.2.256", "10.0.2.2/24", "10.0.2.2,dns=10.0.2.3",
        "10.0.2.0", "10.0.2.255", "1.2.3",      "10.0.2.02",   NULL};
    static const char *const bad_mac[] = {"",
                                          "00:00:00:00:00:00",
                                          "ff:ff:ff:ff:ff:ff",
                                          "53:54:00:12:34:56",
                                          "52:54:00:12:34",
                                          "52:54:00:12:34:gg",
                                          "52:54:00:12:34:5",
                                          "52:54:00:12:34:56,other=1",
                                          NULL};
    struct etude_qemu_device devices[] = {
        {"e1000", NULL, NULL, NULL, NULL, NULL}, {"rtl8139", NULL, NULL, NULL, NULL, NULL}, {0}};
    size_t i;

    for (i = 0; bad_net[i] != NULL; i++) {
        devices[0].net = bad_net[i];
        CHECK(etude_qemu_devices_check(devices) == EINVAL, "bad network");
    }
    devices[0].net = NULL;
    for (i = 0; bad_address[i] != NULL; i++) {
        devices[0].host = bad_address[i];
        CHECK(etude_qemu_devices_check(devices) == EINVAL, "bad host address");
        devices[0].host = NULL;
        devices[0].dns = bad_address[i];
        CHECK(etude_qemu_devices_check(devices) == EINVAL, "bad DNS address");
        devices[0].dns = NULL;
        devices[0].dhcpstart = bad_address[i];
        CHECK(etude_qemu_devices_check(devices) == EINVAL, "bad DHCP address");
        devices[0].dhcpstart = NULL;
    }
    for (i = 0; bad_mac[i] != NULL; i++) {
        devices[0].mac = bad_mac[i];
        CHECK(etude_qemu_devices_check(devices) == EINVAL, "bad station address");
    }
    devices[0].mac = "52:54:aa:bb:cc:dd";
    devices[1].mac = "52:54:AA:BB:CC:DD";
    CHECK(etude_qemu_devices_check(devices) == EINVAL, "duplicate station address");
    devices[0].mac = NULL;
    devices[1].mac = "52:54:00:12:34:56";
    CHECK(etude_qemu_devices_check(devices) == EINVAL, "duplicate default station address");
    devices[0].mac = "52:54:00:12:34:57";
    devices[1].mac = NULL;
    CHECK(etude_qemu_devices_check(devices) == 0, "default skips used station address");
    devices[0].model = "ahci";
    CHECK(etude_qemu_devices_check(devices) == EINVAL, "network options on another device");
}

/** Reject a reduced reservation and any change to the boundary workload. */
static void test_memory(void)
{
    static const char *const rescue[] = {"rach2.rescue", NULL};
    struct etude_qemu_memory memory;
    struct etude_qemu_profile p;
    struct etude_qemu_site site = {ETUDE_ACCEL_TCG, self, self, NULL, NULL, 1};
    struct etude_machine_spec spec;
#if defined(__APPLE__)
    struct rlimit core, changed;
#endif
    struct etude_machine *m = NULL;
    struct etude_qemu *q = etude_qemu_new();
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];
    int i, backend = 0, size = 0;

    CHECK(q != NULL, "memory launcher");
    if (q == NULL)
        return;
    memset(&p, 0, sizeof p);
    p.architecture = "x86_64";
    p.kernel = "/not-started";
    p.initrd = "/not-started";
    p.cpu = "max-nopdpe1gb";
    p.processors = 1;
    p.memory_bytes = ETUDE_BIGMEM_CONFIGURED;
    p.boot_options = rescue;
    CHECK(etude_qemu_memory(&p, &memory) == 0 && memory.resident_bytes == ETUDE_BIGMEM_CONFIGURED &&
              memory.backing_bytes == 0 && memory.overhead_bytes == (128u << 20),
          "ordinary memory reserves configured RAM");
    p.memory_profile = "bigmem-boundary-v1";
    CHECK(etude_qemu_memory(&p, &memory) == 0 &&
              memory.configured_bytes == ETUDE_BIGMEM_CONFIGURED &&
              memory.resident_bytes == ETUDE_BIGMEM_RESIDENT &&
              memory.overhead_bytes == ETUDE_BIGMEM_OVERHEAD && memory.backing_bytes == 0,
          "boundary memory amounts");
    CHECK(make(&p, &site, argv, buf, sizeof buf) == 0, "boundary argv");
    for (i = 0; argv[i] != NULL; i++) {
        backend |= strcmp(argv[i], "memory-backend-ram,id=etude-ram,size=77309411328B,"
                                   "prealloc=off,merge=off"
#if !defined(__APPLE__)
                                   ",dump=off"
#endif
                          ) == 0;
        size |= strcmp(argv[i], "-m") == 0 && argv[i + 1] != NULL &&
                strcmp(argv[i + 1], "77309411328B") == 0;
    }
    CHECK(backend && size, "boundary keeps 72 GiB with preallocation off");
    site.boundary_lazy = 0;
    CHECK(make(&p, &site, argv, buf, sizeof buf) == ENOTSUP, "unproved site backing refused");
    site.boundary_lazy = 1;
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = "unsafe-memory";
    spec.profile = &p;
    spec.site = &site;
    spec.memory_reserved_bytes = ETUDE_BIGMEM_RESIDENT + ETUDE_BIGMEM_OVERHEAD - 1;
    errno = 0;
    CHECK(etude_qemu_start(q, &spec, &m) != 0 && errno == ENOMEM && m == NULL,
          "unsafe boundary allocation refused before execution");
#if defined(__APPLE__)
    CHECK(getrlimit(RLIMIT_CORE, &core) == 0, "read the core-file limit");
    if (core.rlim_max > 0) {
        changed = core;
        changed.rlim_cur = 1;
        CHECK(setrlimit(RLIMIT_CORE, &changed) == 0, "enable core files in the test");
        spec.memory_reserved_bytes++;
        errno = 0;
        CHECK(etude_qemu_start(q, &spec, &m) != 0 && errno == ENOTSUP && m == NULL,
              "boundary core files refused before execution");
        CHECK(setrlimit(RLIMIT_CORE, &core) == 0, "restore the core-file limit");
    } else {
        CHECK(core.rlim_cur == 0, "the hard core-file limit already disables core files");
    }
#endif
    p.memory_bytes = UINT64_C(8) << 30;
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "lazy 8 GiB workload refused");
    p.memory_bytes = ETUDE_BIGMEM_CONFIGURED;
    p.cpu = "max";
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "boundary needs 2 MiB page tables");
    p.cpu = "max-nopdpe1gb";
    p.architecture = "i686";
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "boundary needs x86-64");
    p.architecture = "x86_64";
    p.processors = 2;
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "boundary needs one processor");
    p.processors = 1;
    p.boot_options = NULL;
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "boundary needs rescue boot");
    p.memory_profile = "file";
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "unproved file backing refused");
    etude_qemu_free(q);
}

int main(int argc, char **argv)
{
    const char *fake = getenv("ETUDE_FAKE_QEMU");

    if (fake != NULL && *fake != '\0')
        return fake_qemu(fake, argc, argv);
    if (realpath(argv[0], self) == NULL) {
        printf("etude-qemu-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    if (setenv("TMPDIR", fix_dir(), 1) != 0)
        return 1;
    if (argc == 2 && strcmp(argv[1], "--boundary") == 0) {
        test_real(1);
    } else if (argc == 2 && strcmp(argv[1], "--network") == 0) {
        test_network_argv();
        test_network_refused();
    } else if (argc == 2 && strcmp(argv[1], "--audio") == 0) {
        test_audio_argv();
        test_audio();
    } else if (argc == 2 && strcmp(argv[1], "--memory") == 0) {
        test_memory();
    } else {
        test_argv();
        test_network_argv();
        test_network_refused();
        test_audio_argv();
        test_audio();
        test_memory();
        test_ends();
        test_refused_starts();
        test_real(0);
    }
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_qemu_hosttest: pass\n");
    return 0;
}
