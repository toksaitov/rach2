/* The scenario fixture's platform operations, independent of its assertions. */
#ifdef _WIN32
#include "win/files.h"
#include "win/winfmt.h"
#include <process.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scenario_host.h"

int sh_allocated_bytes(const char *path, uint64_t *bytes)
{
#ifdef _WIN32
    wchar_t *wide = etude_win_long_path(path);
    DWORD low, high;
    int rc = 0;
    if (!wide)
        return -1;
    SetLastError(NO_ERROR);
    low = GetCompressedFileSizeW(wide, &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
        rc = etude_win_file_error();
    else
        *bytes = ((uint64_t)high << 32) | low;
    free(wide);
    return rc;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    *bytes = (uint64_t)st.st_blocks * 512u;
    return 0;
#endif
}

int sh_poll(struct sh_poll *items, size_t count, int milliseconds)
{
    size_t i, used = 0;
    int result;
#ifdef _WIN32
    WSAPOLLFD *polls = calloc(count, sizeof(*polls));
#else
    struct pollfd *polls = calloc(count, sizeof(*polls));
#endif
    if (polls == NULL)
        return -1;
    for (i = 0; i < count; i++) {
        items[i].revents = 0;
        if (items[i].fd < 0)
            continue;
        polls[used].fd = items[i].fd;
        polls[used++].events = items[i].events;
    }
#ifdef _WIN32
    if (used == 0) {
        Sleep(milliseconds >= 0 ? (DWORD)milliseconds : INFINITE);
        result = 0;
    } else {
        result = WSAPoll(polls, (ULONG)used, milliseconds);
    }
#else
    result = poll(polls, used, milliseconds);
#endif
    used = 0;
    for (i = 0; i < count; i++)
        if (items[i].fd >= 0)
            items[i].revents = polls[used++].revents;
    free(polls);
    return result;
}

host_socket sh_socket(int type)
{
#ifdef _WIN32
    if (etude_win_net_start() != 0)
        return HOST_SOCKET_INVALID;
#endif
    host_socket value = (host_socket)socket(AF_INET, type, 0);
#ifdef _WIN32
    if (value >= 0 && !SetHandleInformation((HANDLE)(uintptr_t)value, HANDLE_FLAG_INHERIT, 0)) {
        host_socket_close(value);
        return HOST_SOCKET_INVALID;
    }
#endif
    return value;
}

int sh_bound_error(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEADDRINUSE || WSAGetLastError() == WSAEACCES;
#else
    return errno == EADDRINUSE;
#endif
}

#ifdef _WIN32
/* A separate event per process lets the controller request the real C stop
 * handler without terminating unrelated processes in its console session. */
struct stop_request {
    HANDLE event;
    int cooperative;
};
static DWORD WINAPI stop_event(void *argument)
{
    struct stop_request *request = argument;
    HANDLE event = request->event;
    int cooperative = request->cooperative;
    free(request);
    if (WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0) {
        if (cooperative)
            raise(SIGTERM);
        else
            TerminateProcess(GetCurrentProcess(), ETUDE_WIN_STOP_STATUS);
    }
    CloseHandle(event);
    return 0;
}

int sh_arm_stop(int cooperative)
{
    char name[80];
    HANDLE event, thread;
    struct stop_request *request;
    snprintf(name, sizeof(name), "Local\\rach2-scenario-stop-%lu", GetCurrentProcessId());
    event = CreateEventA(NULL, FALSE, FALSE, name);
    if (event == NULL)
        return -1;
    request = malloc(sizeof(*request));
    if (request == NULL) {
        CloseHandle(event);
        return -1;
    }
    request->event = event;
    request->cooperative = cooperative;
    thread = CreateThread(NULL, 0, stop_event, request, 0, NULL);
    if (thread == NULL) {
        free(request);
        CloseHandle(event);
        return -1;
    }
    CloseHandle(thread);
    return 0;
}

int sh_signal(long pid, int signal)
{
    HANDLE handle;
    char name[80];
    int ok;
    if (signal != SIGKILL) {
        snprintf(name, sizeof(name), "Local\\rach2-scenario-stop-%lu", (unsigned long)pid);
        handle = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
        if (handle != NULL) {
            ok = SetEvent(handle);
            CloseHandle(handle);
            return ok ? 0 : -1;
        }
    }
    handle = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (handle == NULL)
        return -1;
    ok = TerminateProcess(handle, ETUDE_WIN_STOP_STATUS);
    CloseHandle(handle);
    return ok ? 0 : -1;
}

int sh_dead_status(void)
{
    return (int)ETUDE_WIN_STOP_STATUS;
}

/* The CRT receives the same UTF-8 environment as a POSIX execve child. */
static wchar_t *child_environment(char **env)
{
    size_t i, count = 1, at = 0;
    wchar_t *block, *value;
    const char **ordered;
    size_t entries;
    if (env == NULL)
        return NULL;
    for (i = 0; env[i] != NULL; i++)
        count += strlen(env[i]) + 1;
    entries = i;
    ordered = calloc(entries + 1, sizeof(*ordered));
    if (ordered == NULL)
        return NULL;
    memcpy(ordered, env, entries * sizeof(*ordered));
    if (etude_win_envsort(ordered, entries) != 0) {
        free(ordered);
        return NULL;
    }
    block = calloc(count + 1, sizeof(*block));
    if (block == NULL) {
        free(ordered);
        return NULL;
    }
    for (i = 0; ordered[i] != NULL; i++) {
        value = etude_win_wide(ordered[i]);
        if (value == NULL) {
            free(block);
            free(ordered);
            return NULL;
        }
        memcpy(block + at, value, (wcslen(value) + 1) * sizeof(*block));
        at += wcslen(value) + 1;
        free(value);
    }
    free(ordered);
    return block;
}

/* Restrict inheritance to these streams, including beside another row. */
static int child_handles(STARTUPINFOEXW *startup)
{
    SIZE_T size = 0;
    HANDLE handles[] = {startup->StartupInfo.hStdInput, startup->StartupInfo.hStdOutput,
                        startup->StartupInfo.hStdError};
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    startup->lpAttributeList = malloc(size);
    if (startup->lpAttributeList == NULL)
        return -1;
    if (!InitializeProcThreadAttributeList(startup->lpAttributeList, 1, 0, &size)) {
        free(startup->lpAttributeList);
        startup->lpAttributeList = NULL;
        return -1;
    }
    return UpdateProcThreadAttribute(startup->lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     handles, sizeof(handles), NULL, NULL)
               ? 0
               : -1;
}

static int child_job(struct sh_child *child)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit;
    memset(&limit, 0, sizeof(limit));
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    child->job = CreateJobObjectW(NULL, NULL);
    return child->job != NULL &&
                   SetInformationJobObject(child->job, JobObjectExtendedLimitInformation, &limit,
                                           sizeof(limit)) &&
                   AssignProcessToJobObject(child->job, child->process)
               ? 0
               : -1;
}

