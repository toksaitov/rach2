/* Own observation, admitted preparation and collection in one native process.
 */
#include "observe_sequence.h"
#include "routine_owner.h"
#include "authority_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "etude/exec.h"

/* A direct caller can bind its identity before an intervening shell exec. */
int etude_sequence_parent_identity(long parent, struct etude_pid *identity)
{
    const char *text = getenv("ETUDE_SEQUENCE_PARENT");
    uint64_t creation = 0;
    if (text) {
        char *end;
        errno = 0;
        parent = strtol(text, &end, 10);
        if (errno || end == text || *end != ':')
            return -1;
        text = end + 1;
        if (*text < '1' || *text > '9')
            return -1;
        creation = strtoull(text, &end, 10);
        if (errno || *end || !creation)
            return -1;
    }
    if (parent <= 1 || etude_process_state(parent, identity) != 1 || !identity->start_known ||
        (creation && identity->start != creation))
        return -1;
    return 0;
}
#ifdef _WIN32
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include "win/winexec.h"
#include "win/files.h"
#include <tlhelp32.h>
static HANDLE input_gone;
static FILE *record, *sequence_log;
static int output_failed;
static struct etude_win_ident *known;
static size_t nknown, capknown;

static DWORD observe(struct etude_win_child *child, size_t *count)
{
    struct etude_win_ident *ids = NULL;
    size_t cap = 16, n, i, j;
    DWORD error;
    for (;;) {
        struct etude_win_ident *grown;
        if (cap > (SIZE_MAX / sizeof(*ids))) {
            free(ids);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        grown = realloc(ids, cap * sizeof(*ids));
        if (!grown) {
            free(ids);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        ids = grown;
        error = etude_win_pids(child, ids, cap, &n);
        if (error != ERROR_MORE_DATA)
            break;
        if (cap > SIZE_MAX / 2) {
            free(ids);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        cap *= 2;
    }
    if (error) {
        free(ids);
        return error;
    }
    for (i = 0; i < n; i++) {
        for (j = 0; j < nknown; j++)
            if (known[j].pid == ids[i].pid && known[j].created == ids[i].created)
                break;
        if (j != nknown)
            continue;
        if (nknown == capknown) {
            size_t next = capknown ? capknown * 2 : 128;
            struct etude_win_ident *grown;
            if (next < capknown || next > SIZE_MAX / sizeof(*known)) {
                free(ids);
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            grown = realloc(known, next * sizeof(*known));
            if (!grown) {
                free(ids);
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            known = grown;
            capknown = next;
        }
        known[nknown++] = ids[i];
        if (fprintf(record, "observed %lu %llu\n", (unsigned long)ids[i].pid,
                    (unsigned long long)ids[i].created) < 0 ||
            fflush(record)) {
            free(ids);
            return ERROR_WRITE_FAULT;
        }
    }
    free(ids);
    *count = n;
    return 0;
}
static DWORD WINAPI caller_input(void *input)
{
    char byte;
    DWORD got;
    ReadFile((HANDLE)input, &byte, 1, &got, NULL);
    SetEvent(input_gone);
    return 0;
}

static BOOL WINAPI on_console(DWORD type)
{
    (void)type;
    SetEvent(input_gone);
    return TRUE;
}
static int registered(void *unused, struct etude_win_child *child, const struct etude_win_ident *id)
{
    (void)unused;
    (void)child;
    fprintf(record, "child %lu %llu\n", (unsigned long)id->pid, (unsigned long long)id->created);
    return fflush(record);
}
static void output(void *unused, struct etude_win_child *child, enum etude_win_stream stream,
                   const char *bytes, size_t size)
{
    FILE *f = stream == ETUDE_WIN_STDOUT ? stdout : stderr;
    (void)unused;
    (void)child;
    if (fwrite(bytes, 1, size, f) != size || fflush(f))
        output_failed = 1;
    if (sequence_log && (fwrite(bytes, 1, size, sequence_log) != size || fflush(sequence_log)))
        output_failed = 1;
}
/* One persistent owner observes preparation and collection in separate Jobs. */
struct phase {
    const char *name;
    struct etude_win_child *child;
    int ready, overlong;
    size_t line_used;
    char line[64];
    struct etude_reading started;
};
struct sequence {
    const char *run, *trace, *stop, *observer, *prep_path, *runtime_path;
    const char *prep_program, *collection_program, *quiesce_program, *restore_program;
    char **prep_argv, **collection_argv, **quiesce_argv, **restore_argv;
    struct etude_win_sup supervisor;
    struct phase observer_phase, preparation, collection, quiesce, restoration;
    struct etude_routine_owner routine;
    int routed, restored;
    DWORD error;
    HANDLE observer_input, collection_input, caller_input_thread, parent;
    int cancelled, requested;
};

/* Keep an exact parent process handle; a reused PID cannot satisfy it. */
static DWORD parent_pid(void)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry = {0};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snapshot, &entry))
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return parent;
}

int etude_sequence_caller_identity(struct etude_pid *identity)
{
    return etude_sequence_parent_identity(parent_pid(), identity);
}

int etude_observe_parent_main(int argc, char **argv)
{
    struct etude_pid identity;
    (void)argv;
    DWORD parent = parent_pid();
    if (argc != 2 || !parent || etude_process_state(parent, &identity) != 1 ||
        !identity.start_known)
        return 2;
    return printf("%ld %llu\n", identity.pid, (unsigned long long)identity.start) < 0 ? 1 : 0;
}

static DWORD parent_open(struct sequence *s)
{
    DWORD parent = parent_pid();
    if (!parent)
        return ERROR_INVALID_PARAMETER;
    struct etude_pid identity;
    if (etude_sequence_parent_identity(parent, &identity))
        return ERROR_INVALID_PARAMETER;
    parent = (DWORD)identity.pid;
    s->parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
    if (!s->parent)
        return GetLastError();
    FILETIME parent_times[4], own_times[4];
    if (!GetProcessTimes(s->parent, &parent_times[0], &parent_times[1], &parent_times[2],
                         &parent_times[3]) ||
        !GetProcessTimes(GetCurrentProcess(), &own_times[0], &own_times[1], &own_times[2],
                         &own_times[3]))
        return GetLastError();
    if (CompareFileTime(&parent_times[0], &own_times[0]) > 0 ||
        WaitForSingleObject(s->parent, 0) != WAIT_TIMEOUT)
        return ERROR_INVALID_PARAMETER;
    uint64_t created =
        (uint64_t)parent_times[0].dwHighDateTime << 32 | parent_times[0].dwLowDateTime;
    if (created != identity.start)
        return ERROR_INVALID_PARAMETER;
    if (fprintf(record, "parent %lu %llu\n", (unsigned long)parent, (unsigned long long)created) <
            0 ||
        fflush(record))
        return ERROR_WRITE_FAULT;
    return 0;
}

static int token(const char *text)
{
    size_t n = strlen(text);
    return n && n < 256 &&
           strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:/-") == n;
}

static char **vector(int argc, char **argv, int *position, const char **program)
{
    char *end;
    if (*position + 2 > argc)
        return NULL;
    *program = argv[(*position)++];
    const char *count_text = argv[(*position)++];
    if (*count_text < '1' || *count_text > '9')
        return NULL;
    unsigned long count = strtoul(count_text, &end, 10);
    if (*end || count < 1 || count > (unsigned long)(argc - *position))
        return NULL;
    char **result = calloc(count + 1, sizeof(*result));
    if (!result)
        return NULL;
    memcpy(result, argv + *position, count * sizeof(*result));
    *position += (int)count;
    return result;
}

static int sequence_arguments(struct sequence *s, int argc, char **argv)
{
    if (argc < 15)
        return -1;
    s->run = argv[3];
    s->trace = argv[4];
    s->stop = argv[5];
    s->observer = argv[6];
    s->prep_path = argv[7];
    s->runtime_path = argv[8];
    int position = 9;
    s->routed = !strcmp(argv[1], "routine");
    if (s->routed) {
        if (argc < 22 || routine_owner_open(&s->routine, argv[9], argv[10], s->run, argv[11]))
            return -1;
        position = 12;
        s->quiesce_argv = vector(argc, argv, &position, &s->quiesce_program);
        if (!s->quiesce_argv || strcmp(s->quiesce_program, s->observer))
            return -1;
    }
    s->prep_argv = vector(argc, argv, &position, &s->prep_program);
    s->collection_argv = vector(argc, argv, &position, &s->collection_program);
    if (s->routed)
        s->restore_argv = vector(argc, argv, &position, &s->restore_program);
    if (!s->prep_argv || !s->collection_argv || position != argc || !token(s->run) ||
        strcmp(s->prep_program, s->observer) ||
        (!s->routed && strcmp(s->collection_program, s->observer)) || !s->prep_argv[1] ||
        strcmp(s->prep_argv[1], s->routed ? "authority" : "admit") || !s->collection_argv[1] ||
        (!s->routed && strcmp(s->collection_argv[1], "run")) ||
        (s->routed && (!s->restore_argv || strcmp(s->restore_program, s->observer))))
        return -1;
    s->observer_phase.name = "observer";
    s->preparation.name = "preparation";
    s->collection.name = "collection";
    s->quiesce.name = "quiesce";
    s->restoration.name = "restoration";
    return 0;
}

/* Interpret only this owned observer's complete stdout readiness line. */
static void sequence_output(void *context, struct etude_win_child *child,
                            enum etude_win_stream stream, const char *bytes, size_t size)
{
    struct phase *phase = context;
    output(NULL, child, stream, bytes, size);
    if (strcmp(phase->name, "observer") || stream != ETUDE_WIN_STDOUT)
        return;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] == '\n') {
            phase->line[phase->line_used] = 0;
            if (!phase->overlong && !strcmp(phase->line, "etude observe: ready"))
                phase->ready = 1;
            phase->line_used = 0;
            phase->overlong = 0;
        } else if (bytes[i] != '\r') {
            if (phase->line_used + 1 < sizeof(phase->line))
                phase->line[phase->line_used++] = bytes[i];
            else
                phase->overlong = 1;
        }
    }
}

