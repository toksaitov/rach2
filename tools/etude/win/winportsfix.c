/*
 * winportsfix.c: the fixtures of `etude lock ports` and of the host lock
 * that `etude admit` takes on Windows. They run etude.exe, beside
 * winfixture.exe, with wintarget.exe as its command, and check the blocks
 * and the host lock it takes by LockFileEx on the same files, as
 * tests/host/etude_ports_hosttest.c and etude_bridge_hosttest.c check them
 * on POSIX systems.
 */
#include <stdio.h>
#include <string.h>

#include "winexec.h"
#include "winfixture.h"

/*
 * One run of `etude lock ports`, with the parent's ends of its standard
 * streams.
 */
struct run {
    HANDLE process; /* NULL until started */
    HANDLE in;      /* closing it ends the command's input */
    HANDLE out;     /* the command's output and errors */
    char text[2048];
    size_t len;
};

/* etude.exe, the directory of its lock files, and that of its ledger. */
static char etude[MAX_PATH * 4 + 32], dir[MAX_PATH * 4 + 32], ledger[MAX_PATH * 4 + 32];

/* The runs of the fixture, which it ends whatever its result. */
static struct run runs[3];

/**
 * Start etude.exe with the arguments `args`, and RACH2_HOST_LOCK set to
 * `host`, or unset for NULL. Only the child's ends of the pipes are
 * inheritable, and only until it has started, so that no other run holds
 * them open.
 */
static int start(struct run *r, const char *host, const char *args)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE in, out;
    wchar_t line[4096];
    char text[4096];
    BOOL ok;

    memset(r, 0, sizeof(*r));
    if (!CreatePipe(&in, &r->in, &sa, 0))
        return -1;
    if (!CreatePipe(&r->out, &out, &sa, 0)) {
        CloseHandle(in);
        CloseHandle(r->in);
        r->in = NULL;
        return -1;
    }
    SetHandleInformation(in, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    snprintf(text, sizeof text, "\"%s\" %s", etude, args);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, line, sizeof line / sizeof line[0]);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = out;
    SetEnvironmentVariableA("RACH2_HOST_LOCK", host);
    ok = CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA("RACH2_HOST_LOCK", NULL);
    CloseHandle(in);
    CloseHandle(out);
    if (!ok)
        return -1;
    CloseHandle(pi.hThread);
    r->process = pi.hProcess;
    return 0;
}

/** Start `etude lock ports` on two blocks, to run `command`. */
static int spawn(struct run *r, const char *host, const char *command)
{
    char args[4096];

    snprintf(args, sizeof args, "lock ports \"%s\" 2 %s", dir, command);
    return start(r, host, args);
}

/** Read the run's output until it has `n` lines, within the bound. */
static int lines(struct run *r, int n)
{
    DWORD avail, got;
    int waited, seen;
    size_t i;

    for (waited = 0; waited < BOUND; waited += 20) {
        for (seen = 0, i = 0; i < r->len; i++)
            seen += r->text[i] == '\n';
        if (seen >= n)
            return 1;
        if (PeekNamedPipe(r->out, NULL, 0, NULL, &avail, NULL) && avail > 0 &&
            r->len + 1 < sizeof r->text &&
            ReadFile(r->out, r->text + r->len, sizeof r->text - r->len - 1, &got, NULL)) {
            r->len += got;
            r->text[r->len] = '\0';
            continue;
        }
        Sleep(20);
    }
    return 0;
}

/** End the run's input, and give its exit status within the bound, or -1. */
static long finish(struct run *r)
{
    DWORD code;

    if (r->in != NULL)
        CloseHandle(r->in);
    r->in = NULL;
    if (r->process == NULL || WaitForSingleObject(r->process, BOUND) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(r->process, &code))
        return -1;
    return (long)code;
}

/** Read what an ended run wrote and left in its pipe. */
static void drain(struct run *r)
{
    DWORD avail, got;

    while (PeekNamedPipe(r->out, NULL, 0, NULL, &avail, NULL) && avail > 0 &&
           r->len + 1 < sizeof r->text &&
           ReadFile(r->out, r->text + r->len, sizeof r->text - r->len - 1, &got, NULL)) {
        r->len += got;
        r->text[r->len] = '\0';
    }
}

/** Whether the lock file `name` takes a lock now: 1, 0 when another process
 * holds one that conflicts, or -1. */
static int lockable(const char *name, int exclusive)
{
    char path[MAX_PATH * 4 + 64];
    OVERLAPPED ov;
    HANDLE h;
    int took;

    snprintf(path, sizeof path, "%s\\%s", dir, name);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    memset(&ov, 0, sizeof ov);
    took = LockFileEx(h, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0,
                      MAXDWORD, MAXDWORD, &ov) != 0;
    if (took)
        UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
    else if (GetLastError() != ERROR_LOCK_VIOLATION)
        took = -1;
    CloseHandle(h);
    return took;
}