static void close_handle(HANDLE handle)
{
    if (handle != NULL && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
}

int sh_start(struct sh_child *c, const char *self, char *const *argv, char **env, const char *error,
             int owned)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    HANDLE writer = NULL, err = NULL, input = NULL;
    wchar_t *program = NULL, *command = NULL, *environment = NULL, *path = NULL;
    char *line = NULL;
    int ok = 0;
    memset(c, 0, sizeof(*c));
    memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    if (etude_win_cmdline(self, (const char *const *)(argv + 1), &line) != 0)
        goto done;
    program = etude_win_wide(self);
    command = etude_win_wide(line);
    environment = child_environment(env);
    path = error != NULL ? etude_win_long_path(error) : NULL;
    if (program == NULL || command == NULL || (env != NULL && environment == NULL) ||
        (error != NULL && path == NULL) || !CreatePipe(&c->output, &writer, &sa, 0) ||
        !SetHandleInformation(c->output, HANDLE_FLAG_INHERIT, 0))
        goto done;
    err =
        CreateFileW(path != NULL ? path : L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (err == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE)
        goto done;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = writer;
    startup.StartupInfo.hStdError = err;
    if (child_handles(&startup) != 0)
        goto done;
    if (!CreateProcessW(program, command, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        environment, NULL, &startup.StartupInfo, &process))
        goto done;
    c->process = process.hProcess;
    c->pid = (long)process.dwProcessId;
    if ((owned && child_job(c) != 0) || ResumeThread(process.hThread) == (DWORD)-1) {
        TerminateProcess(c->process, 127);
        CloseHandle(process.hThread);
        goto done;
    }
    CloseHandle(process.hThread);
    ok = 1;
done:
    if (startup.lpAttributeList != NULL) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList);
    }
    close_handle(writer);
    close_handle(err);
    close_handle(input);
    free(program);
    free(command);
    free(environment);
    free(path);
    free(line);
    if (!ok) {
        close_handle(c->process);
        close_handle(c->job);
        close_handle(c->output);
        memset(c, 0, sizeof(*c));
        return -1;
    }
    return 0;
}

