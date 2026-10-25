/* Native reexecution and real stop delivery for one set-driver child. */
#ifdef _WIN32
#include "win/files.h"
#include "win/winfmt.h"
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
static BOOL WINAPI ignore_console(DWORD event)
{
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
/** Give this controller a separate console without visible windows. */
static int start_child(struct legacy_child *c, const char *self, const char *description)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
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
            ok = ResumeThread(process.hThread) != (DWORD)-1;
        }
        if (!ok) {
            TerminateProcess(process.hProcess, 121);
            WaitForSingleObject(process.hProcess, INFINITE);
            CloseHandle(process.hProcess);
        }
        CloseHandle(process.hThread);
    }
    free(command);
    free(program);
    if (!ok && c->job)
        CloseHandle(c->job);
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
/** Describe an observed Job member without treating an unknown one as gone. */
static void census_member(const struct legacy_child *c, const struct etude_win_ident *id)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id->pid);
    FILETIME times[4];
    wchar_t path[32768];
    DWORD size = sizeof path / sizeof path[0], error = 0, status = STILL_ACTIVE;
    uint64_t created = 0;
    char *image = NULL;
    const char *state = "unknown";

    if (h == NULL) {
        error = GetLastError();
    } else if (!GetProcessTimes(h, &times[0], &times[1], &times[2], &times[3])) {
        error = GetLastError();
    } else {
        created = (uint64_t)times[0].dwHighDateTime << 32 | times[0].dwLowDateTime;
        if (created == id->created) {
            state = WaitForSingleObject(h, 0) == WAIT_OBJECT_0 ? "ended" : "present";
            if (!GetExitCodeProcess(h, &status) || !QueryFullProcessImageNameW(h, 0, path, &size))
                error = GetLastError();
            else
                image = etude_win_utf8(path);
        } else {
            state = "identity changed";
        }
    }
    fprintf(stderr,
            "legacyset: Job member %lu:%llu actual %llu role %s state %s "
            "exit %lu image %s query_error %lu\n",
            (unsigned long)id->pid, (unsigned long long)id->created, (unsigned long long)created,
            id->pid == c->pid ? "controller" : "owned descendant", state, (unsigned long)status,
            image ? image : "unavailable", (unsigned long)error);
    free(image);
    if (h != NULL)
        CloseHandle(h);
}

/** Retain the native status and exact member census before forced cleanup. */
static void job_census(const struct legacy_child *c,
                       const JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *usage)
{
    struct etude_win_child job = {0};
    size_t count = 0, capacity = usage->TotalProcesses;
    struct etude_win_ident *ids = calloc(capacity, sizeof *ids);
    DWORD error = ERROR_NOT_ENOUGH_MEMORY;

    job.job = c->job;
    if (ids != NULL)
        error = etude_win_pids(&job, ids, capacity, &count);
    fprintf(stderr,
            "legacyset: controller %lu exited %d; Job active %lu total %lu "
            "observed %lu census_error %lu\n",
            (unsigned long)c->pid, c->status, (unsigned long)usage->ActiveProcesses,
            (unsigned long)usage->TotalProcesses, (unsigned long)count, (unsigned long)error);
    for (size_t i = 0; i < count; i++)
        census_member(c, &ids[i]);
    free(ids);
}

/** Reap the whole owned job; an orphan makes the controller row fail. */
static void finish_job(struct legacy_child *c)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION usage;
    if (!QueryInformationJobObject(c->job, JobObjectBasicAccountingInformation, &usage,
                                   sizeof(usage), NULL))
        abort();
    if (usage.ActiveProcesses) {
        job_census(c, &usage);
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
    int ok;
    FreeConsole();
    if (!AttachConsole(c->pid)) {
        if (original)
            AttachConsole(ATTACH_PARENT_PROCESS);
        return -1;
    }
    SetConsoleCtrlHandler(ignore_console, TRUE);
    ok = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
    FreeConsole();
    if (original)
        AttachConsole(ATTACH_PARENT_PROCESS);
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
#endif
    free(c);
}
