/*
 * lifecycle.c: the code the scenarios of tests/cases/machine/ share, over
 * libetude-case.
 */
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/wait.h>
#endif
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include "win/winexec.h"
#include "win/winfmt.h"
typedef intptr_t clock_socket;
typedef int socklen_t;
#else
typedef int clock_socket;
#endif

#include "history_json.h"
#include "lifecycle.h"

/* The room for the reason an assertion fails. */
#define WHY_LEN 160

/** Give the name of status `st`. */
static const char *status_name(enum etude_status st)
{
    switch (st) {
    case ETUDE_OK:
        return "ok";
    case ETUDE_FAILED:
        return "failed";
    case ETUDE_LOST:
        return "lost";
    case ETUDE_BOUND:
        return "past the step's bound";
    case ETUDE_STOPPED:
        return "stopped";
    case ETUDE_REFUSED:
        return "refused";
    }
    return "unknown";
}

/** Give the name of end `e`. */
static const char *end_name(enum etude_end e)
{
    switch (e) {
    case ETUDE_END_EXITED:
        return "exited";
    case ETUDE_END_SIGNALED:
        return "signaled";
    case ETUDE_END_INIT_EXITED:
        return "init_exited";
    case ETUDE_END_INIT_KILLED:
        return "init_killed";
    case ETUDE_END_PANIC:
        return "panic";
    case ETUDE_END_RESET:
        return "reset";
    case ETUDE_END_SHUTDOWN:
        return "shutdown";
    case ETUDE_END_HALTED:
        return "halted";
    case ETUDE_END_QEMU_EXITED:
        return "qemu_exited";
    case ETUDE_END_ACCEL_FAILED:
        return "accel_failed";
    case ETUDE_END_STOPPED:
        return "stopped";
    case ETUDE_END_LOST:
        return "lost";
    }
    return "unknown";
}

void lc_begin(struct lc *l, struct etude_case *c, const char *const ids[])
{
    size_t i;

    memset(l, 0, sizeof(*l));
    l->c = c;
    for (i = 0; i < LC_ASSERTS && ids[i] != NULL; i++)
        l->ids[i] = ids[i];
    if (ids[i] != NULL)
        fprintf(stderr, "lifecycle: %s: more than %d assertions\n", etude_case_id(c), LC_ASSERTS);
}

/**
 * Mark assertion `id` reported, and say whether it may be: it is declared
 * and was not reported before. A fault is said on standard error.
 */
static int lc_tell(struct lc *l, const char *id)
{
    size_t i;

    for (i = 0; l->ids[i] != NULL; i++)
        if (strcmp(l->ids[i], id) == 0)
            break;
    if (l->ids[i] == NULL || l->told[i]) {
        fprintf(stderr, "lifecycle: %s: %s is not declared or was reported\n", etude_case_id(l->c),
                id);
        return 0;
    }
    l->told[i] = 1;
    return 1;
}

void lc_assert(struct lc *l, const char *id, int held, const char *detail)
{
    if (lc_tell(l, id))
        etude_assert(l->c, id, held, held ? NULL : detail);
}

void lc_notrun(struct lc *l, const char *id, const char *reason)
{
    if (lc_tell(l, id))
        etude_notrun(l->c, id, reason);
}

int lc_ok(const struct lc *l)
{
    return l->why[0] == '\0';
}

void lc_halt(struct lc *l, enum etude_status st, const char *what)
{
    if (st == ETUDE_STOPPED)
        l->stopped = 1;
    if (l->why[0] == '\0')
        snprintf(l->why, sizeof(l->why), "%s: %s", what, status_name(st));
}

int lc_finish(struct lc *l)
{
    size_t i;

    for (i = 0; l->ids[i] != NULL; i++)
        if (!l->told[i]) {
            l->told[i] = 1;
            etude_notrun(l->c, l->ids[i], l->why[0] != '\0' ? l->why : "the scenario passed it by");
        }
    return l->stopped;
}

int lc_step(struct lc *l, const char *step)
{
    enum etude_status st;
    char what[64];

    if (!lc_ok(l))
        return 0;
    st = etude_step(l->c, step);
    if (st != ETUDE_OK) {
        snprintf(what, sizeof(what), "step %s", step);
        lc_halt(l, st, what);
    }
    return st == ETUDE_OK;
}

