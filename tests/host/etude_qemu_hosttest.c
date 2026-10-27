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
 * host-deps: tests/host/compat/desktop_host.c
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 * host-first: it runs for minutes
 * host-alone: a stand-in QEMU must quit within a grace of 300 ms
 * host-arch: it boots the kernel and test image of ARCH
 *
 * The command lines of profiles, and the profiles the launcher refuses, are
 * checked as text, disks on IDE, AHCI and NVMe, a CD, disks that boot first,
 * qcow2 overlays, an IDE disk with a serial and the firmware's flash among
 * them. A capture of a card's frames is a filter-dump after the card, and a
 * capture of a card the profile lacks is refused. Fifteen stand-in machines
 * then run at once, each with an end of its own, and each must keep its own
 * console and end: a QEMU that fails before it connects is an unexpected exit,
 * and one that reports that it could not start the accelerator is a failed
 * accelerator; so is one whose QMP reports another accelerator in use, through
 * query-accelerators or, for a QEMU without that command, query-kvm. QMP's
 * SHUTDOWN events of the guest give a shutdown, a reset and a panic; one of the
 * host, or an exit without an event, is an unexpected exit; a cancellation of a
 * machine that obeys QMP's quit, of one that ignores both quit and SIGTERM, of
 * one that never answers QMP, and of one that obeys quit while its QEMU is
 * suspended, which the cancellation resumes, is a cancellation; a cancellation
 * of one that ignores SIGTERM after its guest's shutdown ends its QEMU and
 * keeps the shutdown. Each leaves no process and no socket behind. With the
 * real QEMU, the kernel of the architecture ARCH names boots under TCG to the
 * debug shell, the first boot with an empty disk on IDE and two on AHCI that
 * the kernel must find where the profile put them, and its `poweroff` must
 * give a shutdown; a cancellation at the prompt must give a cancellation, and
 * an accelerator this host's QEMU does not have must give a failed
 * accelerator. OVMF's code image, as tools/setup/ovmf_path.sh finds it, boots
 * from the launcher's read-only flash with a CD and an empty disk of 4 TiB on
 * NVMe, and its boot manager must name both.
 *
 * The memory rows check the reservations of an ordinary profile and of the
 * boundary profile, refuse a changed boundary profile and a site that has not
 * enabled it, and refuse a boundary start one byte short of its reservation
 * before anything runs. With `--boundary` and ARCH=x86_64 the program runs
 * only the 72 GiB boundary boot in the real QEMU, which must report the
 * counts the case machine.bigmem.boundary-73728 requires and keep QEMU's
 * resident peak within the reservation.
 *
 * The network rows check the networks and station addresses of two cards and
 * of cards on hubs of their own beside a card on a user network, each hub
 * card on the hub of its own index with its station address and its capture.
 * They refuse malformed and foreign addresses, bad and duplicate station
 * addresses, a card on a hub with an option of a user network, the
 * restriction, a forward or a guest forward, and a hub on a device that is no
 * network card.
 *
 * The forward rows check the user network of each network card as text: the
 * forwards of the case's local ports by TCP and by UDP, a card without
 * forwards, and a card's network options before its forwards. They refuse a
 * forward on a device that is no network card, a forward of a port the
 * profile does not hold or that is below 1024, one to the guest's port 0 or
 * 65536, and a local port forwarded twice by one protocol.
 *
 * The guest forward rows check a restricted network whose guest reaches two
 * local ports through relays, with a relay's comma and quote quoted for QEMU
 * and for its shell parser, one guest port forwarded from two addresses, and
 * the addresses QEMU can forward on the default network, on a card's own and
 * on a wide one. They refuse an address of the host, of DHCP or outside the
 * network, a guest's port of 0 or 65536, a local port not held, below 1024 or
 * listened on by a TCP forward, a relay without a program or with a control
 * character, one address and port forwarded twice, and a restriction or a
 * guest forward on a device that is no network card. The smallest buffer
 * that takes the line must have it whole. One card has every
 * option of its user network in its one `-netdev`: its network, host, DHCP
 * and name server addresses, the restriction, forwards and guest forwards,
 * with its station address on its `-device`.
 */
#ifdef _WIN32
#include "win/files.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "compat/pathmax_host.h"
#include "compat/desktop_host.h"
#include "compat/fixdir_host.c"
#include "compat/ovmf_host.h"
#include "etude/qemu.h"

