/* Native reexecution and real stop delivery for one set-driver child. */
#ifdef _WIN32
#include "win/files.h"
#include "win/winfmt.h"
#include <wchar.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "legacyset_fixture.h"

struct legacy_child {
#ifdef _WIN32
    HANDLE process, job;
    DWORD pid;
    struct etude_win_ident *consoles;
    size_t nconsoles;
#else
    pid_t pid;
#endif
    int ended, status;
    struct legacy_child *next;
};

static struct legacy_child *children;
static void (*stop_callback)(int);

/** Stop and reap every owned child before fixture directories are removed. */
static void child_cleanup(void)
{
    struct legacy_child *c;
    for (c = children; c; c = c->next) {
        if (c->ended)
            continue;
#ifdef _WIN32
        TerminateJobObject(c->job, 121);
#else
        if (legacy_child_stop(c) != 0)
            kill(c->pid, SIGKILL);
#endif
        legacy_child_wait(c);
    }
}

#ifdef _WIN32
static PVOID volatile break_acknowledgement;

static BOOL WINAPI ignore_console(DWORD event)
{
    if (event == CTRL_BREAK_EVENT) {
        HANDLE ack = (HANDLE)InterlockedCompareExchangePointer(&break_acknowledgement, NULL, NULL);
        if (ack != NULL)
            SetEvent(ack);
    }
    return event == CTRL_BREAK_EVENT || event == CTRL_C_EVENT;
}

static BOOL WINAPI console_stop(DWORD event)
{
    if (event != CTRL_BREAK_EVENT && event != CTRL_C_EVENT)
        return FALSE;
    stop_callback(SIGTERM);
    return TRUE;
}
#endif

void legacy_child_catch(void (*stop)(int))
{
    stop_callback = stop;
#ifdef _WIN32
    SetConsoleCtrlHandler(console_stop, TRUE);
#else
    static const int signals[] = {SIGTERM, SIGINT, SIGHUP};
    struct sigaction action;
    size_t i;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        sigaction(signals[i], &action, NULL);
#endif
}

#ifdef _WIN32
/** Open the private ready and release events for this controller process. */
static int console_events(DWORD pid, HANDLE events[2], int create)
{
    wchar_t name[80];
    int i;

    for (i = 0; i < 2; i++) {
        swprintf(name, sizeof(name) / sizeof(name[0]), L"Local\\rach2-legacy-%lu-%d",
                 (unsigned long)pid, i);
        events[i] = create ? CreateEventW(NULL, TRUE, FALSE, name)
                           : OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
        if (!events[i] || (create && GetLastError() == ERROR_ALREADY_EXISTS)) {
            while (i >= 0) {
                if (events[i])
                    CloseHandle(events[i]);
                events[i--] = NULL;
            }
            return -1;
        }
    }
    return 0;
}

static int remember_consoles(struct legacy_child *c);
static void finish_job(struct legacy_child *c);

/** Hold the child before fixture work until its console identity is saved. */
static int console_ready(struct legacy_child *c, HANDLE ready, HANDLE release)
{
    HANDLE waits[2] = {c->process, ready};

    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1 ||
        remember_consoles(c) != 0)
        return -1;
    return SetEvent(release) ? 0 : -1;
}

/** Give this controller a separate console without visible windows. */
static int start_child(struct legacy_child *c, const char *self, const char *description)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    HANDLE events[2] = {NULL, NULL};
    const char *const args[] = {"--legacy-driver", description, NULL};
    char *line = NULL;
    wchar_t *command, *program = etude_win_wide(self);
    if (etude_win_cmdline(self, args, &line) != 0) {
        free(program);
        return -1;
    }
    command = etude_win_wide(line);
    free(line);
    int ok = 0;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    c->job = CreateJobObjectW(NULL, NULL);
    if (command && program && c->job &&
        SetInformationJobObject(c->job, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits)) &&
        CreateProcessW(program, command, NULL, NULL, FALSE,
                       CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED, NULL, NULL,
                       &startup, &process)) {
        if (AssignProcessToJobObject(c->job, process.hProcess)) {
            c->process = process.hProcess;
            c->pid = process.dwProcessId;
            ok = console_events(c->pid, events, 1) == 0 &&
                 ResumeThread(process.hThread) != (DWORD)-1 &&
                 console_ready(c, events[0], events[1]) == 0;
        }
        if (!ok) {
            if (c->process != NULL) {
                if (!TerminateJobObject(c->job, 121))
                    abort();
            } else {
                TerminateProcess(process.hProcess, 121);
            }
            WaitForSingleObject(process.hProcess, INFINITE);
            if (c->process != NULL)
                finish_job(c);
            CloseHandle(process.hProcess);
        }
        CloseHandle(process.hThread);
    }
    free(command);
    free(program);
    for (int i = 0; i < 2; i++)
        if (events[i])
            CloseHandle(events[i]);
    if (!ok) {
        if (c->job)
            CloseHandle(c->job);
        free(c->consoles);
    }
    return ok ? 0 : -1;
}
#else
static int start_child(struct legacy_child *c, const char *self, const char *description)
{
    c->pid = fork();
    if (c->pid == 0) {
        execl(self, self, "--legacy-driver", description, (char *)NULL);
        _exit(121);
    }
    return c->pid > 0 ? 0 : -1;
}
#endif

