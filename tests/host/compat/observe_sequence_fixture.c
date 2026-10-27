/* Compiled observer, preparation and collection roles for the native owner. */
#include "observe_sequence.h"
#ifdef _WIN32
#include "win/winexec.h"
#else
#include <poll.h>
#include <signal.h>
#include "etude/exec.h"
extern char **environ;
#endif
static const char *sequence_program;
#ifndef _WIN32
static volatile sig_atomic_t sequence_terminated;

static void sequence_terminate(int sig)
{
    (void)sig;
    sequence_terminated = 1;
}

/* A collection that outlasts the observer's grace: it holds the lock file
 * `lock` and, at the owner's cancellation, takes three seconds to end and
 * remove it, as one that cancels its cases and releases the tree does. */
static int sequence_fixture_slow_stop(const char *lock)
{
    FILE *f;
    int i;

    signal(SIGTERM, sequence_terminate);
    if ((f = fopen(lock, "wb")) == NULL || fclose(f) != 0)
        return 2;
    puts("collection: started");
    fflush(stdout);
    for (i = 0; i < 6000 && !sequence_terminated; i++)
        poll(NULL, 0, 10); /* interval: wait for the cancellation */
    if (!sequence_terminated)
        return 2;
    for (i = 0; i < 30; i++)
        poll(NULL, 0, 100); /* interval: the collection's own cleanup */
    return remove(lock) == 0 ? 3 : 2;
}
#endif

static int sequence_input_gone(void)
{
#ifdef _WIN32
    DWORD available;
    return !PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), NULL, 0, NULL, &available, NULL) ||
           available;
#else
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    return poll(&input, 1, 0) > 0 && input.revents;
#endif
}

static void sequence_poll_interval(void)
{
#ifdef _WIN32
    Sleep(10); /* interval: poll the fixture stop request every 10 ms */
#else
    poll(NULL, 0, 10); /* interval: poll the fixture stop request every 10 ms */
#endif
}

static int sequence_environment(const char *name, const char *value)
{
#ifdef _WIN32
    return _putenv_s(name, value ? value : "");
#else
    return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

static void sequence_caller(const char *receipt)
{
    struct etude_pid caller;
    char identity[80], log[1200];
#ifdef _WIN32
    long pid = (long)GetCurrentProcessId();
#else
    long pid = (long)getpid();
#endif
    CHECK(etude_process_state(pid, &caller) == 1 && caller.start_known);
    snprintf(identity, sizeof(identity), "%ld:%llu", caller.pid, (unsigned long long)caller.start);
    fix_format(log, sizeof(log), "%s.log", receipt);
    CHECK(sequence_environment("ETUDE_SEQUENCE_PARENT", identity) == 0);
    CHECK(sequence_environment("ETUDE_SEQUENCE_LOG", log) == 0);
}

static int sequence_fixture_observer(int argc, char **argv)
{
    const char *trace_path = NULL, *stop = NULL, *run = NULL;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-o"))
            trace_path = argv[i + 1];
        if (!strcmp(argv[i], "-s"))
            stop = argv[i + 1];
        if (!strcmp(argv[i], "-r"))
            run = argv[i + 1];
    }
    if (!trace_path || !stop || !run)
        return 2;
    FILE *f = fopen(trace_path, "wb");
    if (!f)
        return 2;
    fputs("start\n", f);
    fflush(f);
    if (!strcmp(getenv("SEQUENCE_FIXTURE_MODE"), "observer-failure")) {
        fclose(f);
        return 8;
    }
    puts("etude observe: ready");
    fflush(stdout);
    int status = 9;
    for (;;) {
        if (sequence_input_gone())
            break;
        FILE *request = fopen(stop, "rb");
        if (request) {
            char text[256];
            size_t n = fread(text, 1, sizeof(text), request);
            int ok = !ferror(request) && n == strlen(run) + 1 && !memcmp(text, run, n - 1) &&
                     text[n - 1] == '\n';
            fclose(request);
            if (ok) {
                fputs("end\n", f);
                status = 0;
            }
            break;
        }
        sequence_poll_interval();
    }
    if (fclose(f))
        status = 9;
    return status;
}

/* Return -1 for the ordinary host-test entry. */
static int sequence_fixture_role(int argc, char **argv)
{
    if (argc < 2)
        return -1;
    if (!strcmp(argv[1], "observe")) {
        if (argc > 2 && !strcmp(argv[2], "sequence"))
            return etude_observe_sequence_main(argc - 1, argv + 1);
        return sequence_fixture_observer(argc, argv);
    }
    if (!strcmp(argv[1], "admit"))
        return !strcmp(getenv("SEQUENCE_FIXTURE_MODE"), "preparation-failure") ? 7 : 0;
    if (!strcmp(argv[1], "run") && argc == 4) {
#ifndef _WIN32
        if (!strcmp(getenv("SEQUENCE_FIXTURE_MODE"), "stopped-collection")) {
            char lock[1200];
            snprintf(lock, sizeof(lock), "%s.lock", argv[2]);
            return sequence_fixture_slow_stop(lock);
        }
#endif
        FILE *f = fopen(argv[2], "wb");
        if (!f)
            return 2;
        int ok = fprintf(f, "%s\n", argv[3]) > 0;
        return fclose(f) || !ok ? 2 : 0;
    }
    return -1;
}