#ifndef _WIN32
extern char **environ;
#endif

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                        \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            printf("etude-qemu-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                            \
        }                                                                                       \
    } while (0)

/* The grace periods: short for the stand-ins, longer for a real QEMU. */
#define FAKE_GRACE_US 300000u
#define REAL_GRACE_US 5000000u

/* The UEFI row's empty disk on NVMe, 4 TiB as the uefi suite's, and what
 * OVMF's boot manager says on the console of the CD and of that disk. */
#define BIG_BYTES (UINT64_C(4) << 40)
#define UEFI_CD   "UEFI QEMU DVD-ROM"
#define UEFI_NVME "UEFI QEMU NVMe Ctrl rach2big"

/* Exact chardev arguments use the transport of the native launcher, which on
 * Windows adds -no-shutdown after them. */
#ifdef _WIN32
#define TEST_CONSOLE "27001"
#define TEST_QMP     "27002"
#define TEST_CHANNELS                                                                             \
    "-chardev socket,id=etude-console,host=127.0.0.1,port=27001 -serial chardev:etude-console "   \
    "-chardev socket,id=etude-qmp,host=127.0.0.1,port=27002 -mon chardev=etude-qmp,mode=control " \
    "-no-shutdown "
#else
#define TEST_CONSOLE "/s/c"
#define TEST_QMP     "/s/q"
#define TEST_CHANNELS                                                           \
    "-chardev socket,id=etude-console,path=/s/c -serial chardev:etude-console " \
    "-chardev socket,id=etude-qmp,path=/s/q -mon chardev=etude-qmp,mode=control "
#endif

/* This program, by an absolute path. */
static char self[PATH_MAX];

/* ---- The stand-in for QEMU. ---- */

/** Write all of `text` to `fd`, or end the stand-in. */
static void put(host_socket fd, const char *text)
{
    size_t len = strlen(text), done = 0;
    ssize_t n;

    while (done < len) {
        n = host_socket_write(fd, text + done, len - done);
        if (n <= 0)
            _exit(120);
        done += (size_t)n;
    }
}

/** Connect to the endpoint used by QEMU's native client chardev. */
static host_socket fake_connect(const char *endpoint)
{
    host_socket socket = host_qemu_connect(endpoint);
    if (socket == HOST_SOCKET_INVALID)
        _exit(121);
    return socket;
}

/** Read one line of QMP into `line`, without its newline; -1 at its end. */
static int fake_line(host_socket fd, char *line, size_t size)
{
    size_t n = 0;
    char c;

    for (;;) {
        if (host_socket_read(fd, &c, 1) != 1)
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
static void fake_shutdown(host_socket qmp, int guest, const char *reason)
{
    char text[256];

    snprintf(text, sizeof(text),
             "{\"timestamp\": {\"seconds\": 1, \"microseconds\": 2}, \"event\": \"SHUTDOWN\", "
             "\"data\": {\"guest\": %s, \"reason\": \"%s\"}}\r\n",
             guest ? "true" : "false", reason);
    put(qmp, text);
}

/** Answer QMP until it asks to quit, then report the quit and exit 0. */
static void fake_obey(host_socket qmp)
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
static void fake_start(host_socket qmp, const char *mode, const char *accel)
{
    const char *other = strcmp(accel, "tcg") == 0 ? "kvm" : "tcg";
    char line[512], name[64], text[256];
    long id;

    if (strncmp(accel, "whpx,", 5) == 0)
        accel = "whpx";
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
static void fake_wait_go(host_socket con)
{
    char seen[4] = "";
    char c;

    while (strcmp(seen, "go\r") != 0) {
        if (host_socket_read(con, &c, 1) != 1)
            _exit(123);
        memmove(seen, seen + 1, 2);
        seen[2] = c;
        seen[3] = '\0';
    }
}

/** Find QEMU's chardev endpoint, or end the stand-in on malformed arguments. */
static const char *fake_socket(int argc, char **argv, const char *id)
{
    const char *endpoint = host_qemu_endpoint(argc, argv, id);
    if (endpoint == NULL)
        _exit(124);
    return endpoint;
}

/**
 * Be QEMU, as ETUDE_FAKE_QEMU's `mode` says: connect to the console and
 * QMP, answer QMP's start, write to the console, and end. With
 * `-no-shutdown` it answers QMP after a shutdown of the guest until its quit.
 */
static int fake_qemu(const char *mode, int argc, char **argv)
{
    const char *accel = NULL, *reason = NULL;
    int i, guest = 1, stopped = 0, kept = 0;
    host_socket con, qmp;
    const char *audio = NULL;
    FILE *wav;

    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-accel") == 0)
            accel = argv[i + 1];
        stopped |= strcmp(argv[i], "-S") == 0;
        if (strcmp(argv[i], "-audiodev") == 0)
            audio = strstr(argv[i + 1], "path=");
    }
    for (i = 1; i < argc; i++)
        kept |= strcmp(argv[i], "-no-shutdown") == 0;
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
        host_wait_forever();
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
        host_wait_forever();
    }
    if (strcmp(mode, "shutdown") == 0 || strcmp(mode, "old") == 0 || strcmp(mode, "linger") == 0)
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
    if (strcmp(mode, "linger") == 0) {
        signal(SIGTERM, SIG_IGN);
        host_wait_forever();
    }
    if (kept && guest)
        fake_obey(qmp);
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
                at the prompt, 'P' cancels at the prompt, 'b' runs the boundary probe,
                'u' boots by OVMF and cancels once it names the CD and the NVMe disk,
                's' suspends QEMU when running and cancels once it is seen stopped,
                'L' types go and cancels once QMP has reported SHUTDOWN */
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

/**
 * Act on row `r`, whose act is 'p', 'P', 'b' or 'a', once the kernel's debug
 * shell has given its prompt on the console of `res`, the machine's result:
 * type poweroff, cancel, start the boundary probe, or start the audio scale.
 */
static void row_shell(struct row *r, const struct etude_machine_result *res)
{
    long shell = find(&res->console, "Kernel debug shell active", 0);

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
 * Act on the suspended row `r`: suspend its QEMU once the machine runs, which
 * a second suspension cannot repeat, and cancel the machine once the stop is
 * seen.
 */
static void row_hold(struct row *r, const struct etude_machine_result *res)
{
    if (r->acted == 1 && res->qemu->suspension == ETUDE_SUSPEND_STOPPED) {
        r->acted = 2;
        etude_machine_cancel(r->m);
    }
    if (r->acted == 0 && res->state == ETUDE_MACHINE_RUNNING) {
        r->acted = 1;
        CHECK(etude_machine_suspend(r->m) == 0, r->name);
        CHECK(etude_machine_suspend(r->m) != 0 && errno == EBUSY, r->name);
    }
}

/** Act on a row whose machine has changed, as its `act` says. */
static void row_act(struct row *r)
{
    const struct etude_machine_result *res = etude_machine_result(r->m);

    if (r->act == 's') {
        row_hold(r, res);
        return;
    }
    if (r->act == 'L' && !r->acted && res->reason[0] != '\0') {
        r->acted = 1;
        etude_machine_cancel(r->m);
    }
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
    if (r->act == 'u' && find(&res->console, UEFI_CD, 0) >= 0 &&
        find(&res->console, UEFI_NVME, 0) >= 0) {
        r->acted = 1;
        etude_machine_cancel(r->m);
    }
    if (r->act == 'p' || r->act == 'P' || r->act == 'b' || r->act == 'a')
        row_shell(r, res);
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
    if (r->act == 'u') {
        r->profile.firmware = ETUDE_FIRMWARE_OVMF;
        r->profile.memory_bytes = 256u << 20;
    }
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
    if (r->act == 'g' || r->act == 'L')
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

/** Run the `n` rows at once until every machine has ended, acting on each
 * at least once a second. */
static void run_rows(struct etude_qemu *q, struct row *rows, int n)
{
    int i, left = 0;

    for (i = 0; i < n; i++)
        left += rows[i].m != NULL;
    while (left > 0) {
        if (etude_qemu_wait(q, 1000) != NULL)
            left--;
        for (i = 0; i < n; i++)
            if (rows[i].m != NULL)
                row_act(&rows[i]);
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
        printf("etude-qemu-test: %s: exit %d value %d, QMP bad %u reason %s, "
               "console %llu bytes closed %d\n",
               r->name, res->qemu->exit_end, res->qemu->exit_value, res->qmp_bad, res->reason,
               (unsigned long long)res->console.total, res->console.closed);
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
        CHECK(host_process_stopped(res->qemu, 1), "deaf");
    } else if (strcmp(r->name, "linger") == 0) {
        CHECK(host_process_stopped(res->qemu, 1), "linger");
        CHECK(strcmp(res->reason, "guest-shutdown") == 0, "linger");
    } else if (strcmp(r->name, "obey") == 0) {
        CHECK(res->qemu->exit_end == ETUDE_END_EXITED && res->qemu->exit_value == 0, "obey");
        CHECK(strcmp(res->reason, "host-qmp-quit") == 0, "obey");
    } else if (strcmp(r->name, "held") == 0) {
        CHECK(res->qemu->exit_end == ETUDE_END_EXITED && res->qemu->exit_value == 0, "held");
        CHECK(strcmp(res->reason, "host-qmp-quit") == 0, "held");
        CHECK(res->qemu->suspensions == 1 && res->qemu->suspended_us >= res->running_us, "held");
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
    CHECK(res->console.closed, "the final console reached EOF");
    CHECK(res->end_console == res->console.total, r->name);
    CHECK(res->accel != NULL && strcmp(res->accel, etude_accel_name(r->accel)) == 0, r->name);
}

/** Run the fifteen stand-in machines at once and check each one's end. */
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
        ROW("held", "obey", ETUDE_ACCEL_TCG, "i686", ETUDE_END_STOPPED, 's'),
        ROW("silent", "silent", ETUDE_ACCEL_TCG, "i686", ETUDE_END_STOPPED, 'C'),
        ROW("linger", "linger", ETUDE_ACCEL_TCG, "x86_64", ETUDE_END_SHUTDOWN, 'L'),
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
        if (rows[i].act == 'g' || rows[i].act == 'L')
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

/** Check the boundary counts of machine.bigmem.boundary-73728 and record
 * QEMU's peak. */
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
 * Give row `r` empty disks of sizes of their own: one on IDE and two on AHCI,
 * each of whose places the kernel prints with its size.
 */
static void attach_disks(struct row *r)
{
    static struct etude_qemu_disk disks[4];
    static const char *const names[] = {"ide.img", "ahci0.img", "ahci1.img"};
    int fd, i;

    for (i = 0; i < 3; i++) {
        disks[i].path = fix_path(names[i]);
        disks[i].bus = i == 0 ? ETUDE_BUS_IDE : ETUDE_BUS_AHCI;
        fd = open(disks[i].path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        CHECK(fd >= 0 && ftruncate(fd, (off_t)1 << (20 + i)) == 0, "an empty disk");
        if (fd >= 0)
            close(fd);
    }
    r->profile.disks = disks;
}

/** Check that the kernel found each disk of attach_disks where it was put. */
static void check_real_disks(const struct etude_machine_result *res, const char *row)
{
    CHECK(find(&res->console, "ata: primary master: 2048 sectors", 0) >= 0, row);
    CHECK(find(&res->console, "ahci: port 0: 4096 sectors", 0) >= 0, row);
    CHECK(find(&res->console, "ahci: port 1: 8192 sectors", 0) >= 0, row);
}

/**
 * Boot the kernel of the architecture ARCH names, i686 when it is unset, in
 * the real QEMU under TCG, twice, and ask QEMU for an accelerator it does not
 * have. The first boot has the disks of attach_disks. A host without QEMU or a
 * tree without the kernel fails the rows.
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
    if (host_program("qemu-system-i386", q32, sizeof(q32)) == NULL ||
        host_program("qemu-system-x86_64", q64, sizeof(q64)) == NULL) {
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
    if (!boundary)
        attach_disks(&rows[0]);
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
        if (rows[i].profile.disks != NULL)
            check_real_disks(res, rows[i].name);
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
 * Give in `out` the code image of OVMF that tools/setup/ovmf_path.sh finds
 * for the suites that boot by UEFI, or NULL when it finds none.
 */
static const char *ovmf_code(char *out, size_t size)
{
    return host_ovmf_code(out, size);
}

/** Create the private sparse NVMe image and verify its full byte length. */
static int uefi_disk(const char *path)
{
    struct stat st;
    int fd, ok, saved;

    errno = 0;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    ok = fd >= 0 && ftruncate(fd, (off_t)BIG_BYTES) == 0 && fstat(fd, &st) == 0 &&
         (uint64_t)st.st_size == BIG_BYTES;
    saved = errno != 0 ? errno : EIO;

    if (fd >= 0)
        close(fd);
    CHECK(ok, "an empty sparse disk of 4 TiB");
    if (!ok)
        printf("etude-qemu-test: UEFI disk setup: %s\n", strerror(saved));
    return ok;
}

/**
 * Boot OVMF's code image, as ovmf_code finds it, in the real qemu-system-x86_64
 * under TCG, with a CD whose medium is the test image of the architecture
 * ARCH names and an empty sparse disk of 4 TiB on NVMe with a serial; the
 * firmware's boot manager must name both on the console. A host without QEMU
 * or OVMF, or a tree without the test image, fails the row.
 */
static void test_uefi(void)
{
    static struct row rows[] = {
        ROW("real-uefi", NULL, ETUDE_ACCEL_TCG, "x86_64", ETUDE_END_STOPPED, 'u'),
    };
    const char *dir =
        getenv("ARCH") != NULL && strcmp(getenv("ARCH"), "x86_64") == 0 ? "build/x86_64" : "build";
    struct etude_qemu_disk disks[3];
    char q32[PATH_MAX], q64[PATH_MAX], code[PATH_MAX], cd[PATH_MAX];
    const char *big = fix_path("big.img");
    struct etude_qemu *q;

    snprintf(cd, sizeof(cd), "%s/images/test.krffs", dir);
    if (host_program("qemu-system-i386", q32, sizeof(q32)) == NULL ||
        host_program("qemu-system-x86_64", q64, sizeof(q64)) == NULL ||
        ovmf_code(code, sizeof(code)) == NULL || access(cd, R_OK) != 0) {
        printf("etude-qemu-test: fail: cannot run the UEFI boot: no QEMU or OVMF, or %s is "
               "not built\n",
               cd);
        fails++;
        return;
    }
    if (!uefi_disk(big))
        return;
    memset(disks, 0, sizeof(disks));
    disks[0].path = cd;
    disks[0].cdrom = 1;
    disks[1].path = big;
    disks[1].bus = ETUDE_BUS_NVME;
    disks[1].serial = "rach2big";
    disks[1].discard = 1;
    rows[0].arch = strcmp(dir, "build") == 0 ? "i686" : "x86_64";
    rows[0].profile.disks = disks;
    rows[0].site.ovmf_x86_64 = code;
    q = etude_qemu_new();
    CHECK(q != NULL, "launcher");
    if (q != NULL) {
        row_start(q, &rows[0], q32, q64, REAL_GRACE_US);
        run_rows(q, rows, 1);
        row_check(&rows[0]);
        if (rows[0].m != NULL)
            CHECK(rows[0].acted, "OVMF names the CD and the NVMe disk with its serial");
        row_free(&rows[0]);
        etude_qemu_free(q);
    }
    unlink(big);
}

/**
 * Make the command line of profile `p` on `site` with the test's sockets, in
 * `max` arguments and `size` bytes.
 */
static int make_in(const struct etude_qemu_profile *p, const struct etude_qemu_site *site,
                   char **argv, size_t max, char *buf, size_t size)
{
    struct etude_qemu_line out = {argv, max, buf, size};

    return etude_qemu_argv(p, site, TEST_CONSOLE, TEST_QMP, "/s/one,two.wav", &out);
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
            CHECK(has_arg(argv, "-device", "hda-output,bus=etude-hda.0,audiodev=etude-audio"),
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
static const struct etude_qemu_device argv_devices[] = {
    {.model = "rtl8139"}, {.model = "e1000"}, {.model = "vga"}, {.model = "ahci"}, {0}};

/**
 * Fill `d` with `ide` disks on IDE and then `nvme` on NVMe, each with a
 * serial, ended by a NULL path; `d` has room for them and the end.
 */
static void fill_disks(struct etude_qemu_disk *d, size_t ide, size_t nvme)
{
    size_t i;

    memset(d, 0, (ide + nvme + 1) * sizeof(*d));
    for (i = 0; i < ide + nvme; i++) {
        d[i].path = "/d/x.img";
        if (i >= ide) {
            d[i].bus = ETUDE_BUS_NVME;
            d[i].serial = "s";
        }
    }
}

/**
 * Require the launcher to make the command line of profile `p` with `disks`
 * in place of its own when `ok`, and to refuse it otherwise.
 */
static void disks_row(const struct etude_qemu_profile *p, const struct etude_qemu_site *site,
                      const struct etude_qemu_disk *disks, int ok, const char *row)
{
    struct etude_qemu_profile b = *p;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];

    b.disks = disks;
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == (ok ? 0 : EINVAL), row);
}

/**
 * Check the disks on AHCI and the disks that boot first that the launcher
 * attaches, and those it refuses. `p` has the direct firmware.
 */
static void test_ahci_disks(const struct etude_qemu_profile *p, const struct etude_qemu_site *site)
{
    struct etude_qemu_disk d[ETUDE_QEMU_DISKS + 2];
    struct etude_qemu_profile b = *p;
    size_t i;

    b.firmware = ETUDE_FIRMWARE_SEABIOS;
    b.initrd = NULL;
    b.boot_options = NULL;
    fill_disks(d, 8, 0);
    for (i = 0; i < 6; i++)
        d[i].bus = ETUDE_BUS_AHCI;
    d[5].boot = 1;
    disks_row(&b, site, d, 1, "six disks on AHCI and two on IDE");
    d[6].bus = ETUDE_BUS_AHCI;
    disks_row(&b, site, d, 0, "seven disks on AHCI");
    fill_disks(d, 1, 0);
    d[0].bus = ETUDE_BUS_AHCI;
    d[0].cdrom = 1;
    disks_row(&b, site, d, 0, "a CD on AHCI");
    d[0].cdrom = 0;
    d[0].serial = "s";
    disks_row(&b, site, d, 0, "a serial on AHCI");
    fill_disks(d, 1, 1);
    d[0].boot = 1;
    disks_row(&b, site, d, 1, "a disk on IDE that boots first");
    disks_row(p, site, d, 0, "a disk that boots first beside -kernel");
    d[1].boot = 1;
    disks_row(&b, site, d, 0, "two disks that boot first");
}

/**
 * Require the command line of profile `p` with `disks` in place of its own to
 * have `want`, the arguments of the disks joined by spaces.
 */
static void disk_line(const struct etude_qemu_profile *p, const struct etude_qemu_site *site,
                      const struct etude_qemu_disk *disks, const char *want)
{
    struct etude_qemu_profile b = *p;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096], line[4096];
    size_t used = 0;
    int i;

    b.disks = disks;
    line[0] = '\0';
    if (make(&b, site, argv, buf, sizeof(buf)) == 0)
        for (i = 0; argv[i] != NULL; i++)
            used +=
                (size_t)snprintf(line + used, sizeof(line) - used, "%s%s", i ? " " : "", argv[i]);
    if (strstr(line, want) == NULL) {
        printf("etude-qemu-test: fail: the command line is\n  %s\nwithout\n  %s\n", line, want);
        fails++;
    }
}

/**
 * Check the command lines of disks on IDE and AHCI in one profile, the
 * second AHCI disk booted first; of a CD on IDE booted first; of a disk
 * on NVMe booted first; and of the q35 machine, whose disks on IDE are on
 * the ports of its own AHCI, the second booted first. `p` has the direct
 * firmware.
 */
static void test_disk_lines(const struct etude_qemu_profile *p, const struct etude_qemu_site *site)
{
    static const struct etude_qemu_disk mixed[] = {
        {"/d/a0", ETUDE_BUS_AHCI, 0, NULL, 0, NULL, 0, 0, 0},
        {"/d/i0", ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0},
        {"/d/a1", ETUDE_BUS_AHCI, 0, NULL, 1, NULL, 1, 0, 0},
        {"/d/i1", ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0},
        {NULL, ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0}};
    static const struct etude_qemu_disk cd[] = {
        {"/d/i0", ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0},
        {"/d/r.iso", ETUDE_BUS_IDE, 1, NULL, 0, NULL, 1, 0, 0},
        {"/d/i2", ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0},
        {NULL, ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0}};
    static const struct etude_qemu_disk nvme[] = {
        {"/d/n", ETUDE_BUS_NVME, 0, "s", 0, NULL, 1, 0, 0},
        {NULL, ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0}};
    static const struct etude_qemu_disk q35[] = {
        {"/d/i0", ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0},
        {"/d/i1", ETUDE_BUS_IDE, 0, NULL, 0, NULL, 1, 0, 0},
        {"/d/a0", ETUDE_BUS_AHCI, 0, NULL, 0, NULL, 0, 0, 0},
        {"/d/i2", ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0},
        {NULL, ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0}};
    struct etude_qemu_profile b = *p;

    b.firmware = ETUDE_FIRMWARE_SEABIOS;
    b.initrd = NULL;
    b.boot_options = NULL;
    disk_line(&b, site, mixed,
              "-m 268435456B -device ich9-ahci,id=etude-ahci "
              "-drive file=/d/a0,format=raw,if=none,id=etude-disk0 "
              "-device ide-hd,drive=etude-disk0,bus=etude-ahci.0 "
              "-drive file=/d/i0,format=raw,if=ide,index=0,media=disk "
              "-drive file=/d/a1,format=raw,if=none,id=etude-disk2,discard=unmap,"
              "detect-zeroes=unmap -device ide-hd,drive=etude-disk2,bus=etude-ahci.1,bootindex=0 "
              "-drive file=/d/i1,format=raw,if=ide,index=1,media=disk -netdev");
    disk_line(&b, site, cd,
              "-m 268435456B -drive file=/d/i0,format=raw,if=ide,index=0,media=disk "
              "-drive file=/d/r.iso,format=raw,if=none,id=etude-disk1,media=cdrom,readonly=on "
              "-device ide-cd,drive=etude-disk1,bus=ide.0,unit=1,bootindex=0 "
              "-drive file=/d/i2,format=raw,if=ide,index=2,media=disk -netdev");
    disk_line(&b, site, nvme,
              "-m 268435456B -drive file=/d/n,format=raw,if=none,id=etude-disk0 "
              "-device nvme,drive=etude-disk0,serial=s,bootindex=0 -netdev");
    b.q35 = 1;
    disk_line(&b, site, q35,
              "-m 268435456B -machine q35 -device ich9-ahci,id=etude-ahci "
              "-drive file=/d/i0,format=raw,if=ide,index=0,media=disk "
              "-drive file=/d/i1,format=raw,if=none,id=etude-disk1 "
              "-device ide-hd,drive=etude-disk1,bus=ide.1,unit=0,bootindex=0 "
              "-drive file=/d/a0,format=raw,if=none,id=etude-disk2 "
              "-device ide-hd,drive=etude-disk2,bus=etude-ahci.0 "
              "-drive file=/d/i2,format=raw,if=ide,index=2,media=disk -netdev");
}

/** Check the disks the launcher attaches together, and those it refuses. */
static void test_disks(const struct etude_qemu_profile *p, const struct etude_qemu_site *site)
{
    struct etude_qemu_disk d[ETUDE_QEMU_DISKS + 2];

    fill_disks(d, 4, 4);
    disks_row(p, site, d, 1, "eight disks, four of them on IDE");
    fill_disks(d, 5, 0);
    disks_row(p, site, d, 0, "five disks on IDE");
    fill_disks(d, 4, 0);
    d[3].cdrom = 1;
    disks_row(p, site, d, 1, "four disks on IDE, a CD among them");
    fill_disks(d, 1, 8);
    disks_row(p, site, d, 0, "nine disks");
    fill_disks(d, 0, 1);
    d[0].serial = "abcdefghijklmnopqrst";
    disks_row(p, site, d, 1, "a serial of 20 characters");
    d[0].serial = "abcdefghijklmnopqrstu";
    disks_row(p, site, d, 0, "a serial of 21 characters");
    d[0].serial = NULL;
    disks_row(p, site, d, 0, "an NVMe disk without a serial");
    d[0].serial = "a,b";
    disks_row(p, site, d, 0, "a serial with a comma");
    d[0].serial = "a b";
    disks_row(p, site, d, 0, "a serial with a space");
    d[0].serial = "s";
    d[0].cdrom = 1;
    disks_row(p, site, d, 0, "a CD on NVMe");
    fill_disks(d, 1, 0);
    d[0].serial = "s";
    disks_row(p, site, d, 1, "a serial on IDE");
    d[0].serial = "a,b";
    disks_row(p, site, d, 0, "a serial with a comma on IDE");
    d[0].serial = "s";
    d[0].cdrom = 1;
    disks_row(p, site, d, 0, "a serial on a CD");
    d[0].serial = NULL;
    d[0].qcow2 = 1;
    disks_row(p, site, d, 0, "a CD of an overlay");
    d[0].qcow2 = 0;
    d[0].discard = 1;
    disks_row(p, site, d, 0, "a CD with discard");
    d[0].cdrom = 0;
    d[0].qcow2 = 1;
    disks_row(p, site, d, 1, "an overlay with discard");
    d[0].qcow2 = 0;
    d[0].discard = 0;
    d[0].path = "";
    disks_row(p, site, d, 0, "a disk of no file");
    d[0].path = "/d/x.img";
    d[0].bus = (enum etude_disk_bus)7;
    d[0].serial = "s";
    disks_row(p, site, d, 0, "an unknown bus");
    test_ahci_disks(p, site);
}

/** Check the accelerators' names, and the reading of QEMU's refusal of one. */
static void test_accel_names(void)
{
    enum etude_accel a;
    int i;

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
    CHECK(etude_qemu_accel_refused("qemu-system-x86_64.exe: -accel "
                                   "whpx,kernel-irqchip=off: WHPX initialization failed",
                                   ETUDE_ACCEL_WHPX),
          "configured whpx refused");
    CHECK(!etude_qemu_accel_refused("qemu-system-x86_64: -accel kvm: failed\n", ETUDE_ACCEL_HVF),
          "another accelerator's refusal");
    CHECK(!etude_qemu_accel_refused("qemu-system-i386: -cpu bogus: unknown\n", ETUDE_ACCEL_TCG),
          "another option's error");
    CHECK(!etude_qemu_accel_refused(": -accel tcg: at the start of a line\n", ETUDE_ACCEL_TCG),
          "no program before the option");
}

/** Check the command lines of profile `p` under each firmware. */
static void test_firmware(const struct etude_qemu_profile *p, struct etude_qemu_site *site)
{
    static const char *const options[] = {"rach2.fsdisk", "quiet=1", NULL};
    struct etude_qemu_profile b;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];

    b = *p;
    b.firmware = ETUDE_FIRMWARE_OVMF;
    b.initrd = NULL;
    b.boot_options = NULL;
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == 0 && strcmp(argv[14], "-drive") == 0 &&
              strcmp(argv[15], "if=pflash,format=raw,readonly=on,file=/f/ov,,mf") == 0 &&
              strcmp(argv[16], "-drive") == 0,
          "ovmf");
    b.cpu = "max-nopdpe1gb";
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == 0 && strcmp(argv[8], "-cpu") == 0 &&
              strcmp(argv[9], "max,rdrand=on,rdseed=on,xsave=on,avx=on,"
                              "pdpe1gb=off,enforce=on") == 0,
          "a profile with present and absent features");
    b.cpu = p->cpu;
    b.architecture = "i686";
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == 0 && strcmp(argv[0], "/q/x86_64") == 0 &&
              has_arg(argv, "-drive", "if=pflash,format=raw,readonly=on,file=/f/ov,,mf"),
          "i686 on x86-64 ovmf");
    site->ovmf_i686 = "/f/i686";
    site->ovmf_x86_64 = NULL;
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == ENOENT, "no x86-64 ovmf for i686");
    site->ovmf_x86_64 = "/f/ov,mf";
    b.firmware = ETUDE_FIRMWARE_SEABIOS;
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == 0 && strcmp(argv[0], "/q/i686") == 0 &&
              strcmp(argv[14], "-drive") == 0,
          "seabios");
    b.boot_options = options;
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == EINVAL, "seabios with a command line");
    b.boot_options = NULL;
    b.disks = NULL;
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == EINVAL, "seabios without a disk");
}