static int sequence_registered(void *context, struct etude_win_child *child,
                               const struct etude_win_ident *id)
{
    struct phase *phase = context;
    if (fprintf(record, "phase-child %s %lu %llu\n", phase->name, (unsigned long)id->pid,
                (unsigned long long)id->created) < 0)
        return 1;
    int error = registered(NULL, child, id);
    return error || WaitForSingleObject(input_gone, 0) == WAIT_OBJECT_0;
}

static DWORD phase_start(struct sequence *s, struct phase *phase, const char *program,
                         char *const *argv, const char *path)
{
    struct etude_win_spawn spawn = {0};
    if (WaitForSingleObject(input_gone, 0) == WAIT_OBJECT_0 ||
        WaitForSingleObject(s->parent, 0) != WAIT_TIMEOUT)
        return ERROR_CANCELLED;
    if (_putenv_s("PATH", path))
        return ERROR_INVALID_ENVIRONMENT;
    if (s->routed && etude_history_now(&phase->started))
        return ERROR_INVALID_DATA;
    spawn.program = program;
    spawn.argv = (const char *const *)argv + 1;
    spawn.registered = sequence_registered;
    spawn.output = sequence_output;
    spawn.ctx = phase;
    fprintf(record, "phase-begin %s %s %llu\n", s->run, phase->name,
            (unsigned long long)GetTickCount64());
    if (fflush(record))
        return ERROR_WRITE_FAULT;
    int direct = s->routed && !strcmp(s->routine.mode, "wsl-native") && phase == &s->collection;
    if (phase != &s->observer_phase && !direct)
        return etude_win_start(&s->supervisor, &spawn, &phase->child);
    HANDLE *writer = direct ? &s->collection_input : &s->observer_input;
    HANDLE read = NULL, original = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreatePipe(&read, writer, NULL, 0))
        return GetLastError();
    DWORD error = 0;
    if (!SetStdHandle(STD_INPUT_HANDLE, read))
        error = GetLastError();
    else {
        spawn.stdin_path = "-";
        error = etude_win_start(&s->supervisor, &spawn, &phase->child);
        if (!SetStdHandle(STD_INPUT_HANDLE, original) && !error)
            error = GetLastError();
    }
    CloseHandle(read);
    return error;
}