int sh_read(struct sh_child *child, void *data, size_t size)
{
    DWORD available = 0, got;
    if (!PeekNamedPipe(child->output, NULL, 0, NULL, &available, NULL))
        return -1;
    if (available == 0)
        return 0;
    if (size > available)
        size = available;
    return ReadFile(child->output, data, (DWORD)size, &got, NULL) ? (int)got : -1;
}

int sh_wait(struct sh_child *c)
{
    DWORD status = 127;
    if (c->process != NULL) {
        WaitForSingleObject(c->process, INFINITE);
        GetExitCodeProcess(c->process, &status);
        CloseHandle(c->process);
    }
    if (c->output != NULL)
        CloseHandle(c->output);
    if (c->job != NULL) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION usage;
        /* Closing the fallback job before this query could hide a leak in
         * the scenario's own jobs. Observe their end before releasing it. */
        for (;;) {
            if (!QueryInformationJobObject(c->job, JobObjectBasicAccountingInformation, &usage,
                                           sizeof(usage), NULL)) {
                status = 127;
                break;
            }
            if (usage.ActiveProcesses == 0)
                break;
            host_sleep_ms(10);
        }
        CloseHandle(c->job);
    }
    memset(c, 0, sizeof(*c));
    return status == ETUDE_WIN_STOP_STATUS ? -1 : (int)status;
}

static int socket_bytes(host_socket socket, void *bytes, size_t size, int writing)
{
    char *at = bytes;
    int count;
    while (size > 0) {
        count = writing ? host_socket_write(socket, at, size) : host_socket_read(socket, at, size);
        if (count <= 0)
            return -1;
        at += count;
        size -= (size_t)count;
    }
    return 0;
}

int sh_send_socket(host_socket connection, host_socket socket)
{
    DWORD pid;
    WSAPROTOCOL_INFOW info;
    if (socket_bytes(connection, &pid, sizeof(pid), 0) != 0 ||
        WSADuplicateSocketW((SOCKET)socket, pid, &info) != 0)
        return -1;
    return socket_bytes(connection, &info, sizeof(info), 1);
}

host_socket sh_receive_socket(host_socket connection)
{
    DWORD pid = GetCurrentProcessId();
    WSAPROTOCOL_INFOW info;
    if (socket_bytes(connection, &pid, sizeof(pid), 1) != 0 ||
        socket_bytes(connection, &info, sizeof(info), 0) != 0)
        return HOST_SOCKET_INVALID;
    return (host_socket)WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                   &info, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}
#else
int sh_signal(long pid, int signal)
{
    return kill((pid_t)pid, signal);
}
int sh_arm_stop(int cooperative)
{
    (void)cooperative;
    return 0;
}
#endif