/**
 * Check an SMBIOS vendor's argument, its commas doubled, and the vendors
 * refused. `p` has the direct firmware and no vendor.
 */
static void test_smbios(const struct etude_qemu_profile *p, const struct etude_qemu_site *site)
{
    static const char *const refused[][2] = {
        {"", "an empty smbios vendor"},
        {"Insyde\tCorp.", "a control character in the smbios vendor"},
        {"Insyde \303\251", "a vendor not in ascii"},
        {"Insyde Corp. Insyde Corp. Insyde Corp. Insyde Corp. Insyde Corp.1",
         "an smbios vendor of 65 bytes"}};
    struct etude_qemu_profile b = *p;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];
    size_t i;

    b.smbios_vendor = "Insyde Corp., v2";
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == 0 && strcmp(argv[14], "-smbios") == 0 &&
              strcmp(argv[15], "type=0,vendor=Insyde Corp.,, v2") == 0 &&
              strcmp(argv[16], "-kernel") == 0,
          "an smbios vendor");
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        b.smbios_vendor = refused[i][0];
        CHECK(make(&b, site, argv, buf, sizeof(buf)) == EINVAL, refused[i][1]);
    }
    b.smbios_vendor = "Insyde Corp. Insyde Corp. Insyde Corp. Insyde Corp. Insyde Corp.";
    CHECK(make(&b, site, argv, buf, sizeof(buf)) == 0, "an smbios vendor of 64 bytes");
}