struct etude_case_machine *lc_boot(struct lc *l, const char *machine)
{
    struct etude_case_machine *m = NULL;
    struct etude_termination end;
    enum etude_status st;
    char what[96];

    if (!lc_ok(l))
        return NULL;
    end.end = ETUDE_END_LOST;
    st = etude_boot(l->c, machine, NULL, &m, &end);
    if (st == ETUDE_OK)
        return m;
    snprintf(what, sizeof(what), "the boot of %s, which ended %s", machine, end_name(end.end));
    lc_halt(l, st, what);
    return NULL;
}

int lc_wait(struct lc *l, struct etude_case_machine *m, const char *text, uint64_t *at,
            const char *id)
{
    enum etude_status st;
    char what[160];

    if (!lc_ok(l))
        return 0;
    st = etude_console_wait(m, text, *at, at);
    if (st == ETUDE_OK) {
        if (id != NULL)
            lc_assert(l, id, 1, NULL);
        return 1;
    }
    snprintf(what, sizeof(what), "the wait for '%s'", text);
    lc_halt(l, st, what);
    if (id != NULL && st != ETUDE_STOPPED)
        lc_assert(l, id, 0, l->why);
    return 0;
}

int lc_type(struct lc *l, struct etude_case_machine *m, const char *text)
{
    enum etude_status st;

    if (!lc_ok(l))
        return 0;
    st = etude_type(m, text);
    if (st != ETUDE_OK)
        lc_halt(l, st, "typing at the console");
    return st == ETUDE_OK;
}

void lc_find(struct lc *l, struct etude_case_machine *m, const char *id, const char *text,
             uint64_t from, int present)
{
    enum etude_status st;
    uint64_t at = 0;
    char detail[160];

    if (!lc_ok(l))
        return;
    st = etude_console_find(m, text, from, &at);
    if (st == ETUDE_OK)
        snprintf(detail, sizeof(detail), "the console says '%s'", text);
    else if (st == ETUDE_FAILED)
        snprintf(detail, sizeof(detail), "the console never says '%s'", text);
    else
        snprintf(detail, sizeof(detail), "the search for '%s' was %s", text, status_name(st));
    lc_assert(l, id, present ? st == ETUDE_OK : st == ETUDE_FAILED, detail);
}

int lc_text(struct lc *l, const char *console, const char *key, uint64_t from, char *buf,
            size_t size)
{
    const char *path = etude_evidence(l->c, console);
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    size_t n = 0, cap = 0, i = 0;
    char *text = NULL, *at, *grown;
    int found = 0;

    if (size > 0)
        buf[0] = '\0';
    if (f == NULL)
        return 0;
    do {
        if (n == cap) {
            cap = cap ? 2 * cap : 65536;
            if ((grown = realloc(text, cap + 1)) == NULL)
                break;
            text = grown;
        }
        n += fread(text + n, 1, cap - n, f);
    } while (n == cap && !feof(f) && !ferror(f));
    fclose(f);
    if (text == NULL)
        return 0;
    text[n] = '\0';
    if (from < n && (at = strstr(text + from, key)) != NULL) {
        found = 1;
        at += strlen(key);
        while (size > 0 && i < size - 1 && at[i] != '\0' && at[i] != '\r' && at[i] != '\n') {
            buf[i] = at[i];
            i++;
        }
        if (size > 0)
            buf[i] = '\0';
    }
    free(text);
    return found;
}

struct etude_case_machine *lc_shell(struct lc *l, const char *step, const char *machine,
                                    uint64_t *at)
{
    struct etude_case_machine *m;

    *at = 0;
    if (!lc_step(l, step) || (m = lc_boot(l, machine)) == NULL ||
        !lc_wait(l, m, "Type 'help'", at, NULL) || !lc_wait(l, m, "\n> ", at, NULL))
        return NULL;
    return m;
}

char *lc_console(struct lc *l, const char *machine, uint64_t from, uint64_t to)
{
    char name[64];
    const char *path;
    FILE *f;
    char *buf = NULL;
    size_t n = (size_t)(to - from);

    snprintf(name, sizeof(name), "%s.1.console", machine);
    path = etude_evidence(l->c, name);
    f = path != NULL && from <= to ? fopen(path, "rb") : NULL;
    if (f != NULL && fseek(f, (long)from, SEEK_SET) == 0 && (buf = malloc(n + 1)) != NULL &&
        fread(buf, 1, n, f) == n) {
        buf[n] = '\0';
    } else {
        free(buf);
        buf = NULL;
        lc_halt(l, ETUDE_FAILED, "the read of the console's evidence");
    }
    if (f != NULL)
        fclose(f);
    return buf;
}

