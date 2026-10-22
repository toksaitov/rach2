/*
 * hold.c: the Windows half of etude-hold, as hold.h declares it: a LockFileEx
 * lock for the guard, a Job Object for the command's tree, wide file names.
 */
#include <windows.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <tlhelp32.h>

#include "../hold.h"
#include "winfmt.h"

/* The exit code of a command a stop ended: SIGKILL's, as a shell gives it. */
#define STOP_STATUS 137

/* How long a rename or a remove is tried again while another process has the
 * file open, in milliseconds, and the pause between two tries. */
#define BUSY_BOUND 2000
#define BUSY_PAUSE 20

/* How long hold_wait waits for the rest of the job to end, in milliseconds. */
#define JOB_BOUND 10000

/* The guard's handle, open while this process lives. */
static HANDLE guard = INVALID_HANDLE_VALUE;

/* The command's job, process and first thread, from hold_start. */
static HANDLE job;
static HANDLE process;
static HANDLE thread;

/* The command's job once it has its process, for the console's handler. */
static HANDLE volatile running;

/* The first stop noted, SIGTERM, or 0. */
static LONG volatile stopped;

/* 1 once a stop has ended the command's job. */
static LONG volatile ended;

/** Give the errno value nearest the Win32 error `err`. */
static int win_errno(DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_DIRECTORY:
    case ERROR_BAD_PATHNAME:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return EACCES;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_BAD_EXE_FORMAT:
        return ENOEXEC;
    case ERROR_FILENAME_EXCED_RANGE:
        return ENAMETOOLONG;
    case ERROR_INVALID_PARAMETER:
        return EINVAL;
    default:
        return EIO;
    }
}

/** Set errno from GetLastError, and give -1. */
static int fail(void)
{
    errno = win_errno(GetLastError());
    return -1;
}

/** Convert the UTF-8 string `s` to UTF-16; a string from malloc, or NULL
 * with errno set. */
static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w;

    if (n <= 0) {
        errno = EILSEQ;
        return NULL;
    }
    if ((w = malloc((size_t)n * sizeof *w)) == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
    return w;
}

/** Give the concatenation of `a` and `b` from malloc, or NULL. */
static wchar_t *concat(const wchar_t *a, const wchar_t *b)
{
    size_t na = wcslen(a), nb = wcslen(b);
    wchar_t *w = malloc((na + nb + 1) * sizeof *w);

    if (w == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(w, a, na * sizeof *w);
    memcpy(w + na, b, (nb + 1) * sizeof *w);
    return w;
}

/**
 * Give the name the wide file functions take for the UTF-8 path `path`: its
 * full path with backslashes and the `\\?\` prefix, or `\\?\UNC\` for a
 * share, which lifts the limit of 260 characters.
 *
 * @return a string from malloc, or NULL with errno set.
 */
static wchar_t *path_w(const char *path)
{
    wchar_t *w = wide(path), *full = NULL, *res = NULL, *p;
    DWORD n;

    if (w == NULL)
        return NULL;
    for (p = w; *p != L'\0'; p++)
        if (*p == L'/')
            *p = L'\\';
    if (wcsncmp(w, L"\\\\?\\", 4) == 0)
        return w;
    if ((n = GetFullPathNameW(w, 0, NULL, NULL)) == 0) {
        fail();
    } else if ((full = malloc(n * sizeof *full)) == NULL) {
        errno = ENOMEM;
    } else if (GetFullPathNameW(w, n, full, NULL) >= n) {
        errno = ENAMETOOLONG;
    } else {
        res = wcsncmp(full, L"\\\\", 2) == 0 ? concat(L"\\\\?\\UNC\\", full + 2)
                                             : concat(L"\\\\?\\", full);
    }
    free(full);
    free(w);
    return res;
}

/** Give a FILETIME as one count of 100 ns units. */
static unsigned long long ticks(const FILETIME *ft)
{
    return (unsigned long long)ft->dwHighDateTime << 32 | ft->dwLowDateTime;
}

/** Give the creation time of process `h`, or 0 when it cannot be read. */
static unsigned long long created(HANDLE h)
{
    FILETIME ft[4];

    if (!GetProcessTimes(h, &ft[0], &ft[1], &ft[2], &ft[3]))
        return 0;
    return ticks(&ft[0]);
}

long hold_self(void)
{
    return (long)GetCurrentProcessId();
}

int hold_read(long pid, struct hold_proc *p)
{
    HANDLE h;
    int state;

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (h == NULL)
        return GetLastError() == ERROR_INVALID_PARAMETER ? 0 : -1;
    memset(p, 0, sizeof *p);
    p->pid = pid;
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0)
        state = 0;
    else
        state = (p->start = created(h)) != 0 ? 1 : -1;
    CloseHandle(h);
    return state;
}