/** Check the command lines of profiles, and the profiles that are refused. */
static void test_argv(void)
{
    static const struct etude_qemu_disk disks[] = {
        {"/d/one,two.img", ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 1},
        {"/d/big,img", ETUDE_BUS_NVME, 0, "rach2big", 1, NULL, 0, 0, 0},
        {"/d/b.img", ETUDE_BUS_IDE, 0, NULL, 1, NULL, 0, 0, 0},
        {"/d/grow,1.qcow2", ETUDE_BUS_IDE, 0, "GROWTEST", 1, NULL, 0, 1, 0},
        {"/d/r.iso", ETUDE_BUS_IDE, 1, NULL, 0, NULL, 0, 0, 0},
        {"/d/n.qcow2", ETUDE_BUS_NVME, 0, "rach2.n-1", 0, NULL, 0, 1, 1},
        {NULL, ETUDE_BUS_IDE, 0, NULL, 0, NULL, 0, 0, 0}};
    static const char *const options[] = {"rach2.fsdisk", "quiet=1", NULL};
    static const char *const want =
        "/q/x86_64 -accel kvm -nodefaults -display none -no-reboot -S "
        "-cpu qemu64,rdrand=off,rdseed=off,xsave=off,avx=off -smp 2 "
        "-m 268435456B -kernel /k/kernel -initrd /k/test.krffs "
        "-drive file=/d/one,,two.img,format=raw,if=ide,index=0,media=disk,cache=unsafe "
        "-drive file=/d/big,,img,format=raw,if=none,id=etude-disk1,discard=unmap,"
        "detect-zeroes=unmap -device nvme,drive=etude-disk1,serial=rach2big "
        "-drive file=/d/b.img,format=raw,if=ide,index=1,media=disk,discard=unmap,"
        "detect-zeroes=unmap "
        "-drive file=/d/grow,,1.qcow2,format=qcow2,if=none,id=etude-disk3,discard=unmap,"
        "detect-zeroes=unmap -device ide-hd,drive=etude-disk3,bus=ide.1,unit=0,serial=GROWTEST "
        "-drive file=/d/r.iso,format=raw,if=ide,index=3,media=cdrom,readonly=on "
        "-drive file=/d/n.qcow2,format=qcow2,if=none,id=etude-disk5,cache=unsafe "
        "-device nvme,drive=etude-disk5,serial=rach2.n-1 "
        "-netdev user,id=net0 -device rtl8139,netdev=net0 -netdev user,id=net1 "
        "-device e1000,netdev=net1 -vga std -device ahci " TEST_CHANNELS
        "-append rach2.fsdisk quiet=1";
    struct etude_qemu_site site = {ETUDE_ACCEL_KVM, "/q/i686", "/q/x86_64", NULL, "/f/ov,mf", 0};
    struct etude_qemu_profile p;
    struct etude_qemu_profile b;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096], line[4096];
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
    site.accel = ETUDE_ACCEL_WHPX;
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0 &&
              strcmp(argv[2], "whpx,kernel-irqchip=off") == 0,
          "whpx uses QEMU interrupt-controller emulation");
    site.accel = ETUDE_ACCEL_KVM;
    CHECK(make_in(&p, &site, argv, 10, buf, sizeof(buf)) == E2BIG, "few arguments");
    CHECK(make_in(&p, &site, argv, ETUDE_QEMU_ARGS_MAX, buf, 100) == E2BIG, "few bytes");
    test_firmware(&p, &site);
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
    REFUSED(kernel, NULL, "direct without a kernel");
    REFUSED(firmware, (enum etude_firmware)7, "an unknown firmware");
    test_accel_names();
    test_smbios(&p, &site);
    test_disks(&p, &site);
    test_disk_lines(&p, &site);
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
        {.model = "rtl8139",
         .net = "192.168.90.0/24",
         .host = "192.168.90.2",
         .dhcpstart = "192.168.90.20",
         .dns = "192.168.90.3",
         .mac = "52:54:00:12:34:56"},
        {.model = "e1000", .net = "10.0.3.0/24", .mac = "52:54:00:12:34:58"},
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
    struct etude_qemu_device devices[] = {{.model = "e1000"}, {.model = "rtl8139"}, {0}};
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