size_t lc_past_prefix(const char *text, size_t len, size_t at)
{
    size_t i = at;

    if (i + 4 > len || text[i] != '<' || text[i + 2] != '>' || text[i + 3] != '[')
        return at;
    for (i += 4; i < len && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.'); i++) {
    }
    if (i + 2 > len || text[i] != ']' || text[i + 1] != ' ')
        return at;
    return i + 2;
}

/* The records that hand the debug shell back: an exit with its status, or a
 * death by a signal. */
static const char exit_head[] = "sys_exit: init process ";
static const char exit_mid[] = " exited with status ";
static const char kill_head[] = "terminate: init process ";
static const char kill_mid[] = " killed by signal ";
static const char back[] = ", returning to kernel shell";

/**
 * Say whether `head`, a pid, `mid`, a number and `back` are whole at offset
 * `at` of the `len` bytes of `text`, giving the number, which may have a
 * minus sign, in `*value`.
 */
static int marker_at(const char *text, size_t len, size_t at, const char *head, const char *mid,
                     long *value)
{
    size_t i = at, hlen = strlen(head), mlen = strlen(mid), blen = strlen(back), start;
    long sign = 1, n = 0;

    if (i + hlen > len || memcmp(text + i, head, hlen) != 0)
        return 0;
    for (i += hlen, start = i; i < len && text[i] >= '0' && text[i] <= '9'; i++) {
    }
    if (i == start || i + mlen > len || memcmp(text + i, mid, mlen) != 0)
        return 0;
    i += mlen;
    if (i < len && text[i] == '-') {
        sign = -1;
        i++;
    }
    for (start = i; i < len && text[i] >= '0' && text[i] <= '9' && i - start < 9; i++)
        n = n * 10 + (text[i] - '0');
    if (i == start || i + blen > len || memcmp(text + i, back, blen) != 0)
        return 0;
    *value = sign * n;
    return 1;
}

/**
 * Judge the `len` bytes of window `w` as lc_run says, putting what decided
 * it in `why`. @return 1 when the program exited with status 0.
 */
static int score_window(const char *w, size_t len, char *why, size_t size)
{
    size_t at;
    long value;
    int pass;

    for (pass = 0; pass < 2; pass++)
        for (at = 0; at < len; at++) {
            if (at != 0 && w[at - 1] != '\n')
                continue;
            if (pass == 0 &&
                marker_at(w, len, lc_past_prefix(w, len, at), kill_head, kill_mid, &value)) {
                snprintf(why, size, "the program was killed by signal %ld", value);
                return 0;
            }
            if (pass == 1 &&
                marker_at(w, len, lc_past_prefix(w, len, at), exit_head, exit_mid, &value)) {
                snprintf(why, size, "the program exited with status %ld", value);
                return value == 0;
            }
        }
    snprintf(why, size, "the kernel reported no exit of the program");
    return 0;
}

char *lc_handback(struct lc *l, struct etude_case_machine *m, const char *machine, uint64_t from,
                  uint64_t *at, const char *id)
{
    char why[WHY_LEN], *w;

    if (!lc_wait(l, m, back, at, NULL) || (w = lc_console(l, machine, from, *at)) == NULL) {
        if (!l->stopped)
            lc_assert(l, id, 0, l->why[0] != '\0' ? l->why : "the program did not run");
        return NULL;
    }
    lc_assert(l, id, score_window(w, strlen(w), why, sizeof(why)), why);
    return w;
}

char *lc_run(struct lc *l, struct etude_case_machine *m, const char *machine, const char *command,
             uint64_t *at, const char *id)
{
    uint64_t from = *at;

    if (!lc_type(l, m, command)) {
        if (!l->stopped)
            lc_assert(l, id, 0, l->why);
        return NULL;
    }
    return lc_handback(l, m, machine, from, at, id);
}

int lc_exited0(const char *w, size_t len, char *why, size_t size)
{
    return score_window(w, len, why, size);
}

