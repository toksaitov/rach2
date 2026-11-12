/*
 * winjob.c: on the testing host's Windows, run a release part in a named Job
 * Object that ends with its processes, and stop it or say whether it runs.
 *
 * Usage: winjob run NAME TOKEN NONCE PROGRAM [ARG...]
 *        winjob stop NAME
 *        winjob state NAME
 *
 * release.sh starts `run` through WMI's Win32_Process.Create, outside the
 * Job Object of any ssh session. It creates the job NAME, refusing a name
 * that exists, assigns itself to it, and only then reads the file TOKEN: when
 * that does not hold NONCE, a stop came first, and it exits. Otherwise it
 * starts PROGRAM, which is born inside the job with everything it starts,
 * waits for it, and terminates the job with PROGRAM's status. The job ends
 * its processes when its last handle closes, so a helper that dies takes the
 * part with it, and no process may break away.
 *
 * `stop` terminates the job NAME and waits up to half a minute for its last
 * process to end; `state` only reads it. Both print one line: `absent` when
 * no job NAME exists or it has no process left, `active N` while N processes
 * remain, and `unknown` when Windows cannot say. A stopper writes the token
 * before it tests the job, and `run` takes the job before it reads the
 * token, so a start in flight either holds a job that the stop ends or reads
 * a token not its own. Exit 0 on `absent`, 1 otherwise.
 *
 * It builds with the host's MinGW: gcc -static -o winjob.exe winjob.c.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long `stop` waits for the job's processes to end, in milliseconds. */
#define STOP_BOUND 30000

/* The longest command line CreateProcess takes, in characters. */
#define LINE_MAX 32767

/**
 * Print the job's state, `absent`, `active N` or `unknown`, from its handle
 * J, NULL when OpenJobObject failed with ERR. Return 0 on `absent`.
 */
static int report(HANDLE j, DWORD err)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION a;

    if (j == NULL) {
        if (err == ERROR_FILE_NOT_FOUND) {
            puts("absent");
            return 0;
        }
        puts("unknown");
        return 1;
    }
    if (!QueryInformationJobObject(j, JobObjectBasicAccountingInformation, &a, sizeof a, NULL)) {
        puts("unknown");
        return 1;
    }
    if (a.ActiveProcesses == 0) {
        puts("absent");
        return 0;
    }
    printf("active %lu\n", (unsigned long)a.ActiveProcesses);
    return 1;
}

/**
 * Return the number of processes left in job J, or -1 when Windows cannot
 * say.
 */
static long active(HANDLE j)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION a;

    if (!QueryInformationJobObject(j, JobObjectBasicAccountingInformation, &a, sizeof a, NULL))
        return -1;
    return (long)a.ActiveProcesses;
}

/** Terminate the job NAME, wait for it to empty, and print its state. */
static int stop(const char *name)
{
    DWORD waited;
    HANDLE j;
    long n;

    j = OpenJobObjectA(JOB_OBJECT_TERMINATE | JOB_OBJECT_QUERY, FALSE, name);
    if (j == NULL)
        return report(NULL, GetLastError());
    TerminateJobObject(j, 137);
    for (waited = 0; waited < STOP_BOUND; waited += 100) {
        n = active(j);
        if (n <= 0)
            break;
        Sleep(100);
    }
    n = report(j, 0);
    CloseHandle(j);
    return (int)n;
}

/** Print the state of the job NAME. */
static int state(const char *name)
{
    HANDLE j;
    int rc;

    j = OpenJobObjectA(JOB_OBJECT_QUERY, FALSE, name);
    if (j == NULL)
        return report(NULL, GetLastError());
    rc = report(j, 0);
    CloseHandle(j);
    return rc;
}

/**
 * Succeed when the file PATH holds NONCE, alone on its first line; a missing
 * or unreadable file holds nothing. The file is opened sharing delete, so
 * that a stop's rename of a new token over it is not refused while it is
 * read.
 */
static int token_is(const char *path, const char *nonce)
{
    char buf[256];
    DWORD got = 0;
    size_t n;
    HANDLE f;

    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(f, buf, sizeof buf - 1, &got, NULL))
        got = 0;
    CloseHandle(f);
    n = got;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return strcmp(buf, nonce) == 0;
}

/**
 * Append ARG to the command line LINE of SIZE characters, quoted as
 * CommandLineToArgvW reads it. Return -1 when it does not fit.
 */
static int quote(char *line, size_t size, const char *arg)
{
    size_t n = strlen(line), slashes;
    const char *p;
    int plain;

    plain = *arg != '\0' && strpbrk(arg, " \t\"") == NULL;
    if (n + 2 >= size)
        return -1;
    if (n > 0)
        line[n++] = ' ';
    if (!plain)
        line[n++] = '"';
    for (p = arg;; p++) {
        slashes = 0;
        while (*p == '\\') {
            p++;
            slashes++;
        }
        if (*p == '\0' && !plain)
            slashes *= 2;
        else if (*p == '"')
            slashes = slashes * 2 + 1;
        if (n + slashes + 3 >= size)
            return -1;
        while (slashes-- > 0)
            line[n++] = '\\';
        if (*p == '\0')
            break;
        line[n++] = *p;
    }
    if (!plain)
        line[n++] = '"';
    line[n] = '\0';
    return 0;
}

/** Run ARGV[0] in the new job NAME once TOKEN holds NONCE, as `run` says. */
static int run(const char *name, const char *token, const char *nonce, char **argv)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    DWORD code = 1;
    HANDLE j;
    char *line;

    j = CreateJobObjectA(NULL, name);
    if (j == NULL) {
        fprintf(stderr, "winjob: cannot create %s: error %lu\n", name, GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "winjob: %s exists\n", name);
        return 1;
    }
    memset(&lim, 0, sizeof lim);
    lim.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &lim, sizeof lim) ||
        !AssignProcessToJobObject(j, GetCurrentProcess())) {
        fprintf(stderr, "winjob: cannot join %s: error %lu\n", name, GetLastError());
        return 1;
    }
    if (!token_is(token, nonce)) {
        fprintf(stderr, "winjob: %s does not hold %s; the run was stopped\n", token, nonce);
        return 1;
    }
    line = malloc(LINE_MAX);
    if (line == NULL)
        return 1;
    line[0] = '\0';
    for (; *argv != NULL; argv++)
        if (quote(line, LINE_MAX, *argv) != 0) {
            fprintf(stderr, "winjob: the command line is too long\n");
            return 1;
        }
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "winjob: cannot start %s: error %lu\n", line, GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &code))
        code = 1;
    /* This process is in the job too, so this is its end. */
    TerminateJobObject(j, code);
    return (int)code;
}

int main(int argc, char **argv)
{
    if (argc >= 6 && strcmp(argv[1], "run") == 0)
        return run(argv[2], argv[3], argv[4], argv + 5);
    if (argc == 3 && strcmp(argv[1], "stop") == 0)
        return stop(argv[2]);
    if (argc == 3 && strcmp(argv[1], "state") == 0)
        return state(argv[2]);
    fprintf(stderr, "usage: winjob run NAME TOKEN NONCE PROGRAM [ARG...]\n"
                    "       winjob stop NAME\n"
                    "       winjob state NAME\n");
    return 2;
}