/**
 * Give the creation time of process `pid`, or 0 when it cannot be opened or
 * has ended.
 */
static unsigned long long created_pid(DWORD pid)
{
    struct hold_proc p;

    return hold_read((long)pid, &p) == 1 ? p.start : 0;
}

long hold_parent(long pid)
{
    PROCESSENTRY32W e;
    HANDLE snap;
    DWORD parent = 0;
    unsigned long long mine, theirs;

    if (pid <= 0 ||
        (snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)) == INVALID_HANDLE_VALUE)
        return 0;
    e.dwSize = sizeof e;
    if (Process32FirstW(snap, &e)) {
        do {
            if (e.th32ProcessID == (DWORD)pid) {
                parent = e.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    if (parent == 0 || (mine = created_pid((DWORD)pid)) == 0 ||
        (theirs = created_pid(parent)) == 0 || theirs > mine)
        return 0;
    return (long)parent;
}

size_t hold_members(long group, long *pids, size_t max)
{
    (void)group;
    (void)pids;
    (void)max;
    return 0;
}

int hold_guard(const char *path)
{
    OVERLAPPED ov;
    wchar_t *w = path_w(path);
    DWORD err;

    if (w == NULL)
        return -1;
    guard = CreateFileW(w, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (guard == INVALID_HANDLE_VALUE)
        return fail();
    /* The lock is on byte 0 alone, past the end of the empty file. */
    memset(&ov, 0, sizeof ov);
    if (LockFileEx(guard, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov))
        return 0;
    err = GetLastError();
    CloseHandle(guard);
    guard = INVALID_HANDLE_VALUE;
    if (err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING)
        return 1;
    errno = win_errno(err);
    return -1;
}

int hold_mkdir(const char *path)
{
    wchar_t *w = path_w(path);
    int rc;

    if (w == NULL)
        return -1;
    rc = CreateDirectoryW(w, NULL) ? 0 : fail();
    free(w);
    return rc;
}

/** Say whether the Win32 error `err` is one that another process's open
 * handle gives for a while. */
static int busy(DWORD err)
{
    return err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION;
}

int hold_rename(const char *from, const char *to)
{
    wchar_t *wf = path_w(from), *wt = path_w(to);
    DWORD attrs, flags, waited;
    int rc = -1;

    if (wf != NULL && wt != NULL) {
        attrs = GetFileAttributesW(wf);
        /* MoveFileExW refuses MOVEFILE_REPLACE_EXISTING for a directory. */
        flags = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)
                    ? 0
                    : MOVEFILE_REPLACE_EXISTING;
        for (waited = 0;; waited += BUSY_PAUSE) {
            if (MoveFileExW(wf, wt, flags)) {
                rc = 0;
                break;
            }
            if (!busy(GetLastError()) || waited >= BUSY_BOUND) {
                fail();
                break;
            }
            Sleep(BUSY_PAUSE);
        }
    }
    free(wf);
    free(wt);
    return rc;
}

/**
 * Remove the file or tree `w`, a wide path of path_w's form, clearing the
 * read-only attribute of each entry. A link to a directory is removed, not
 * followed.
 */
static void remove_w(const wchar_t *w)
{
    WIN32_FIND_DATAW fd;
    DWORD attrs = GetFileAttributesW(w);
    wchar_t *pattern, *child, *dir;
    HANDLE h;

    if (attrs == INVALID_FILE_ATTRIBUTES)
        return;
    if (attrs & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(w, attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        DeleteFileW(w);
        return;
    }
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT) && (dir = concat(w, L"\\")) != NULL) {
        if ((pattern = concat(dir, L"*")) != NULL &&
            (h = FindFirstFileW(pattern, &fd)) != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;
                if ((child = concat(dir, fd.cFileName)) != NULL) {
                    remove_w(child);
                    free(child);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        free(pattern);
        free(dir);
    }
    RemoveDirectoryW(w);
}

int hold_remove(const char *path)
{
    wchar_t *w = path_w(path);
    DWORD waited;
    int rc = -1;

    if (w == NULL)
        return -1;
    for (waited = 0;; waited += BUSY_PAUSE) {
        remove_w(w);
        if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) {
            rc = 0;
            break;
        }
        if (waited >= BUSY_BOUND) {
            errno = EBUSY;
            break;
        }
        Sleep(BUSY_PAUSE);
    }
    free(w);
    return rc;
}

int hold_young(const char *path, double seconds)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME now;
    wchar_t *w = path_w(path);
    unsigned long long t, n;
    BOOL ok;

    if (w == NULL)
        return 0;
    ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (!ok)
        return 0;
    GetSystemTimeAsFileTime(&now);
    t = ticks(&a.ftLastWriteTime);
    n = ticks(&now);
    return n < t || (double)(n - t) < seconds * 1e7;
}

/** End the command's job for a stop, once. */
static void end_job(void)
{
    HANDLE j = running;

    if (j != NULL && InterlockedExchange(&ended, 1) == 0)
        TerminateJobObject(j, STOP_STATUS);
}

/**
 * The console's handler of hold_catch. A close, logoff or shutdown is a stop:
 * it is noted, and once the command has started its job is ended. The system
 * ends this process when the handler returns from such an event, so the
 * handler waits instead, and hold_wait's return lets the main thread report
 * the stop and exit; the system's own time limit ends the wait otherwise.
 *
 * Ctrl-C and Ctrl-Break are not stops. Every process of the console receives
 * them, the command among them, which answers for itself; the helper goes on
 * waiting for it.
 */
static BOOL WINAPI on_console(DWORD event)
{
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        InterlockedCompareExchange(&stopped, SIGTERM, 0);
        end_job();
        Sleep(INFINITE);
        return TRUE;
    default:
        return FALSE;
    }
}