int lc_timing_level(const char *w, size_t len)
{
    size_t at, from;
    int level = 0;

    for (at = 0; at < len; at++) {
        if (at != 0 && w[at - 1] != '\n')
            continue;
        from = lc_past_prefix(w, len, at);
        if (from == at || from + 8 > len || memcmp(w + from, "timing: ", 8) != 0)
            continue;
        if (w[at + 1] == '6')
            return 6;
        if (w[at + 1] == '3')
            level = 3;
    }
    return level;
}

/** Answer clockmusl's requests on the bound UDP socket `fd` until killed. */
static void clockref_serve(clock_socket fd)
{
    struct sockaddr_in peer;
    socklen_t size;
    struct timespec mono, real;
    char packet[160], tail;
    unsigned sequence;
    ssize_t n;

    for (;;) {
        size = sizeof(peer);
        n = recvfrom(fd, packet, sizeof(packet) - 1, 0, (struct sockaddr *)&peer, &size);
        if (n <= 0) {
#ifdef _WIN32
            return;
#else
            continue;
#endif
        }
        packet[n] = '\0';
#ifdef _WIN32
        if (sscanf(packet, "CLOCKREF %u %c", &sequence, &tail) != 1)
            continue;
        {
            uint64_t us = etude_win_mono_us();
            int64_t utc = etude_win_utc_us();
            mono.tv_sec = (time_t)(us / 1000000u);
            mono.tv_nsec = (long)(us % 1000000u * 1000);
            real.tv_sec = (time_t)(utc / 1000000);
            real.tv_nsec = (long)(utc % 1000000 * 1000);
        }
#else
        if (sscanf(packet, "CLOCKREF %u %c", &sequence, &tail) != 1 ||
            clock_gettime(CLOCK_MONOTONIC, &mono) != 0 || clock_gettime(CLOCK_REALTIME, &real) != 0)
            continue;
#endif
        n = snprintf(packet, sizeof(packet), "CLOCKREF %u %lld %lld", sequence,
                     (long long)mono.tv_sec * 1000000000LL + mono.tv_nsec,
                     (long long)real.tv_sec * 1000000000LL + real.tv_nsec);
        sendto(fd, packet, (size_t)n, 0, (struct sockaddr *)&peer, size);
    }
}

#ifdef _WIN32
static SOCKET clockref_socket = INVALID_SOCKET;
static HANDLE clockref_thread;

/** Answer clock requests until the owner closes the socket. */
static DWORD WINAPI clockref_worker(void *arg)
{
    clockref_serve((clock_socket)(uintptr_t)arg);
    return 0;
}

pid_t lc_clockref_start(unsigned *port)
{
    struct sockaddr_in a;
    int size = sizeof a;
    if (etude_win_net_start() != 0 || clockref_thread != NULL)
        return -1;
    clockref_socket =
        WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (clockref_socket == INVALID_SOCKET ||
        bind(clockref_socket, (struct sockaddr *)&a, size) != 0 ||
        getsockname(clockref_socket, (struct sockaddr *)&a, &size) != 0)
        goto fail;
    *port = ntohs(a.sin_port);
    clockref_thread =
        CreateThread(NULL, 0, clockref_worker, (void *)(uintptr_t)clockref_socket, 0, NULL);
    if (clockref_thread != NULL)
        return 1;
fail:
    if (clockref_socket != INVALID_SOCKET)
        closesocket(clockref_socket);
    clockref_socket = INVALID_SOCKET;
    return -1;
}
void lc_clockref_stop(pid_t pid)
{
    if (pid <= 0 || clockref_thread == NULL)
        return;
    closesocket(clockref_socket);
    WaitForSingleObject(clockref_thread, INFINITE);
    CloseHandle(clockref_thread);
    clockref_thread = NULL;
    clockref_socket = INVALID_SOCKET;
}
#else
pid_t lc_clockref_start(unsigned *port)
{
    struct sockaddr_in a;
    socklen_t size = sizeof(a);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    pid_t pid;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, size) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &size) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    pid = fork();
    if (pid == 0)
        clockref_serve(fd);
    close(fd);
    return pid;
}

void lc_clockref_stop(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
}

#endif