/**
 * Check that the launcher makes a command line of profile `p` on `site`, and
 * that its arguments, joined by spaces, have `want` among them; `row` names
 * the check.
 */
static void has(const struct etude_qemu_profile *p, const struct etude_qemu_site *site,
                const char *want, const char *row)
{
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096], line[4096];
    size_t used = 0;
    int i;

    line[0] = '\0';
    if (make(p, site, argv, buf, sizeof(buf)) != 0) {
        printf("etude-qemu-test: fail: %s: the launcher refuses the profile\n", row);
        fails++;
        return;
    }
    for (i = 0; argv[i] != NULL && used < sizeof(line); i++)
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s", i ? " " : "", argv[i]);
    if (strstr(line, want) == NULL) {
        printf("etude-qemu-test: fail: %s: the command line is\n  %s\nwithout\n  %s\n", row, line,
               want);
        fails++;
    }
}

/**
 * Check the forwards of the case's local ports to the guest on the user
 * networks of a profile's cards, with a card's network options, and the
 * forwards the launcher refuses.
 */
static void test_forwards(void)
{
    static const unsigned ports[] = {40001, 40002, 1023, 40003};
    static const char *const want =
        "-netdev user,id=net0,hostfwd=tcp:127.0.0.1:40001-:22,"
        "hostfwd=udp:127.0.0.1:40001-:9999,hostfwd=udp:127.0.0.1:40002-:7502 "
        "-device rtl8139,netdev=net0 -netdev user,id=net1 -device e1000,netdev=net1 "
        "-chardev";
    struct etude_qemu_site site = {ETUDE_ACCEL_TCG, "/q/i686", "/q/x86_64", NULL, NULL, 0};
    struct etude_qemu_forward fw[3] = {{0, 0, 22}, {1, 0, 9999}, {1, 1, 7502}};
    struct etude_qemu_forward bad;
    struct etude_qemu_device devices[] = {
        {.model = "vga"}, {.model = "rtl8139"}, {.model = "e1000"}, {0}};
    struct etude_qemu_profile p;
    struct etude_qemu_profile b;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];

    memset(&p, 0, sizeof(p));
    devices[1].forwards = fw;
    devices[1].forward_count = 3;
    p.architecture = "i686";
    p.kernel = "/k/kernel";
    p.cpu = "max";
    p.processors = 1;
    p.memory_bytes = 128u << 20;
    p.firmware = ETUDE_FIRMWARE_DIRECT;
    p.devices = devices;
    p.ports = ports;
    p.port_count = 2;
    has(&p, &site, want, "forwards");
    devices[1].net = "192.168.90.0/24";
    devices[1].dhcpstart = "192.168.90.20";
    devices[1].mac = "52:54:00:12:34:60";
    has(&p, &site,
        "-netdev user,id=net0,net=192.168.90.0/24,dhcpstart=192.168.90.20,"
        "hostfwd=tcp:127.0.0.1:40001-:22,hostfwd=udp:127.0.0.1:40001-:9999,"
        "hostfwd=udp:127.0.0.1:40002-:7502 -device rtl8139,netdev=net0,mac=52:54:00:12:34:60 ",
        "a card's network options and its forwards");
    devices[1].net = devices[1].dhcpstart = devices[1].mac = NULL;
#define NET_REFUSED(change, row)                                       \
    do {                                                               \
        b = p;                                                         \
        change;                                                        \
        CHECK(make(&b, &site, argv, buf, sizeof(buf)) == EINVAL, row); \
    } while (0)
    bad = fw[0];
    bad.port = 1;
    devices[0].forwards = &bad;
    devices[0].forward_count = 1;
    NET_REFUSED((void)0, "a forward on a device that is no network card");
    devices[0].forwards = NULL;
    devices[0].forward_count = 0;
    devices[2].forward_count = 1;
    NET_REFUSED((void)0, "forwards without their list");
    devices[2].forwards = &bad;
    bad = fw[0];
    bad.port = 2;
    NET_REFUSED(b.port_count = 3, "a forward of a port below 1024");
    bad.port = 3;
    NET_REFUSED((void)0, "a forward of a port the case does not hold");
    NET_REFUSED(b.ports = NULL, "a forward without the case's ports");
    bad.port = 1;
    bad.guest_port = 0;
    NET_REFUSED((void)0, "a forward to the guest's port 0");
    bad.guest_port = 65536;
    NET_REFUSED((void)0, "a forward to the guest's port 65536");
    bad = fw[0];
    NET_REFUSED((void)0, "a TCP port forwarded twice");
    bad = fw[2];
    NET_REFUSED((void)0, "a UDP port forwarded twice");
    bad.udp = 0;
    has(&p, &site, "-netdev user,id=net1,hostfwd=tcp:127.0.0.1:40002-:7502 -device e1000",
        "a forward on the second card");
#undef NET_REFUSED
}

/**
 * Require etude_qemu_guest_address to judge each address of `list`, ended by
 * NULL, on the network of card `d` as `want` says; `row` names the check.
 */
static void guest_addresses(const struct etude_qemu_device *d, const char *const *list, int want,
                            const char *row)
{
    size_t i;

    for (i = 0; list[i] != NULL; i++)
        if (!etude_qemu_guest_address(d, list[i]) != !want) {
            printf("etude-qemu-test: fail: %s: %s\n", row, list[i]);
            fails++;
        }
}

/**
 * Check the addresses QEMU can forward from: on the default network, on a
 * card's network with its own host, DHCP and name server addresses, and on a
 * wide network with QEMU's defaults for them. A card whose host is outside its
 * network, and a network with host bits set, have none.
 */