void hold_catch(void)
{
    SetConsoleCtrlHandler(on_console, TRUE);
}

/* A stop has no number on Windows; hold_stopped gives SIGTERM's for each,
 * so that a stop before the command starts gives 143, as it does on POSIX. */
int hold_stopped(void)
{
    return (int)stopped;
}

/**
 * Find the program `name` names: a name with a directory part as it is, or
 * with `.exe` added when that is missing and the name has no extension; a
 * bare name on the directories of PATH, in order, with `.exe` added as
 * SearchPathW adds it. The current directory and the helper's own are not
 * searched, as execvp does not search them.
 *
 * @return a path from malloc, or NULL with errno set.
 */
static wchar_t *find_program(const char *name)
{
    wchar_t *w = wide(name), *exe, *pathvar = NULL, *found = NULL;
    const wchar_t *base;
    DWORD n;

    if (w == NULL)
        return NULL;
    if (wcspbrk(w, L"\\/:") != NULL) {
        if (GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES)
            return w;
        base = wcsrchr(w, L'\\') != NULL ? wcsrchr(w, L'\\') : w;
        if (wcsrchr(base, L'/') != NULL)
            base = wcsrchr(base, L'/');
        if (wcschr(base, L'.') == NULL && (exe = concat(w, L".exe")) != NULL) {
            if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) {
                free(w);
                return exe;
            }
            free(exe);
        }
        free(w);
        errno = ENOENT;
        return NULL;
    }
    if ((n = GetEnvironmentVariableW(L"PATH", NULL, 0)) > 0 &&
        (pathvar = malloc(n * sizeof *pathvar)) != NULL &&
        GetEnvironmentVariableW(L"PATH", pathvar, n) < n &&
        (n = SearchPathW(pathvar, w, L".exe", 0, NULL, NULL)) > 0 &&
        (found = malloc(n * sizeof *found)) != NULL &&
        SearchPathW(pathvar, w, L".exe", n, found, NULL) >= n) {
        free(found);
        found = NULL;
    }
    free(pathvar);
    free(w);
    if (found == NULL)
        errno = ENOENT;
    return found;
}