int lc_clockref_said(const char *w, size_t len)
{
    static const char result[] = "CLOCKMUSL REFERENCE OK";
    size_t at, end, n = sizeof(result) - 1;

    for (at = 0; at + n <= len; at++) {
        if ((at != 0 && w[at - 1] != '\n') || memcmp(w + at, result, n) != 0)
            continue;
        end = at + n;
        if (end < len && w[end] == '\r')
            end++;
        if (end < len && w[end] == '\n')
            return 1;
    }
    return 0;
}

int lc_end(struct lc *l, struct etude_case_machine *m, struct etude_termination *end)
{
    enum etude_status st;

    if (!lc_ok(l))
        return 0;
    end->end = ETUDE_END_LOST;
    st = etude_wait_end(m, end);
    if (st != ETUDE_OK)
        lc_halt(l, st, "the wait for the machine's end");
    return st == ETUDE_OK;
}

int lc_quit(struct lc *l, struct etude_case_machine *m, const char *id)
{
    struct etude_termination end;
    enum etude_status quit, st;
    char *reply = NULL, detail[160];

    if (!lc_ok(l))
        return 0;
    quit = etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    if (quit == ETUDE_STOPPED) {
        lc_halt(l, quit, "QMP's quit");
        return 0;
    }
    end.end = ETUDE_END_LOST;
    st = etude_wait_end(m, &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "the wait for the machine's end after QMP's quit");
        if (st != ETUDE_STOPPED && id != NULL)
            lc_assert(l, id, 0, l->why);
        return 0;
    }
    if (id == NULL)
        return 1;
    snprintf(detail, sizeof(detail), "QMP's quit was %s, and the machine ended %s",
             status_name(quit), end_name(end.end));
    lc_assert(l, id, quit == ETUDE_OK && end.end == ETUDE_END_QEMU_EXITED, detail);
    return 1;
}

/**
 * Send QMP command `execute` with `arguments` and give the member `return`
 * of its reply, parsed, with the whole reply in `*doc` for the caller to
 * free; or NULL, with the reason in `why`, when there is none.
 */
static const struct eh_json *ask(struct lc *l, struct etude_case_machine *m, const char *execute,
                                 const char *arguments, struct eh_json **doc, char why[WHY_LEN])
{
    char *reply = NULL;
    const struct eh_json *r = NULL;
    enum etude_status st;

    *doc = NULL;
    st = etude_qmp(m, execute, arguments, &reply);
    if (st == ETUDE_OK && reply != NULL && (*doc = eh_json_parse(reply)) != NULL)
        r = eh_json_get(*doc, "return");
    free(reply);
    if (r == NULL) {
        snprintf(why, WHY_LEN, "QMP's %s was %s", execute, status_name(st));
        if (st != ETUDE_OK && st != ETUDE_FAILED)
            lc_halt(l, st, why);
    }
    return r;
}

/**
 * Say whether every processor of the `info registers -a` text `regs` has
 * halted with its interrupts off, and they are `cpus` when that is not 0;
 * the reason when not is put in `why`. Each processor has one line of
 * flags, `EFL=` or `RFL=` in hexadecimal, that ends with `HLT=0` or `HLT=1`.
 *
 * @return 1 when they have; -1 when a processor has not halted yet; and 0
 *         when they will not.
 */
static int all_halted(const char *regs, int cpus, char *why, size_t size)
{
    const char *p = regs, *eol, *hlt;
    unsigned long flags;
    char *end;
    int n = 0;

    while ((p = strstr(p, "FL=")) != NULL) {
        n++;
        flags = strtoul(p + 3, &end, 16);
        eol = strchr(p, '\n');
        hlt = strstr(p, "HLT=");
        if (end == p + 3 || hlt == NULL || (eol != NULL && hlt > eol)) {
            snprintf(why, size, "the registers of processor %d are not readable", n);
            return 0;
        }
        if (hlt[4] != '1') {
            snprintf(why, size, "processor %d has not halted", n);
            return -1;
        }
        if (flags & 0x200) {
            snprintf(why, size, "processor %d has halted with its interrupts on", n);
            return 0;
        }
        p = hlt;
    }
    if (n == 0) {
        snprintf(why, size, "the monitor gave the registers of no processor");
        return 0;
    }
    if (cpus != 0 && n != cpus) {
        snprintf(why, size, "the monitor gave the registers of %d processors, not %d", n, cpus);
        return 0;
    }
    return 1;
}