/* A stop request is an immutable exact run identity, including its LF. */
static int stop_matches(const struct sequence *s)
{
    FILE *f = fopen(s->stop, "rb");
    char bytes[257];
    if (!f)
        return errno == ENOENT ? 0 : -1;
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    int ok = !ferror(f) && n == strlen(s->run) + 1 && !memcmp(bytes, s->run, n - 1) &&
             bytes[n - 1] == '\n';
    if (fclose(f))
        ok = 0;
    return ok ? 1 : -1;
}

static DWORD request_stop(struct sequence *s)
{
    int match = stop_matches(s);
    if (match) {
        s->requested = match == 1;
        return match == 1 ? 0 : ERROR_INVALID_DATA;
    }
    char part[4096], text[257];
    int n = snprintf(part, sizeof(part), "%s.owner-%lu.tmp", s->stop,
                     (unsigned long)GetCurrentProcessId());
    if (n < 0 || n >= (int)sizeof(part))
        return ERROR_FILENAME_EXCED_RANGE;
    n = snprintf(text, sizeof(text), "%s\n", s->run);
    int fd = etude_win_open(part, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
        return ERROR_OPEN_FAILED;
    DWORD error = 0;
    if (_write(fd, text, (unsigned)n) != n || _commit(fd))
        error = ERROR_WRITE_FAULT;
    if (_close(fd) && !error)
        error = ERROR_WRITE_FAULT;
    if (!error && link(part, s->stop))
        error = ERROR_WRITE_FAULT;
    if (unlink(part) && !error)
        error = ERROR_WRITE_FAULT;
    if (!error)
        s->requested = 1;
    return error;
}

static void stop_all(struct sequence *s)
{
    struct phase *phases[] = {&s->observer_phase, &s->quiesce, &s->preparation, &s->collection,
                              &s->restoration};
    for (size_t i = 0; i < sizeof(phases) / sizeof(*phases); i++)
        if (phases[i]->child && !phases[i]->child->ended) {
            DWORD error = etude_win_stop(phases[i]->child);
            if (error && !s->error)
                s->error = error;
        }
}

static int sequence_poll(struct sequence *s)
{
    if (output_failed)
        s->error = ERROR_WRITE_FAULT;
    if (s->routed && routine_owner_direct_prepared(&s->routine))
        s->error = ERROR_INVALID_DATA;
    if (s->routed && routine_owner_expired(&s->routine))
        s->error = ERROR_TIMEOUT;
    if (WaitForSingleObject(input_gone, 0) == WAIT_OBJECT_0 ||
        WaitForSingleObject(s->parent, 0) != WAIT_TIMEOUT) {
        s->cancelled = 1;
        stop_all(s);
    }
    if (etude_win_poll(&s->supervisor, 100) < 0) {
        s->error = GetLastError();
        if (!s->error)
            s->error = ERROR_GEN_FAILURE;
    }
    struct phase *phases[] = {&s->observer_phase, &s->quiesce, &s->preparation, &s->collection,
                              &s->restoration};
    for (size_t i = 0; i < sizeof(phases) / sizeof(*phases); i++)
        if (phases[i]->child) {
            size_t count;
            DWORD error = observe(phases[i]->child, &count);
            if (error && !s->error)
                s->error = error;
        }
    if (s->error)
        stop_all(s);
    return s->error || s->cancelled ? -1 : 0;
}

static int wait_phase(struct sequence *s, struct phase *phase)
{
    while (!phase->child->ended) {
        if (sequence_poll(s))
            return -1;
        struct etude_win_child *observer = s->observer_phase.child;
        if (observer && observer->ended && phase != &s->observer_phase &&
            ((phase != &s->collection && phase != &s->restoration) || observer->status ||
             stop_matches(s) != 1)) {
            s->error = ERROR_BROKEN_PIPE;
            stop_all(s);
            return -1;
        }
    }
    return 0;
}

static DWORD phase_end(struct sequence *s, struct phase *phase, DWORD *status)
{
    struct etude_win_usage usage = {0};
    struct etude_win_child *child = phase->child;
    size_t count;
    DWORD error = etude_win_usage(child, &usage);
    if (observe(child, &count) || count || usage.active || !child->ended)
        error = ERROR_BUSY;
    *status = child->status;
    fprintf(record, "phase-end %s %s %llu status %lu active %lu cpu_us %llu\n", s->run, phase->name,
            (unsigned long long)GetTickCount64(), (unsigned long)*status,
            (unsigned long)usage.active, (unsigned long long)(usage.user_us + usage.kernel_us));
    if (fflush(record) && !error)
        error = ERROR_WRITE_FAULT;
    struct etude_exec_result result = {0};
    result.process.pid = child->ident.pid;
    result.process.start = child->ident.created;
    result.process.start_known = 1;
    result.end = child->stopped ? ETUDE_END_STOPPED : ETUDE_END_EXITED;
    result.value = result.exit_value = (int)child->status;
    result.exit_end = child->main_ended ? ETUDE_END_EXITED : ETUDE_END_LOST;
    result.cleanup = ETUDE_CLEANUP_CONFIRMED;
    result.output_error = output_failed ? EIO : 0;
    if (etude_win_release(child, 30000))
        error = ERROR_BUSY;
    else
        phase->child = NULL;
    if (!error && s->routed && !strcmp(s->routine.mode, "windows-import") &&
        routine_owner_phase(&s->routine, phase->name, &result, &phase->started))
        error = ERROR_WRITE_FAULT;
    return error;
}

static int clear_preparation_inputs(void)
{
    return _putenv_s("ETUDE_PREPARATION_GRANT", "") || _putenv_s("ETUDE_PREPARATION_IMPORT", "") ||
           _putenv_s("ETUDE_PREPARATION_CLEAN", "");
}

static int sequence_observer(struct sequence *s)
{
    FILETIME times[4];
    char pid[32], creation[32];
    if (!GetProcessTimes(GetCurrentProcess(), &times[0], &times[1], &times[2], &times[3])) {
        s->error = ERROR_INVALID_HANDLE;
        return -1;
    }
    uint64_t created = (uint64_t)times[0].dwHighDateTime << 32 | times[0].dwLowDateTime;
    snprintf(pid, sizeof(pid), "%lu", (unsigned long)GetCurrentProcessId());
    snprintf(creation, sizeof(creation), "%llu", (unsigned long long)created);
    if (fprintf(record, "owner %s %s\nrun %s\n", pid, creation, s->run) < 0 || fflush(record)) {
        s->error = ERROR_WRITE_FAULT;
        return -1;
    }
    char *observer_argv[] = {(char *)s->observer,
                             "observe",
                             "-o",
                             (char *)s->trace,
                             "-p",
                             pid,
                             "-c",
                             creation,
                             "-r",
                             (char *)s->run,
                             "-s",
                             (char *)s->stop,
                             NULL};
    s->error = phase_start(s, &s->observer_phase, s->observer, observer_argv, s->runtime_path);
    if (s->error)
        return -1;
    while (!s->observer_phase.ready) {
        if (sequence_poll(s))
            return -1;
        if (s->observer_phase.child->ended) {
            s->error = ERROR_BROKEN_PIPE;
            return -1;
        }
    }
    return 0;
}

static int sequence_preparation(struct sequence *s, DWORD *status)
{
    if (s->routed) {
        s->error = phase_start(s, &s->quiesce, s->quiesce_program, s->quiesce_argv, s->prep_path);
        if (s->error || wait_phase(s, &s->quiesce) || phase_end(s, &s->quiesce, status) || *status)
            return -1;
        if (routine_owner_grant(&s->routine)) {
            s->error = ERROR_ACCESS_DENIED;
            return -1;
        }
    }
    s->error = phase_start(s, &s->preparation, s->prep_program, s->prep_argv, s->prep_path);
    if (s->error || wait_phase(s, &s->preparation))
        return -1;
    s->error = phase_end(s, &s->preparation, status);
    if (s->error)
        return -1;
    if (s->routed && !strcmp(s->routine.mode, "windows-import") &&
        routine_owner_prepared(&s->routine)) {
        s->error = ERROR_INVALID_DATA;
        return -1;
    }
    return 0;
}

static int sequence_collection(struct sequence *s, DWORD *status)
{
    if (clear_preparation_inputs()) {
        s->error = ERROR_INVALID_ENVIRONMENT;
        return -1;
    }
    if (s->routed && !strcmp(s->routine.mode, "windows-import")) {
        char projection[4096];
        int n = snprintf(projection, sizeof projection, "%s/consumer.site", s->routine.directory);
        if (n < 0 || (size_t)n >= sizeof projection || setenv("ETUDE_SITE", projection, 1) ||
            setenv("ETUDE_AUTHORITY_PHASE", "consumer", 1)) {
            s->error = ERROR_INVALID_ENVIRONMENT;
            return -1;
        }
    }
    s->error =
        phase_start(s, &s->collection, s->collection_program, s->collection_argv, s->runtime_path);
    if (s->error || wait_phase(s, &s->collection))
        return -1;
    s->error = phase_end(s, &s->collection, status);
    if (s->error)
        return -1;
    /* A successful observed collection must have requested its own footer.
     */
    if (!*status && stop_matches(s) != 1)
        s->error = ERROR_INVALID_DATA;
    return s->error ? -1 : 0;
}

static DWORD sequence_run(struct sequence *s)
{
    DWORD status = 125, observer_status;
    if ((s->routed && routine_owner_observation_start(&s->routine)) || sequence_observer(s) ||
        sequence_preparation(s, &status))
        return 125;
    if (!status && sequence_collection(s, &status))
        return 125;
    if (s->routed) {
        if (routine_owner_observed(&s->routine)) {
            s->error = ERROR_INVALID_DATA;
            return 125;
        }
        DWORD restore_status;
        s->error =
            phase_start(s, &s->restoration, s->restore_program, s->restore_argv, s->prep_path);
        if (s->error || wait_phase(s, &s->restoration) ||
            phase_end(s, &s->restoration, &restore_status) || restore_status)
            return 125;
        s->restored = 1;
    }
    DWORD error = request_stop(s);
    if (error && !s->error)
        s->error = error;
    if (s->error || wait_phase(s, &s->observer_phase))
        return status;
    s->error = phase_end(s, &s->observer_phase, &observer_status);
    if (!observer_status || s->error)
        return status;
    s->error = ERROR_INVALID_DATA;
    return status;
}

static void sequence_dispose(struct sequence *s)
{
    free(known);
    free(s->prep_argv);
    free(s->collection_argv);
    free(s->quiesce_argv);
    free(s->restore_argv);
    SetConsoleCtrlHandler(on_console, FALSE);
    if (s->parent)
        CloseHandle(s->parent);
    if (s->observer_input)
        CloseHandle(s->observer_input);
    if (s->collection_input)
        CloseHandle(s->collection_input);
    if (s->caller_input_thread) {
        CancelSynchronousIo(s->caller_input_thread);
        WaitForSingleObject(s->caller_input_thread, INFINITE);
        CloseHandle(s->caller_input_thread);
    }
    CloseHandle(input_gone);
}

static int sequence_terminal(struct sequence *s, int result)
{
    if (s->routed && s->restored) {
        struct etude_authority_phase phase = {.run_id = s->run,
                                              .commit = s->routine.commit,
                                              .nonce = getenv("ETUDE_AUTHORITY_NONCE"),
                                              .owner = s->routine.owner};
        if (etude_authority_physical(&phase, 1, NULL))
            result = 125;
        else if (routine_owner_observation_finish(&s->routine, 1) ||
                 routine_owner_complete(&s->routine, result, 1))
            result = 125;
    }
    if (s->routed && s->routine.observer)
        routine_owner_observation_finish(&s->routine, 0);
    return result;
}

static int sequence_close(struct sequence *s, DWORD status)
{
    if (s->error || s->cancelled)
        stop_all(s);
    struct phase *phases[] = {&s->observer_phase, &s->quiesce, &s->preparation, &s->collection,
                              &s->restoration};
    for (size_t i = 0; i < sizeof(phases) / sizeof(*phases); i++)
        if (phases[i]->child) {
            /* release stops and waits even when the main poll failed. */
            DWORD error = etude_win_release(phases[i]->child, 30000);
            fprintf(record, "phase-cleanup %s %s %s\n", s->run, phases[i]->name,
                    error ? "unconfirmed" : "confirmed");
            if (!error)
                phases[i]->child = NULL;
        }
    DWORD cleanup_error = s->supervisor.port ? etude_win_sup_close(&s->supervisor, 30000) : 0;
    if (cleanup_error && !s->error)
        s->error = cleanup_error;
    if (sequence_log && fclose(sequence_log))
        output_failed = 1;
    if (output_failed)
        s->error = ERROR_WRITE_FAULT;
    int result = cleanup_error  ? 125
                 : s->cancelled ? 130
                 : s->error     ? 125
                 : status > 255 ? 126
                                : (int)status;
    if (!cleanup_error)
        result = sequence_terminal(s, result);
    else if (s->routed && s->routine.observer)
        routine_owner_observation_finish(&s->routine, 0);
    fprintf(record, "error %lu\ncancelled %d\nstatus %d\nactive %s\ncleanup %s\n",
            (unsigned long)s->error, s->cancelled, result, cleanup_error ? "unknown" : "0",
            cleanup_error ? "unconfirmed" : "confirmed");
    int receipt_failed = fclose(record) != 0;
    sequence_dispose(s);
    return receipt_failed ? 125 : result;
}

int etude_observe_sequence_main(int argc, char **argv)
{
    struct sequence s = {0};
    if (sequence_arguments(&s, argc, argv)) {
        fputs("etude observe sequence: invalid counted sequence arguments\n", stderr);
        free(s.prep_argv);
        free(s.collection_argv);
        return 2;
    }
    struct stat st;
    if (!lstat(s.trace, &st) || errno != ENOENT || !lstat(s.stop, &st) || errno != ENOENT) {
        fputs("etude observe sequence: trace or stop request already exists\n", stderr);
        free(s.prep_argv);
        free(s.collection_argv);
        return 2;
    }
    int fd = etude_win_open(argv[2], O_CREAT | O_EXCL | O_WRONLY, 0600);
    record = fd < 0 ? NULL : _fdopen(fd, "wb");
    if (!record) {
        if (fd >= 0)
            _close(fd);
        free(s.prep_argv);
        free(s.collection_argv);
        return 2;
    }
    input_gone = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!input_gone) {
        fclose(record);
        return 2;
    }
    if (getenv("ETUDE_ROUTINE_INPUT")) {
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (GetFileType(input) != FILE_TYPE_PIPE ||
            !(s.caller_input_thread = CreateThread(NULL, 0, caller_input, input, 0, NULL)))
            s.error = ERROR_INVALID_HANDLE;
    }
    SetConsoleCtrlHandler(on_console, TRUE);
    sequence_log = NULL;
    output_failed = 0;
    const char *log_path = getenv("ETUDE_SEQUENCE_LOG");
    if (log_path && *log_path) {
        fd = etude_win_open(log_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        sequence_log = fd < 0 ? NULL : _fdopen(fd, "wb");
        if (!sequence_log) {
            if (fd >= 0)
                _close(fd);
            s.error = ERROR_OPEN_FAILED;
        }
    }
    if (!s.error)
        s.error = parent_open(&s);
    if (!s.error)
        s.error = etude_win_sup_open(&s.supervisor);
    DWORD status = s.error ? 125 : sequence_run(&s);
    return sequence_close(&s, status);
}

#else
int etude_observe_parent_main(int argc, char **argv)
{
    return etude_posix_observe_parent_main(argc, argv);
}

int etude_observe_sequence_main(int argc, char **argv)
{
    return etude_posix_observe_sequence_main(argc, argv);
}
#endif