static void test_guest_addresses(void)
{
    static const char *const refused[] = {"10.0.2.0",   "10.0.2.2",  "10.0.2.3",   "10.0.2.15",
                                          "10.0.2.22",  "10.0.2.30", "10.0.2.255", "10.0.3.100",
                                          "10.0.2.256", "10.0.2.04", "10.0.2",     "",
                                          NULL};
    static const char *const taken[] = {"10.0.2.1",   "10.0.2.4",   "10.0.2.14", "10.0.2.31",
                                        "10.0.2.100", "10.0.2.254", NULL};
    static const char *const card_refused[] = {"192.168.90.1",
                                               "192.168.90.40",
                                               "192.168.90.50",
                                               "192.168.90.65",
                                               "192.168.90.255",
                                               "10.0.2.100",
                                               NULL};
    static const char *const card_taken[] = {"192.168.90.2",  "192.168.90.3",  "192.168.90.15",
                                             "192.168.90.49", "192.168.90.66", NULL};
    static const char *const wide_refused[] = {"10.90.2.2", "10.90.2.3", "10.90.2.15", "10.90.2.30",
                                               NULL};
    static const char *const wide_taken[] = {"10.90.0.2", "10.90.0.15", "10.90.2.31", NULL};
    struct etude_qemu_device d = {.model = "rtl8139"};
    struct etude_qemu_device card = {.model = "e1000",
                                     .net = "192.168.90.0/24",
                                     .host = "192.168.90.1",
                                     .dhcpstart = "192.168.90.50",
                                     .dns = "192.168.90.40"};
    struct etude_qemu_device wide = {.model = "e1000", .net = "10.90.0.0/16"};

    guest_addresses(&d, refused, 0, "an address QEMU cannot forward");
    guest_addresses(&d, taken, 1, "an address QEMU can forward");
    guest_addresses(&card, card_refused, 0, "an address of a card's network QEMU cannot forward");
    guest_addresses(&card, card_taken, 1, "an address of a card's network QEMU can forward");
    guest_addresses(&wide, wide_refused, 0, "an address of a wide network QEMU cannot forward");
    guest_addresses(&wide, wide_taken, 1, "an address of a wide network QEMU can forward");
    card.host = "10.0.2.2";
    CHECK(!etude_qemu_guest_address(&card, "192.168.90.100"),
          "an address of a card whose host is outside its network");
    card.host = "192.168.90.1";
    card.net = "192.168.90.1/24";
    CHECK(!etude_qemu_guest_address(&card, "192.168.90.100"), "an address of no network");
}

/**
 * Check a restricted network with guest forwards: the relays' words quoted
 * for QEMU and for its shell parser, the guest forwards the launcher refuses,
 * and a line that the smallest buffer that takes it has whole.
 */
static void test_guest_forwards(void)
{
    static const unsigned ports[] = {40001, 40002, 40003, 1023};
    static const char *const relay[] = {"/r/relay,one", "it's", NULL};
    static const char *const two[] = {"/r/two", NULL};
    static const char *const none[] = {NULL};
    static const char *const empty[] = {"", NULL};
    static const char *const newline[] = {"/r/two", "a\nb", NULL};
    static const char *const del[] = {"/r/two", "a\177b", NULL};
    static const char *const want =
        "-netdev user,id=net0,restrict=on,hostfwd=tcp:127.0.0.1:40001-:22,"
        "hostfwd=udp:127.0.0.1:40002-:9999,"
        "guestfwd=tcp:10.0.2.100:443-cmd:'/r/relay,,one' 'it'\\''s' 40002,"
        "guestfwd=tcp:10.0.2.100:80-cmd:'/r/two' 40003 -device rtl8139,netdev=net0 -chardev";
    struct etude_qemu_site site = {ETUDE_ACCEL_TCG, "/q/i686", "/q/x86_64", NULL, NULL, 0};
    struct etude_qemu_forward fw[2] = {{0, 0, 22}, {1, 1, 9999}};
    struct etude_qemu_guest_forward gf[2] = {{"10.0.2.100", 443, 1, relay},
                                             {"10.0.2.100", 80, 2, two}};
    struct etude_qemu_device devices[] = {{.model = "rtl8139",
                                           .forwards = fw,
                                           .forward_count = 2,
                                           .restricted = 1,
                                           .guest_forwards = gf,
                                           .guest_forward_count = 2},
                                          {0}};
    struct etude_qemu_profile p, b;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096], *small[ETUDE_QEMU_ARGS_MAX], *tight;
    size_t i, size;
    int status = -1;

    memset(&p, 0, sizeof(p));
    p.architecture = "i686";
    p.kernel = "/k/kernel";
    p.cpu = "max";
    p.processors = 1;
    p.memory_bytes = 128u << 20;
    p.firmware = ETUDE_FIRMWARE_DIRECT;
    p.devices = devices;
    p.ports = ports;
    p.port_count = 3;
    has(&p, &site, want, "guest forwards");
#define GUEST_REFUSED(change, row)                                     \
    do {                                                               \
        gf[1].address = "10.0.2.100";                                  \
        gf[1].guest_port = 80;                                         \
        gf[1].port = 2;                                                \
        gf[1].relay = two;                                             \
        b = p;                                                         \
        change;                                                        \
        CHECK(make(&b, &site, argv, buf, sizeof(buf)) == EINVAL, row); \
    } while (0)
    GUEST_REFUSED(gf[1].address = "10.0.2.2", "a guest forward from the host's address");
    GUEST_REFUSED(gf[1].address = "10.0.2.20", "a guest forward from an address DHCP gives");
    GUEST_REFUSED(gf[1].address = NULL, "a guest forward without an address");
    GUEST_REFUSED(gf[1].guest_port = 0, "a guest forward from the guest's port 0");
    GUEST_REFUSED(gf[1].guest_port = 65536, "a guest forward from the guest's port 65536");
    GUEST_REFUSED(gf[1].port = 3, "a guest forward to a port the case does not hold");
    GUEST_REFUSED(gf[1].port = 3; b.port_count = 4, "a guest forward to a port below 1024");
    GUEST_REFUSED(gf[1].port = 0, "a guest forward to a port QEMU listens on");
    GUEST_REFUSED(gf[1].relay = NULL, "a guest forward without a relay");
    GUEST_REFUSED(gf[1].relay = none, "a relay without a program");
    GUEST_REFUSED(gf[1].relay = empty, "a relay whose program is empty");
    GUEST_REFUSED(gf[1].relay = newline, "a relay with a control character");
    GUEST_REFUSED(gf[1].relay = del, "a relay with a delete character");
    GUEST_REFUSED(gf[1].guest_port = 443, "connections to one address and port forwarded twice");
    GUEST_REFUSED(devices[0].guest_forwards = NULL, "guest forwards without their list");
    devices[0].guest_forwards = gf;
    devices[0].model = "vga";
    devices[0].forward_count = 0;
    GUEST_REFUSED(devices[0].guest_forward_count = 0,
                  "a restriction on a device that is no network card");
    devices[0].guest_forward_count = 2;
    GUEST_REFUSED(devices[0].restricted = 0, "a guest forward on a device that is no network card");
    devices[0].restricted = 1;
    devices[0].forward_count = 2;
    devices[0].model = "rtl8139";
#undef GUEST_REFUSED
    fw[1].udp = 0;
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == EINVAL,
          "a guest forward to a port forwarded to the guest by TCP");
    fw[1].udp = 1;
    gf[1].address = "10.0.2.101";
    gf[1].guest_port = 443;
    has(&p, &site, "guestfwd=tcp:10.0.2.101:443-cmd:'/r/two' 40003 ",
        "guest forwards of one guest port from two addresses");
    for (size = 1; size <= sizeof(buf); size++) {
        tight = malloc(size);
        if (tight == NULL)
            break;
        status = make(&p, &site, small, tight, size);
        if (status == 0) {
            make(&p, &site, argv, buf, sizeof(buf));
            for (i = 0; argv[i] != NULL && small[i] != NULL; i++)
                if (strcmp(argv[i], small[i]) != 0)
                    break;
            CHECK(argv[i] == NULL && small[i] == NULL,
                  "the smallest buffer that takes the line has it whole");
        }
        free(tight);
        if (status != E2BIG)
            break;
    }
    CHECK(status == 0, "a buffer too small has E2BIG, and a large one takes the line");
}

/**
 * Check one card with every option of its user network in its one
 * `-netdev`: its own network, host, DHCP and name server addresses, the
 * restriction, forwards of the case's ports by TCP and by UDP, and guest
 * forwards from addresses of its network, with its station address on its
 * `-device`.
 */