void lc_halted(struct lc *l, struct etude_case_machine *m, int cpus, const char *id)
{
    const struct eh_json *r, *status;
    struct eh_json *doc;
    char why[WHY_LEN];
    int held = 0;

    if (!lc_ok(l))
        return;
    r = ask(l, m, "query-status", NULL, &doc, why);
    status = r != NULL ? eh_json_get(r, "status") : NULL;
    if (r != NULL && (status == NULL || status->type != 's' || strcmp(status->string, "running")))
        snprintf(why, sizeof(why), "QEMU says that the machine is %s; a halt leaves it running",
                 status != NULL && status->type == 's' ? status->string : "in no state");
    else if (r != NULL)
        held = 1;
    eh_json_free(doc);
    if (held)
        do {
            r = ask(l, m, "human-monitor-command", "{\"command-line\": \"info registers -a\"}",
                    &doc, why);
            if (r != NULL && r->type != 's')
                snprintf(why, sizeof(why), "the monitor gave no registers");
            held = r != NULL && r->type == 's' ? all_halted(r->string, cpus, why, sizeof(why)) : 0;
            eh_json_free(doc);
        } while (held < 0);
    if (!l->stopped)
        lc_assert(l, id, held, why);
}

void lc_received(struct lc *l, struct etude_case_machine *m, const char *id)
{
    const struct eh_json *r;
    const char *value;
    struct eh_json *doc;
    unsigned long lsr = 0;
    char why[WHY_LEN];
    int held = 0;

    if (!lc_ok(l))
        return;
    r = ask(l, m, "human-monitor-command", "{\"command-line\": \"i /b 0x3fd\"}", &doc, why);
    value = r != NULL && r->type == 's' ? strstr(r->string, "= 0x") : NULL;
    if (value != NULL) {
        lsr = strtoul(value + 2, NULL, 16);
        held = (lsr & 1) != 0;
        snprintf(why, sizeof(why), "the line status register is 0x%02lx: no byte received", lsr);
    } else if (r != NULL) {
        snprintf(why, sizeof(why), "the monitor gave no port value");
    }
    eh_json_free(doc);
    if (!l->stopped)
        lc_assert(l, id, held, why);
}

/** Say whether file `path` has `text`, reading at most its first MiB. */
static int file_has(const char *path, const char *text)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    char *buf;
    int found;

    if (f == NULL)
        return 0;
    buf = malloc((1u << 20) + 1);
    n = buf != NULL ? fread(buf, 1, 1u << 20, f) : 0;
    fclose(f);
    if (buf == NULL)
        return 0;
    buf[n] = '\0';
    found = strstr(buf, text) != NULL;
    free(buf);
    return found;
}

void lc_fsck(struct lc *l, const char *disk, int dirty, const char *id)
{
    static const char unclean[] = "not cleanly unmounted";
    char name[] = "fsck.krffs", offset[] = "--offset", where[] = LC_PART_OFFSET, size[] = "--size",
         bytes[] = LC_PART_SIZE, path[1024], detail[200];
    char *argv[] = {name, path, offset, where, size, bytes, NULL};
    const char *copy = etude_private_copy(l->c, disk);
    struct etude_termination end;
    enum etude_status st;
    int said;

    if (!lc_ok(l))
        return;
    if (copy == NULL || strlen(copy) >= sizeof(path)) {
        lc_halt(l, ETUDE_REFUSED, "the copy of the disk");
        return;
    }
    snprintf(path, sizeof(path), "%s", copy);
    etude_progress(l->c, "fsck", 0);
    st = etude_host(l->c, argv, "fsck", &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "fsck.krffs");
        return;
    }
    etude_progress(l->c, "fsck", 1);
    said = file_has(etude_evidence(l->c, "fsck"), unclean) ||
           file_has(etude_evidence(l->c, "fsck.err"), unclean);
    if (end.end != ETUDE_END_EXITED || end.value != 0)
        snprintf(detail, sizeof(detail), "fsck.krffs ended %s %d", end_name(end.end), end.value);
    else
        snprintf(detail, sizeof(detail), "fsck.krffs says the copy %s",
                 said ? "was not cleanly unmounted" : "was cleanly unmounted");
    lc_assert(l, id, end.end == ETUDE_END_EXITED && end.value == 0 && said == dirty, detail);
}