/** The checks of the fixture; the caller ends the runs. */
static int check(void)
{
    char envcat[MAX_PATH * 4 + 64], exit7[MAX_PATH * 4 + 16];
    struct run *a = &runs[0], *b = &runs[1], *c = &runs[2];
    long status;

    snprintf(envcat, sizeof envcat, "\"%s\" envcat RACH2_PORT_OFFSET RACH2_HOST_LOCK", target);
    snprintf(exit7, sizeof exit7, "\"%s\" exit 7", target);
    CHECK(spawn(a, "shared", envcat) == 0, "etude lock ports cannot be started: %lu",
          (unsigned long)GetLastError());
    CHECK(lines(a, 2), "the first run's command did not report: %s", a->text);
    CHECK(strstr(a->text, "env RACH2_PORT_OFFSET=0\n") != NULL &&
              strstr(a->text, "env RACH2_HOST_LOCK unset\n") != NULL,
          "the first run was given %s", a->text);
    CHECK(lockable("rach2-ports.0", 1) == 0, "block 0 is not held while its command runs");
    CHECK(lockable("rach2-host.lock", 1) == 0 && lockable("rach2-host.lock", 0) == 1,
          "the host lock is not held shared while the command runs");
    CHECK(spawn(b, "shared", envcat) == 0, "etude lock ports cannot be started again");
    CHECK(lines(b, 2) && strstr(b->text, "env RACH2_PORT_OFFSET=1000\n") != NULL,
          "the second run was not given block 1: %s", b->text);
    CHECK(spawn(c, "alone", exit7) == 0, "etude lock ports cannot be started alone");
    CHECK(WaitForSingleObject(c->process, 600) == WAIT_TIMEOUT,
          "a run that needs the host alone did not wait for the shared runs");
    CHECK(finish(a) == 0 && finish(b) == 0, "the shared runs did not end with status 0");
    CHECK((status = finish(c)) == 7, "the run alone ended with %ld, not its command's 7", status);
    CHECK(lockable("rach2-ports.0", 1) == 1 && lockable("rach2-ports.1", 1) == 1 &&
              lockable("rach2-host.gate", 1) == 1 && lockable("rach2-host.lock", 1) == 1,
          "a lock outlived its run");
    return 0;
}

/** End the fixture's runs, whatever their state. */
static void end_runs(void)
{
    size_t i;

    for (i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        if (runs[i].in != NULL)
            CloseHandle(runs[i].in);
        if (runs[i].process != NULL && WaitForSingleObject(runs[i].process, BOUND) != WAIT_OBJECT_0)
            TerminateProcess(runs[i].process, 1);
        if (runs[i].process != NULL)
            CloseHandle(runs[i].process);
        if (runs[i].out != NULL)
            CloseHandle(runs[i].out);
        memset(&runs[i], 0, sizeof runs[i]);
    }
}

int fx_ports(void)
{
    int rc;

    snprintf(etude, sizeof etude, "%s\\etude.exe", binary_dir);
    snprintf(dir, sizeof dir, "%s\\ports", scratch);
    CreateDirectoryA(dir, NULL);
    rc = check();
    end_runs();
    return rc;
}

/**
 * Start `etude admit` of a shared claim ID that waits at most `wait`
 * seconds, to run wintarget.exe's `exit 7`.
 */
static int admit(struct run *r, const char *id, const char *wait)
{
    char args[4096];

    snprintf(args, sizeof args,
             "admit -d \"%s\" -w %s \"%s\" %s fixture shared 1000,1048576,1048576 "
             "4000,4294967296,4294967296 60 \"%s\" exit 7",
             dir, wait, ledger, id, target);
    return start(r, NULL, args);
}

/** The checks of the admission fixture, with the host lock held by `old`. */
static int admit_check(HANDLE old)
{
    struct run *a = &runs[0], *b = &runs[1];
    OVERLAPPED ov;
    long status;

    memset(&ov, 0, sizeof ov);
    CHECK(LockFileEx(old, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov),
          "the old run's host lock cannot be taken: %lu", (unsigned long)GetLastError());
    CHECK(admit(a, "fixture:wait", "30") == 0, "etude admit cannot be started: %lu",
          (unsigned long)GetLastError());
    CHECK(WaitForSingleObject(a->process, 1500) == WAIT_TIMEOUT,
          "a launch did not wait for the host lock: %ld", finish(a));
    CHECK(admit(b, "fixture:late", "1") == 0, "etude admit cannot be started again");
    status = finish(b);
    drain(b);
    CHECK(status == 2 && strstr(b->text, "refused: locks: held by other runs for 1 s") != NULL,
          "a launch past its wait ended with %ld: %s", status, b->text);
    memset(&ov, 0, sizeof ov);
    CHECK(UnlockFileEx(old, 0, MAXDWORD, MAXDWORD, &ov), "the old run's host lock stayed");
    status = finish(a);
    drain(a);
    CHECK(status == 7, "the waiting launch ended with %ld, not its command's 7: %s", status,
          a->text);
    CHECK(lockable("rach2-host.gate", 1) == 1 && lockable("rach2-host.lock", 1) == 1,
          "a lock outlived its launch");
    return 0;
}

int fx_admit_locks(void)
{
    char path[MAX_PATH * 4 + 64];
    HANDLE old;
    int rc;

    snprintf(etude, sizeof etude, "%s\\etude.exe", binary_dir);
    snprintf(dir, sizeof dir, "%s\\admit-locks", scratch);
    snprintf(ledger, sizeof ledger, "%s\\admit-ledger", scratch);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\rach2-host.lock", dir);
    old = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (old == INVALID_HANDLE_VALUE)
        return failed("the host lock file cannot be made: %lu", (unsigned long)GetLastError());
    rc = admit_check(old);
    CloseHandle(old);
    end_runs();
    return rc;
}