static void test_card(void)
{
    static const unsigned ports[] = {40001, 40002, 40003};
    static const char *const relay[] = {"/r/relay", NULL};
    static const char *const want =
        "-netdev user,id=net0,net=192.168.90.0/24,host=192.168.90.1,dhcpstart=192.168.90.50,"
        "dns=192.168.90.40,restrict=on,hostfwd=tcp:127.0.0.1:40001-:22,"
        "hostfwd=udp:127.0.0.1:40002-:9999,"
        "guestfwd=tcp:192.168.90.100:443-cmd:'/r/relay' 40003,"
        "guestfwd=tcp:192.168.90.2:80-cmd:'/r/relay' 40003 "
        "-device e1000,netdev=net0,mac=52:54:00:12:34:60 -chardev";
    struct etude_qemu_site site = {ETUDE_ACCEL_TCG, "/q/i686", "/q/x86_64", NULL, NULL, 0};
    struct etude_qemu_forward fw[2] = {{0, 0, 22}, {1, 1, 9999}};
    struct etude_qemu_guest_forward gf[2] = {{"192.168.90.100", 443, 2, relay},
                                             {"192.168.90.2", 80, 2, relay}};
    struct etude_qemu_device devices[] = {{.model = "e1000",
                                           .net = "192.168.90.0/24",
                                           .host = "192.168.90.1",
                                           .dhcpstart = "192.168.90.50",
                                           .dns = "192.168.90.40",
                                           .mac = "52:54:00:12:34:60",
                                           .forwards = fw,
                                           .forward_count = 2,
                                           .restricted = 1,
                                           .guest_forwards = gf,
                                           .guest_forward_count = 2},
                                          {0}};
    struct etude_qemu_profile p;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];

    memset(&p, 0, sizeof(p));
    p.architecture = "x86_64";
    p.kernel = "/k/kernel";
    p.cpu = "max";
    p.processors = 1;
    p.memory_bytes = 128u << 20;
    p.firmware = ETUDE_FIRMWARE_DIRECT;
    p.devices = devices;
    p.ports = ports;
    p.port_count = 3;
    has(&p, &site, want, "a card with every option");
    gf[1].address = "10.0.2.100";
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == EINVAL,
          "a guest forward from outside the card's network");
}

/**
 * Check cards on hubs of their own beside a card on a user network: each hub
 * card's `-netdev` is a port of the hub of its own index, with its station
 * address and its capture as on a user network. A hub card with an option of
 * a user network, and a hub on a device that is no network card, are refused,
 * each taken once the hub is removed, and a hub card has no address of a
 * guest forward.
 */
static void test_hubs(void)
{
    static const unsigned ports[] = {40001};
    static const char *const relay[] = {"/r/relay", NULL};
    static const char *const want =
        "-netdev user,id=net0,net=10.0.3.0/24 -device rtl8139,netdev=net0 "
        "-netdev hubport,id=net1,hubid=1 -device e1000,netdev=net1,mac=52:54:00:12:34:60 "
        "-object filter-dump,id=net1-dump,netdev=net1,file=/w/hub.pcap "
        "-netdev hubport,id=net2,hubid=2 -device rtl8139,netdev=net2 -chardev";
    static const struct etude_qemu_capture captures[] = {{1, "/w/hub.pcap"}, {0, NULL}};
    struct etude_qemu_site site = {ETUDE_ACCEL_TCG, "/q/i686", "/q/x86_64", NULL, NULL, 0};
    struct etude_qemu_forward fw = {0, 0, 22};
    struct etude_qemu_guest_forward gf = {"10.0.2.100", 443, 0, relay};
    struct etude_qemu_device devices[] = {{.model = "rtl8139", .net = "10.0.3.0/24"},
                                          {.model = "e1000", .mac = "52:54:00:12:34:60", .hub = 1},
                                          {.model = "rtl8139", .hub = 1},
                                          {0}};
    struct etude_qemu_device *d = &devices[1];
    struct etude_qemu_profile p;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];

    memset(&p, 0, sizeof(p));
    p.architecture = "i686";
    p.kernel = "/k/kernel";
    p.cpu = "max";
    p.processors = 1;
    p.memory_bytes = 128u << 20;
    p.firmware = ETUDE_FIRMWARE_DIRECT;
    p.devices = devices;
    p.ports = ports;
    p.port_count = 1;
    p.captures = captures;
    has(&p, &site, want, "cards on hubs of their own");
#define HUB_REFUSED(change, undo, row)                                                     \
    do {                                                                                   \
        change;                                                                            \
        d->hub = 0;                                                                        \
        CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, row " taken without the hub"); \
        d->hub = 1;                                                                        \
        CHECK(make(&p, &site, argv, buf, sizeof(buf)) == EINVAL, row);                     \
        undo;                                                                              \
    } while (0)
    HUB_REFUSED(d->net = "10.0.4.0/24", d->net = NULL, "a hub card with a network");
    HUB_REFUSED(d->host = "10.0.2.2", d->host = NULL, "a hub card with a host address");
    HUB_REFUSED(d->dhcpstart = "10.0.2.20", d->dhcpstart = NULL, "a hub card with a DHCP address");
    HUB_REFUSED(d->dns = "10.0.2.3", d->dns = NULL, "a hub card with a name server address");
    HUB_REFUSED(d->restricted = 1, d->restricted = 0, "a hub card with the restriction");
    HUB_REFUSED((d->forwards = &fw, d->forward_count = 1),
                (d->forwards = NULL, d->forward_count = 0), "a hub card with a forward");
    HUB_REFUSED((d->guest_forwards = &gf, d->guest_forward_count = 1),
                (d->guest_forwards = NULL, d->guest_forward_count = 0),
                "a hub card with a guest forward");
    HUB_REFUSED((d->model = "virtio-rng-pci", d->mac = NULL),
                (d->model = "e1000", d->mac = "52:54:00:12:34:60"),
                "a hub on a device that is no network card");
#undef HUB_REFUSED
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, "the hub cards are taken again");
    CHECK(!etude_qemu_guest_address(d, "10.0.2.100"), "an address of a guest forward on a hub");
    d->hub = 0;
    CHECK(etude_qemu_guest_address(d, "10.0.2.100"), "that address on the user network");
}

/** Give the command line of `argv` as one line, its arguments separated by
 * spaces, in the `size` bytes of `line`. */
static const char *joined(char *const *argv, char *line, size_t size)
{
    size_t used = 0;
    int i;

    line[0] = '\0';
    for (i = 0; argv[i] != NULL && used < size; i++)
        used += (size_t)snprintf(line + used, size - used, "%s%s", i ? " " : "", argv[i]);
    return line;
}

/* Require test_usb's profile to be refused with `field` of disk `at` set to
 * `value`. */
#define DISK_REFUSED(at, field, value, row)                            \
    do {                                                               \
        kept = disks[at];                                              \
        disks[at].field = value;                                       \
        CHECK(make(&p, &site, argv, buf, sizeof(buf)) == EINVAL, row); \
        disks[at] = kept;                                              \
    } while (0)

/**
 * Check the command line of a machine of USB devices, a hub and two disks on
 * them, without the 8042, and refuse each device whose bus, port, drive,
 * identity or properties QEMU could not take or the profile does not have.
 */
static void test_usb(void)
{
    static const char *const boot[] = {"rach2.usbdisk", NULL};
    static const char *const msi[] = {"msi", "on", NULL};
    static const char *const index0[] = {"bootindex", "0", NULL};
    static const char *const comma[] = {"serial", "a,b", NULL};
    static const char *const bus[] = {"bus", "xhci.0", NULL};
    static const char *const mac[] = {"mac", "52:54:00:12:34:56", NULL};
    static const char *const odd[] = {"msi", NULL};
    static const char *const want =
        "/q/i686 -accel tcg -nodefaults -display none -no-reboot -S -cpu qemu64,"
        "rdrand=off,rdseed=off,xsave=off,avx=off -smp 1 -m 268435456B -machine i8042=off "
        "-kernel /k/kernel -initrd /k/test.krffs "
        "-drive file=/d/stick,,one.img,format=raw,if=none,id=stick "
        "-drive file=/d/lun1.img,format=raw,if=none,id=lun1,discard=unmap,detect-zeroes=unmap,"
        "cache=unsafe "
        "-device qemu-xhci,id=xhci,msi=on -device usb-hub,id=hub1,bus=xhci.0,port=1 "
        "-device usb-kbd,bus=xhci.0,port=1.2 "
        "-device usb-storage,bus=xhci.0,drive=stick,bootindex=0 "
        "-device usb-bot,id=bot,bus=xhci.0 -device scsi-hd,bus=bot.0,drive=lun1 "
        "-netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:58,id=nic " TEST_CHANNELS
        "-append rach2.usbdisk";
    struct etude_qemu_disk disks[] = {
        {.path = "/d/stick,one.img", .bus = ETUDE_BUS_DEVICE, .id = "stick"},
        {.path = "/d/lun1.img", .bus = ETUDE_BUS_DEVICE, .id = "lun1", .discard = 1, .unsafe = 1},
        {.path = NULL}};
    struct etude_qemu_device d[] = {
        {.model = "qemu-xhci", .id = "xhci", .properties = msi},
        {.model = "usb-hub", .id = "hub1", .bus = "xhci.0", .port = "1"},
        {.model = "usb-kbd", .bus = "xhci.0", .port = "1.2"},
        {.model = "usb-storage", .bus = "xhci.0", .drive = "stick", .properties = index0},
        {.model = "usb-bot", .id = "bot", .bus = "xhci.0"},
        {.model = "scsi-hd", .bus = "bot.0", .drive = "lun1"},
        {.model = "e1000", .id = "nic", .mac = "52:54:00:12:34:58"},
        {.model = NULL}};
    struct etude_qemu_device keep;
    struct etude_qemu_disk kept;
    struct etude_qemu_site site = {ETUDE_ACCEL_TCG, "/q/i686", "/q/x86_64", NULL, NULL, 0};
    struct etude_qemu_profile p, b;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096], line[4096];

    memset(&p, 0, sizeof(p));
    p.architecture = "i686";
    p.kernel = "/k/kernel";
    p.initrd = "/k/test.krffs";
    p.disks = disks;
    p.cpu = "qemu64";
    p.processors = 1;
    p.memory_bytes = 256u << 20;
    p.firmware = ETUDE_FIRMWARE_DIRECT;
    p.devices = d;
    p.boot_options = boot;
    p.no_i8042 = 1;
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, "usb argv");
    if (strcmp(joined(argv, line, sizeof(line)), want) != 0) {
        printf("etude-qemu-test: fail: the usb command line is\n  %s\nnot\n  %s\n", line, want);
        fails++;
    }
    b = p;
    b.firmware = ETUDE_FIRMWARE_SEABIOS;
    b.initrd = NULL;
    b.boot_options = NULL;
    CHECK(make(&b, &site, argv, buf, sizeof(buf)) == 0, "seabios from disks on devices alone");
    disks[1].boot = 1;
    CHECK(make(&b, &site, argv, buf, sizeof(buf)) == EINVAL, "a disk on a device that boots first");
    disks[1].boot = 0;
    DISK_REFUSED(1, id, "stick", "a disk's identity given twice");
    DISK_REFUSED(1, id, "lun.1", "a disk's identity with a dot");
    DISK_REFUSED(1, id, NULL, "a disk on a device without an identity");
    DISK_REFUSED(1, bus, ETUDE_BUS_IDE, "an identity of a disk on IDE");
    DISK_REFUSED(1, cdrom, 1, "a CD on a device");
    DISK_REFUSED(1, serial, "S1", "a serial of a disk on a device");