static char *sequence_fixture_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *bytes = calloc(65536, 1);
    if (!f || !bytes) {
        if (f)
            fclose(f);
        free(bytes);
        return NULL;
    }
    size_t n = fread(bytes, 1, 65535, f);
    int bad = ferror(f) || !feof(f);
    bytes[n] = 0;
    if (fclose(f) || bad) {
        free(bytes);
        return NULL;
    }
    return bytes;
}

/* Run the owner in `mode`, cancelling it once its collection has started
 * when `cancel` is set, and check that it ends with `expected`. */
static void sequence_fixture_row(const char *mode, unsigned expected, int cancel)
{
    char self[32768], receipt[1200], trace_path[1200], stop[1200], lock[1200];
#ifdef _WIN32
    CHECK(GetModuleFileNameA(NULL, self, sizeof(self)) > 0);
#else
    CHECK(realpath(sequence_program, self) != NULL);
#endif
    snprintf(receipt, sizeof(receipt), "%s/%s.receipt", fix_dir(), mode);
    snprintf(trace_path, sizeof(trace_path), "%s/%s.trace", fix_dir(), mode);
    snprintf(stop, sizeof(stop), "%s/%s.stop", fix_dir(), mode);
    snprintf(lock, sizeof(lock), "%s/%s.stop.lock", fix_dir(), mode);
    CHECK(sequence_environment("SEQUENCE_FIXTURE_MODE", mode) == 0);
    /* This owner watches our identity; its executor gives it /dev/null,
     * not the enclosing sequence owner's cancellation pipe. */
    CHECK(sequence_environment("ETUDE_SEQUENCE_INPUT", NULL) == 0);
    sequence_caller(receipt);
    const char *path = getenv("PATH");
    const char *args[] = {self, "observe", "sequence", receipt, "fixture", trace_path, stop,
                          self, path,      path,       self,    "2",       self,       "admit",
                          self, "4",       self,       "run",   stop,      "fixture",  NULL};
#ifdef _WIN32
    struct etude_win_sup sup;
    struct etude_win_spawn spawn = {0};
    struct etude_win_child *child = NULL;
    DWORD error = etude_win_sup_open(&sup);
    CHECK(error == 0);
    if (error)
        return;
    spawn.program = self;
    spawn.argv = args + 1;
    error = etude_win_start(&sup, &spawn, &child);
    CHECK(error == 0);
    if (!error) {
        while (!child->ended && !error)
            if (etude_win_poll(&sup, 100) < 0)
                error = GetLastError();
        CHECK(!error && child->status == expected);
    }
    CHECK(etude_win_sup_close(&sup, 30000) == 0);
#else
    struct etude_exec *ex = etude_exec_new();
    struct etude_exec_spec spec = {0};
    struct etude_proc *child = NULL;
    spec.attempt_id = "sequence-fixture";
    spec.argv = (char *const *)args;
    spec.envp = environ;
    spec.output_max = 65536;
    spec.grace_us = 30000000;
    CHECK(ex != NULL);
    int error = ex ? etude_exec_start(ex, &spec, &child) : -1;
    CHECK(error == 0);
    if (!error) {
        while (etude_exec_wait(ex, 100) != child)
            if (cancel && strstr(etude_proc_result(child)->out.data, "collection: started\n"))
                etude_exec_cancel(child);
        const struct etude_exec_result *r = etude_proc_result(child);
        CHECK(r->exit_end == ETUDE_END_EXITED && r->exit_value == (int)expected);
        CHECK(r->cleanup == ETUDE_CLEANUP_CONFIRMED);
        CHECK(access(lock, F_OK) != 0);
    }
    if (ex)
        etude_exec_free(ex);
#endif
    char *text = sequence_fixture_read(receipt);
    CHECK(text && strstr(text, "\nactive 0\ncleanup confirmed\n"));
    if (text) {
        CHECK(strstr(text, "\nrun fixture\n") != NULL);
        CHECK((strstr(text, "phase-begin fixture collection ") != NULL) ==
              (expected == 0 || cancel));
        free(text);
    }
    text = sequence_fixture_read(trace_path);
    CHECK(text &&
          (!strcmp(mode, "observer-failure") || strstr(text, cancel ? "start\n" : "start\nend\n")));
    free(text);
}

static void sequence_fixture(void)
{
    sequence_fixture_row("pass", 0, 0);
    sequence_fixture_row("preparation-failure", 7, 0);
    sequence_fixture_row("observer-failure", 125, 0);
#ifndef _WIN32
    sequence_fixture_row("stopped-collection", 130, 1);
#endif
    CHECK(sequence_environment("SEQUENCE_FIXTURE_MODE", NULL) == 0);
    CHECK(sequence_environment("ETUDE_SEQUENCE_PARENT", NULL) == 0);
    CHECK(sequence_environment("ETUDE_SEQUENCE_LOG", NULL) == 0);
}
