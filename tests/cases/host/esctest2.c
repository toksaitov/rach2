/* esctest2.c: run the upstream driver on term's pty and judge its results. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#endif
#include "native.h"

#ifndef _WIN32
static pid_t terminal = -1, driver = -1;
static volatile sig_atomic_t stopped;
static FILE *commands, *answers;
static int listener = -1, peer = -1;
static int socket_created;
static struct native_case test;

struct exception {
    char name[256];
    int seen; /* Exactly one failing result must consume this entry. */
};
static struct exception known[2048];
static char protected[128][256];
static size_t nknown, nprotected;

/** Mark interruption; the foreground controller owns all cleanup. */
static void stop(int sig)
{
    stopped = sig;
}

/** Stop the driver before closing its pipes, then reap its terminal parent. */
static void cleanup(void)
{
    int status;
    if (driver > 0)
        kill(-driver, SIGKILL);
    if (terminal > 0) {
        kill(terminal, SIGTERM);
        while (waitpid(terminal, &status, 0) < 0 && errno == EINTR)
            ;
        terminal = -1;
    }
    if (commands)
        fclose(commands);
    if (answers)
        fclose(answers);
    commands = answers = NULL;
    if (peer >= 0)
        close(peer);
    if (listener >= 0)
        close(listener);
    peer = listener = -1;
    if (socket_created)
        unlink("screen.sock");
    socket_created = 0;
}

/** Read one complete line, rejecting truncation and embedded NUL bytes. */
static int line(FILE *f, char *out, size_t size)
{
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            out[n] = 0;
            return 1;
        }
        if (!c || n + 1 >= size)
            return -1;
        out[n++] = (char)c;
    }
    return n || ferror(f) ? -1 : 0;
}