#define DEVICE_REFUSED(at, field, value, row)                          \
    do {                                                               \
        keep = d[at];                                                  \
        d[at].field = value;                                           \
        CHECK(make(&p, &site, argv, buf, sizeof(buf)) == EINVAL, row); \
        d[at] = keep;                                                  \
    } while (0)
    DEVICE_REFUSED(1, bus, "xhcx.0", "a bus no device provides");
    DEVICE_REFUSED(0, bus, "hub1.0", "a bus of a later device");
    DEVICE_REFUSED(1, bus, "xhci", "a bus without its number");
    DEVICE_REFUSED(1, bus, "xhci.0a", "a bus of a number and more");
    DEVICE_REFUSED(1, port, "0", "port 0");
    DEVICE_REFUSED(1, port, "1..2", "a port of an empty number");
    DEVICE_REFUSED(1, port, "1.", "a port that ends in a dot");
    DEVICE_REFUSED(2, drive, "lun9", "a drive the profile lacks");
    DEVICE_REFUSED(2, drive, "stick", "a drive two devices attach");
    DEVICE_REFUSED(5, drive, NULL, "a disk on a device that no device attaches");
    DEVICE_REFUSED(2, id, "hub1", "a device's identity given twice");
    DEVICE_REFUSED(2, id, "1kbd", "an identity that begins with a digit");
    DEVICE_REFUSED(2, id, "k.bd", "an identity with a dot");
    DEVICE_REFUSED(4, model, "usb-bot,id=x", "a model with a comma");
    DEVICE_REFUSED(0, properties, comma, "a property value with a comma");
    DEVICE_REFUSED(0, properties, bus, "a property the launcher sets");
    DEVICE_REFUSED(0, properties, mac, "a station address as a property");
    DEVICE_REFUSED(0, properties, odd, "a property without its value");
    DEVICE_REFUSED(6, model, "vga", "vga with an identity");
#undef DISK_REFUSED
#undef DEVICE_REFUSED
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, "the profile is whole again");
}

/**
 * Check that a capture of a card is a `filter-dump` on its network after the
 * card, with its file's commas doubled, and that a capture of a card the
 * profile lacks, of a card twice and without a file are refused.
 */
static void test_captures(void)
{
    static const struct etude_qemu_device devices[] = {
        {.model = "rtl8139"}, {.model = "vga"}, {.model = "e1000"}, {0}};
    static const struct etude_qemu_device none[] = {{.model = "vga"}, {0}};
    static const struct etude_qemu_capture second[] = {{1, "/w/e1000,1.pcap"}, {0, NULL}};
    static const struct etude_qemu_capture third[] = {{2, "/w/c.pcap"}, {0, NULL}};
    static const struct etude_qemu_capture twice[] = {
        {0, "/w/a.pcap"}, {1, "/w/b.pcap"}, {0, "/w/c.pcap"}, {0, NULL}};
    static const struct etude_qemu_capture nameless[] = {{0, ""}, {0, NULL}};
    struct etude_qemu_site site = {ETUDE_ACCEL_TCG, "/q/i686", "/q/x86_64", NULL, NULL, 0};
    struct etude_qemu_profile p, b;
    char *argv[ETUDE_QEMU_ARGS_MAX], buf[4096];
    int i, at = -1, dumps = 0;

    memset(&p, 0, sizeof(p));
    p.architecture = "i686";
    p.kernel = "/k/kernel";
    p.cpu = "qemu64";
    p.processors = 1;
    p.memory_bytes = 128u << 20;
    p.firmware = ETUDE_FIRMWARE_DIRECT;
    p.devices = devices;
    p.captures = second;
    CHECK(make(&p, &site, argv, buf, sizeof(buf)) == 0, "a capture's argv");
    for (i = 0; argv[i] != NULL; i++) {
        if (strcmp(argv[i], "e1000,netdev=net1") == 0)
            at = i;
        dumps += strncmp(argv[i], "filter-dump", 11) == 0;
    }
    CHECK(at > 0 && argv[at + 1] != NULL && strcmp(argv[at + 1], "-object") == 0 &&
              argv[at + 2] != NULL &&
              strcmp(argv[at + 2], "filter-dump,id=net1-dump,netdev=net1,file=/w/e1000,,1.pcap") ==
                  0,
          "the second card's capture follows it");
    CHECK(dumps == 1, "one capture");
    REFUSED(captures, third, "a capture of a third card of two");
    REFUSED(captures, twice, "a card captured twice");
    REFUSED(captures, nameless, "a capture without a file");
    REFUSED(devices, none, "a capture of a profile without cards");
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
#if !defined(__APPLE__) && !defined(_WIN32)
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
    p.no_i8042 = 1;
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "boundary needs the 8042");
    p.no_i8042 = 0;
    p.q35 = 1;
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "boundary needs the PC");
    p.q35 = 0;
    p.boot_options = NULL;
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "boundary needs rescue boot");
    p.memory_profile = "file";
    CHECK(etude_qemu_memory(&p, &memory) == EINVAL, "unproved file backing refused");
    etude_qemu_free(q);
}

/**
 * A host attempt finishes through the launcher with no machine or descriptor.
 */
static void test_host_without_machine(void)
{
    struct etude_qemu *q = etude_qemu_new();
    struct etude_exec_spec spec;
    struct etude_proc *p = NULL, *done = NULL;
    char *args[] = {self, "--host-child", NULL};

    CHECK(q != NULL, "host launcher");
    if (q == NULL)
        return;
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = "host-without-machine";
    spec.argv = args;
    spec.envp = environ;
    spec.output_max = 1024;
    spec.grace_us = FAKE_GRACE_US;
    CHECK(etude_exec_start(etude_qemu_exec(q), &spec, &p) == 0, "host start");
    if (p != NULL) {
        while (done != p)
            etude_qemu_watch(q, -1, NULL, 0, &done);
        CHECK(done == p, "host result with no machine");
        if (done == p)
            CHECK(etude_proc_result(p)->end == ETUDE_END_EXITED &&
                      etude_proc_result(p)->value == 17,
                  "host exit status");
    }
    etude_qemu_free(q);
}

int main(int argc, char **argv)
{
    const char *fake = getenv("ETUDE_FAKE_QEMU");

    if (argc == 2 && strcmp(argv[1], "--host-child") == 0)
        return 17;
    if (fake != NULL && *fake != '\0')
        return fake_qemu(fake, argc, argv);
    if (host_executable(argv[0], self, sizeof(self)) != 0) {
        printf("etude-qemu-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    if (host_environment("TMPDIR", fix_dir()) != 0)
        return 1;
    if (argc == 2 && strcmp(argv[1], "--argv-only") == 0) {
        test_argv();
    } else if (argc == 2 && strcmp(argv[1], "--memory-only") == 0) {
        test_memory();
    } else if (argc == 2 && strcmp(argv[1], "--host-only") == 0) {
        test_host_without_machine();
    } else if (argc == 2 && strcmp(argv[1], "--boundary") == 0) {
        test_real(1);
    } else {
        test_argv();
        test_network_argv();
        test_network_refused();
        test_hubs();
        test_forwards();
        test_guest_addresses();
        test_guest_forwards();
        test_card();
        test_captures();
        test_audio_argv();
        test_audio();
        test_usb();
        test_memory();
        test_host_without_machine();
        test_ends();
        test_refused_starts();
        test_real(0);
        test_uefi();
    }
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_qemu_hosttest: pass\n");
    return 0;
}