/** Let the parent observe this console before a short controller can exit. */
int legacy_child_ready(void)
{
#ifdef _WIN32
    HANDLE events[2] = {NULL, NULL};
    int ok;

    if (console_events(GetCurrentProcessId(), events, 0) != 0)
        return -1;
    ok = SetEvent(events[0]) && WaitForSingleObject(events[1], INFINITE) == WAIT_OBJECT_0;
    CloseHandle(events[0]);
    CloseHandle(events[1]);
    return ok ? 0 : -1;
#else
    return 0;
#endif
}

struct legacy_child *legacy_child_start(const char *self, const char *description)
{
    struct legacy_child *c = calloc(1, sizeof(*c));
    static int registered;
    if (!registered) {
        atexit(child_cleanup);
        registered = 1;
    }
    if (c == NULL)
        return NULL;
    c->status = -1;
    if (start_child(c, self, description) != 0) {
        free(c);
        return NULL;
    }
    c->next = children;
    children = c;
    return c;
}

int legacy_child_running(struct legacy_child *c)
{
    if (c == NULL || c->ended)
        return 0;
#ifdef _WIN32
    return WaitForSingleObject(c->process, 0) == WAIT_TIMEOUT;
#else
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    return waitid(P_PID, (id_t)c->pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == 0;
#endif
}

#ifdef _WIN32
/** Match the native system console host, never an arbitrary basename. */
static int system_console(const wchar_t *path)
{
    wchar_t system[32768];
    static const wchar_t tail[] = L"\\conhost.exe";
    UINT size = sizeof(system) / sizeof(system[0]);
    UINT n = GetSystemDirectoryW(system, size);

    if (n == 0 || n >= size || n + sizeof(tail) / sizeof(tail[0]) > size)
        return 0;
    memcpy(system + n, tail, sizeof(tail));
    return _wcsicmp(system, path) == 0;
}

/** Whether an earlier query proved this exact Job member's native image. */
static int recorded_console(const struct legacy_child *c, const struct etude_win_ident *id)
{
    for (size_t i = 0; i < c->nconsoles; i++)
        if (c->consoles[i].pid == id->pid && c->consoles[i].created == id->created)
            return 1;
    return 0;
}

/** Describe a member and identify only an exact, owned system console host. */
static int census_member(const struct legacy_child *c, const struct etude_win_ident *id)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id->pid);
    FILETIME times[4];
    wchar_t path[32768];
    DWORD size = sizeof path / sizeof path[0], error = 0, status = STILL_ACTIVE;
    DWORD wait = WAIT_FAILED;
    uint64_t created = 0;
    char *image = NULL;
    const char *state = "unknown";
    int console = 0;

    if (h == NULL) {
        error = GetLastError();
    } else if (!GetProcessTimes(h, &times[0], &times[1], &times[2], &times[3])) {
        error = GetLastError();
    } else {
        created = (uint64_t)times[0].dwHighDateTime << 32 | times[0].dwLowDateTime;
        if (created != 0 && created == id->created) {
            wait = WaitForSingleObject(h, 0);
            state = wait == WAIT_OBJECT_0 ? "ended" : wait == WAIT_TIMEOUT ? "present" : "unknown";
            if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT)
                error = wait == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
            else if (!GetExitCodeProcess(h, &status))
                error = GetLastError();
            else if (!QueryFullProcessImageNameW(h, 0, path, &size)) {
                error = GetLastError();
                console = error == ERROR_ACCESS_DENIED && recorded_console(c, id);
            } else {
                image = etude_win_utf8(path);
                if (image == NULL)
                    error = ERROR_NOT_ENOUGH_MEMORY;
                else
                    console = id->pid != c->pid && system_console(path);
            }
        } else {
            state = "identity changed";
        }
    }
    fprintf(stderr,
            "legacyset: Job member %lu:%llu actual %llu role %s state %s "
            "wait %lu exit %lu image %s query_error %lu recorded_console %d\n",
            (unsigned long)id->pid, (unsigned long long)id->created, (unsigned long long)created,
            id->pid == c->pid ? "controller" : "owned descendant", state, (unsigned long)wait,
            (unsigned long)status, image ? image : "unavailable", (unsigned long)error,
            recorded_console(c, id));
    free(image);
    if (h != NULL)
        CloseHandle(h);
    return console;
}