/** Read exact exception names and reasons, or protected name prefixes. */
static int policy(const char *name, int exceptions)
{
    char path[4096], text[4096], previous[256] = "";
    FILE *f;
    int r, bad = 0;
    if (native_tool(&test, path, name) || !(f = fopen(path, "r")))
        return -1;
    while ((r = line(f, text, sizeof(text))) > 0) {
        char *reason;
        size_t n;
        if (!*text || *text == '#')
            continue;
        reason = strchr(text, '|');
        if (exceptions) {
            if (!reason || !reason[1] || reason[1] == ' ' || reason[1] == '\t') {
                bad = 1;
                break;
            }
            *reason = 0;
            if (!strstr(text, ".test_") || strcmp(previous, text) >= 0) {
                bad = 1;
                break;
            }
        }
        n = strlen(text);
        if (!n || n >= sizeof(previous) ||
            strspn(text, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.") != n ||
            (exceptions ? nknown == 2048 : nprotected == 128)) {
            bad = 1;
            break;
        }
        if (exceptions) {
            for (size_t i = 0; i < nprotected; i++)
                if (!strncmp(text, protected[i], strlen(protected[i])))
                    bad = 1;
            strcpy(known[nknown++].name, text);
            strcpy(previous, text);
        } else {
            strcpy(protected[nprotected++], text);
        }
    }
    fclose(f);
    return bad || r < 0 ? -1 : 0;
}

/** Launch term -H and the packaged upstream driver, with no shell. */
static int start(void)
{
    struct sockaddr_un addr;
    int in[2], out[2];
    char term[4096], zip[4096], python[4096], result[4096], log[4096];
    if (native_tool(&test, term, "tests/host/esctest2-term") ||
        native_tool(&test, zip, "tests/host/esctest2-driver.zip") ||
        native_tool(&test, python, "tests/host/esctest2-python3") ||
        native_path(result, test.work, "results.tsv") || native_path(log, test.work, "driver.log"))
        return -1;
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "screen.sock");
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)))
        return -1;
    socket_created = 1;
    if (listen(listener, 1) || pipe(in))
        return -1;
    if (pipe(out)) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    fflush(NULL);
    terminal = fork();
    if (terminal == 0) {
        close(listener);
        if (dup2(in[0], 0) < 0 || dup2(out[1], 1) < 0)
            _exit(127);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        execl(term, term, "-H", "-c", "absent.conf", python, "-B", zip, "--snapshot-socket",
              "screen.sock", "--result-file", result, "--logfile", log, "--no-print-logs",
              (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (terminal < 0) {
        close(in[1]);
        close(out[0]);
        return -1;
    }
    printf("@etude 1 child %ld\n", (long)terminal);
    commands = fdopen(in[1], "w");
    answers = fdopen(out[0], "r");
    return commands && answers && fprintf(commands, "wait ESCTEST2_READY\n") > 0 &&
                   !fflush(commands)
               ? 0
               : -1;
}

/** Write a complete line to the inspection socket, or fail on interruption. */
static int reply(const char *text)
{
    size_t len = strlen(text), used = 0;
    while (used < len) {
        ssize_t n = write(peer, text + used, len - used);
        if (n < 0 && errno == EINTR && !stopped)
            continue;
        if (n <= 0 || stopped)
            return -1;
        used += (size_t)n;
    }
    return 0;
}

/** Send a snapshot command once the driver's CPR has drained its output. */
static int measurement(char *request)
{
    char answer[4096], trailing;
    unsigned x, y, cols, rows;
    if (sscanf(request, "pid %u%c", &x, &trailing) == 1 && driver < 0 && x > 1) {
        driver = (pid_t)x;
        printf("@etude 1 child %ld\n", (long)driver);
        return 0;
    }
    if (strcmp(request, "dump") && sscanf(request, "cell %u %u%c", &x, &y, &trailing) != 2)
        return -1;
    if (fprintf(commands, "%s\n", request) < 0 || fflush(commands) ||
        line(answers, answer, sizeof(answer)) != 1)
        return -1;
    if (!strcmp(request, "dump")) {
        if (sscanf(answer, "screen %u %u", &cols, &rows) != 2 || rows > 1000)
            return -1;
        if (reply(answer) || reply("\n"))
            return -1;
        for (unsigned i = 0; i < rows; i++)
            if (line(answers, answer, sizeof(answer)) != 1)
                return -1;
    } else if (reply(answer) || reply("\n")) {
        return -1;
    }
    return 0;
}

/**
 * Serve inspection until the driver exits. Terminal output outside a
 * requested answer is a protocol error. Progress is reported every sixteenth
 * measurement, since etude takes at most 4096 records and a run makes over
 * 5000 measurements.
 */
static int serve(void)
{
    struct pollfd fds[2];
    char request[4096];
    size_t used = 0;
    unsigned measurements = 0;
    int ready = 0;
    for (;;) {
        fds[0].fd = peer >= 0 ? peer : listener;
        fds[0].events = POLLIN;
        fds[1].fd = fileno(answers);
        fds[1].events = POLLIN;
        if (stopped)
            return -1;
        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (peer < 0 && fds[0].revents) {
            peer = accept(listener, NULL, NULL);
            if (peer < 0)
                return -1;
            continue;
        }
        if (fds[1].revents) {
            if (ready || peer < 0 || line(answers, request, sizeof(request)) != 1 ||
                strcmp(request, "found 0") || reply("ready\n"))
                return -1;
            ready = 1;
            continue;
        }
        if (!fds[0].revents)
            continue;
        char c;
        ssize_t n = read(peer, &c, 1);
        if (n == 0)
            return used ? -1 : 0;
        if (n < 0 || !c || used + 1 == sizeof(request))
            return -1;
        if (c != '\n') {
            request[used++] = c;
            continue;
        }
        request[used] = 0;
        if (measurement(request))
            return -1;
        used = 0;
        if (++measurements % 16 == 0)
            printf("@etude 1 progress measurements %u\n", measurements);
    }
}

/**
 * Add case `name` to the `*count` names in `*seen`. Returns 0, 1 when the
 * name was there already, or -1 when memory runs out.
 */
static int remember(char ***seen, size_t *count, const char *name)
{
    int dup = 0;
    for (size_t i = 0; i < *count; i++)
        if (!strcmp((*seen)[i], name))
            dup = 1;
    char **more = realloc(*seen, (*count + 1) * sizeof(**seen));
    if (!more)
        return -1;
    *seen = more;
    more[*count] = strdup(name);
    if (!more[*count])
        return -1;
    (*count)++;
    return dup;
}

/**
 * Count case `name`'s result `text` against the known failures. Returns 1,
 * after saying so, when a passing case is a known failure or a failing one
 * is not, and 0 otherwise.
 */
static int unexpected(const char *name, const char *text, int pass)
{
    struct exception *entry = NULL;
    for (size_t i = 0; i < nknown; i++)
        if (!strcmp(known[i].name, name))
            entry = &known[i];
    if (entry)
        entry->seen++;
    if ((!pass && !entry) || (pass && entry)) {
        fprintf(stderr, "esctest2: %s: %s%s\n", name, text,
                pass ? ": remove the known failure" : ": unexpected result");
        return 1;
    }
    return 0;
}

/** Require a result for each case chosen, END, and each exception used once. */
static int results(void)
{
    char path[4096], text[4096];
    char **seen = NULL;
    size_t count = 0;
    unsigned total = 0;
    int r, ended = 0, bad = 0;
    FILE *f;
    if (native_path(path, test.work, "results.tsv") || !(f = fopen(path, "r")))
        return -1;
    while ((r = line(f, text, sizeof(text))) > 0) {
        char *name = strchr(text, '\t'), extra;
        if (ended || !name) {
            bad = 1;
            break;
        }
        *name++ = 0;
        if (!strcmp(text, "END")) {
            ended = sscanf(name, "%u%c", &total, &extra) == 1;
            bad |= !ended || total != count || !total;
            continue;
        }
        int pass = !strcmp(text, "PASS");
        if ((!pass && strcmp(text, "FAIL") && strcmp(text, "SKIP")) || !strstr(name, ".test_")) {
            bad = 1;
            break;
        }
        int dup = remember(&seen, &count, name);
        if (dup < 0) {
            bad = 1;
            break;
        }
        bad |= dup | unexpected(name, text, pass);
    }
    bad |= r < 0 || !ended;
    for (size_t i = 0; i < nknown; i++)
        if (known[i].seen != 1) {
            fprintf(stderr, "esctest2: unused known failure: %s\n", known[i].name);
            bad = 1;
        }
    for (size_t i = 0; i < count; i++)
        free(seen[i]);
    free(seen);
    fclose(f);
    return bad ? -1 : 0;
}

/** Run the declared host case and retain the driver log on any failure. */
int main(void)
{
    char answer[4096];
    int status = 0, ok;
    if (native_begin(&test, "host.esctest2.conformance"))
        return 2;
    atexit(cleanup);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGHUP, stop);
    signal(SIGPIPE, SIG_IGN);
    ok = !policy("tests/host/esctest2-protected-prefixes.txt", 0) && nprotected &&
         !policy("tests/host/esctest2-known-failures.txt", 1);
    native_assert(&test, "policy", ok, "malformed or protected known failure");
    ok = ok && !chdir(test.work) && !start() && !serve();
    if (ok) {
        ok = fprintf(commands, "exit\n") > 0 && !fflush(commands) &&
             line(answers, answer, sizeof(answer)) == 1 && !strcmp(answer, "exit 0");
        fclose(commands);
        commands = NULL;
        pid_t got;
        do
            got = waitpid(terminal, &status, 0);
        while (got < 0 && errno == EINTR && !stopped);
        ok = ok && got == terminal && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (got == terminal)
            terminal = driver = -1;
    }
    native_assert(&test, "driver", ok, "the driver or its terminal did not finish cleanly");
    native_assert(&test, "results", ok && !results(), "unexpected, missing or stale results");
    cleanup();
    if (chdir(test.tree))
        return 1;
    return native_end(&test);
}
#else
/** Refuse a host without the POSIX pty that term -H currently requires. */
int main(void)
{
    fprintf(stderr, "esctest2: term -H requires a POSIX host\n");
    return 1;
}
#endif