/**
 * Start `argv` suspended, inheriting this process's console, environment
 * and inheritable handles, its standard handles among them, whether or not
 * they were inheritable here.
 *
 * @return 0 with the process in `*pi`, or -1 with errno set.
 */
static int create(char *const *argv, PROCESS_INFORMATION *pi)
{
    STARTUPINFOW si;
    HANDLE std[3] = {NULL, NULL, NULL}, self = GetCurrentProcess(), h;
    static const DWORD which[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    wchar_t *prog = NULL, *line = NULL;
    char *text = NULL;
    int i, rc = -1, err;

    if ((prog = find_program(argv[0])) == NULL)
        return -1;
    if ((err = etude_win_cmdline(argv[0], (const char *const *)argv + 1, &text)) != 0) {
        errno = err;
        goto out;
    }
    if ((line = wide(text)) == NULL)
        goto out;
    if (wcslen(line) >= ETUDE_WIN_CMDLINE_MAX) {
        errno = E2BIG;
        goto out;
    }
    for (i = 0; i < 3; i++) {
        h = GetStdHandle(which[i]);
        if (h != NULL && h != INVALID_HANDLE_VALUE &&
            !DuplicateHandle(self, h, self, &std[i], 0, TRUE, DUPLICATE_SAME_ACCESS))
            std[i] = NULL;
    }
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = std[0];
    si.hStdOutput = std[1];
    si.hStdError = std[2];
    if (CreateProcessW(prog, line, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, pi))
        rc = 0;
    else
        fail();
    for (i = 0; i < 3; i++)
        if (std[i] != NULL)
            CloseHandle(std[i]);
out:
    free(text);
    free(line);
    free(prog);
    return rc;
}

/**
 * Make the command's job: its processes die when its last handle closes,
 * which is this process's, and an unhandled exception ends a process without
 * the error report's dialog. No process may break away.
 */
static HANDLE make_job(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    HANDLE j = CreateJobObjectW(NULL, NULL);

    if (j == NULL)
        return NULL;
    memset(&lim, 0, sizeof lim);
    lim.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &lim, sizeof lim)) {
        CloseHandle(j);
        return NULL;
    }
    return j;
}

int hold_start(const char *name, char *const *argv, long *group)
{
    PROCESS_INFORMATION pi;
    DWORD err;

    (void)name;
    *group = 0;
    if ((job = make_job()) == NULL)
        return fail();
    if (create(argv, &pi) != 0) {
        CloseHandle(job);
        job = NULL;
        return -1;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        job = NULL;
        errno = win_errno(err);
        return -1;
    }
    process = pi.hProcess;
    thread = pi.hThread;
    InterlockedExchangePointer((PVOID volatile *)&running, job);
    if (stopped != 0)
        end_job();
    return 0;
}

void hold_go(void)
{
    ResumeThread(thread);
}

void hold_cancel(void)
{
    TerminateJobObject(job, 1);
    WaitForSingleObject(process, INFINITE);
}

/**
 * Wait until the job of the command has no process left, for at most
 * JOB_BOUND milliseconds.
 */
static void wait_empty(void)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION a;
    DWORD waited;

    for (waited = 0; waited < JOB_BOUND; waited += 10) {
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &a, sizeof a,
                                       NULL) ||
            a.ActiveProcesses == 0)
            return;
        Sleep(10);
    }
}

/*
 * The command's first process ends the hold. What it leaves running in its
 * job is ended then, before the lock is released, since nothing may work in
 * the tree once the hold is gone.
 */
int hold_wait(void)
{
    DWORD code = 1;

    WaitForSingleObject(process, INFINITE);
    if (!GetExitCodeProcess(process, &code))
        code = 1;
    TerminateJobObject(job, code);
    wait_empty();
    if (ended)
        return STOP_STATUS;
    return (int)code;
}

/** The console's handler of hold_exec: the command answers Ctrl-C itself. */
static BOOL WINAPI on_console_exec(DWORD event)
{
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}

void hold_exec(char *const *argv)
{
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    if (create(argv, &pi) != 0)
        return;
    SetConsoleCtrlHandler(on_console_exec, TRUE);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    fflush(NULL);
    ExitProcess(code);
}