/** Record console hosts while the controller is held at its entry. */
static int remember_consoles(struct legacy_child *c)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION usage;
    struct etude_win_child job = {0};
    struct etude_win_ident *ids;
    size_t count = 0;
    DWORD error;

    if (!QueryInformationJobObject(c->job, JobObjectBasicAccountingInformation, &usage,
                                   sizeof(usage), NULL))
        return -1;
    ids = calloc(usage.TotalProcesses, sizeof(*ids));
    c->consoles = calloc(usage.TotalProcesses, sizeof(*c->consoles));
    if (ids == NULL || c->consoles == NULL) {
        free(ids);
        return -1;
    }
    job.job = c->job;
    error = etude_win_pids(&job, ids, usage.TotalProcesses, &count);
    if (error == 0)
        for (size_t i = 0; i < count; i++)
            if (census_member(c, &ids[i]))
                c->consoles[c->nconsoles++] = ids[i];
    free(ids);
    if (error != 0 || c->nconsoles == 0) {
        fprintf(stderr, "legacyset: console identity unavailable before release: %lu\n",
                (unsigned long)error);
        return -1;
    }
    return 0;
}

/** Retain the census; accept only known console hosts for owned teardown. */
static int job_census(const struct legacy_child *c,
                      const JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *usage)
{
    struct etude_win_child job = {0};
    size_t count = 0, capacity = usage->TotalProcesses;
    struct etude_win_ident *ids = calloc(capacity, sizeof *ids);
    DWORD error = ERROR_NOT_ENOUGH_MEMORY;
    int consoles;

    job.job = c->job;
    if (ids != NULL)
        error = etude_win_pids(&job, ids, capacity, &count);
    fprintf(stderr,
            "legacyset: controller %lu exited %d; Job active %lu total %lu "
            "observed %lu census_error %lu\n",
            (unsigned long)c->pid, c->status, (unsigned long)usage->ActiveProcesses,
            (unsigned long)usage->TotalProcesses, (unsigned long)count, (unsigned long)error);
    consoles = error == 0 && count != 0;
    for (size_t i = 0; i < count; i++)
        if (!census_member(c, &ids[i]))
            consoles = 0;
    free(ids);
    return consoles;
}

/** Reap the owned Job; any non-console or unknown orphan fails the row. */
static void finish_job(struct legacy_child *c)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION usage;
    if (!QueryInformationJobObject(c->job, JobObjectBasicAccountingInformation, &usage,
                                   sizeof(usage), NULL))
        abort();
    if (usage.ActiveProcesses) {
        if (!job_census(c, &usage))
            c->status = 121;
        if (!TerminateJobObject(c->job, 121))
            abort();
        do {
            Sleep(1); /* interval: wait for the native job's process count */
            if (!QueryInformationJobObject(c->job, JobObjectBasicAccountingInformation, &usage,
                                           sizeof(usage), NULL))
                abort();
        } while (usage.ActiveProcesses);
    }
}
#endif

int legacy_child_wait(struct legacy_child *c)
{
    if (c == NULL)
        return -1;
    if (c->ended)
        return c->status;
#ifdef _WIN32
    DWORD code;
    if (WaitForSingleObject(c->process, INFINITE) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(c->process, &code))
        c->status = (int)code;
    finish_job(c);
#else
    int status;
    pid_t waited;
    do {
        waited = waitpid(c->pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == c->pid && WIFEXITED(status))
        c->status = WEXITSTATUS(status);
#endif
    c->ended = 1;
    return c->status;
}

int legacy_child_stop(struct legacy_child *c)
{
    if (c == NULL || c->ended)
        return -1;
#ifdef _WIN32
    DWORD ids[1];
    int original = GetConsoleProcessList(ids, 1) > 0;
    HANDLE ack = CreateEventW(NULL, TRUE, FALSE, NULL);
    int ok;
    if (ack == NULL)
        return -1;
    FreeConsole();
    if (!AttachConsole(c->pid)) {
        if (original)
            AttachConsole(ATTACH_PARENT_PROCESS);
        if (stop_callback != NULL)
            SetConsoleCtrlHandler(console_stop, TRUE);
        CloseHandle(ack);
        return -1;
    }
    InterlockedExchangePointer(&break_acknowledgement, ack);
    ok = SetConsoleCtrlHandler(ignore_console, TRUE) &&
         GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
    if (ok)
        ok = WaitForSingleObject(ack, INFINITE) == WAIT_OBJECT_0;
    InterlockedExchangePointer(&break_acknowledgement, NULL);
    FreeConsole();
    if (original)
        AttachConsole(ATTACH_PARENT_PROCESS);
    if (stop_callback != NULL)
        SetConsoleCtrlHandler(console_stop, TRUE);
    CloseHandle(ack);
    return ok ? 0 : -1;
#else
    return kill(c->pid, SIGTERM);
#endif
}

void legacy_child_free(struct legacy_child *c)
{
    struct legacy_child **at;
    if (c == NULL)
        return;
    if (!c->ended)
        abort();
    for (at = &children; *at && *at != c; at = &(*at)->next) {
    }
    if (*at)
        *at = c->next;
#ifdef _WIN32
    CloseHandle(c->process);
    CloseHandle(c->job);
    free(c->consoles);
#endif
    free(c);
}
