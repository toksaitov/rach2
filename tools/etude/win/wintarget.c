/*
 * wintarget.c: the program the fixtures of etude's Windows adapter start. Its
 * first argument chooses what it does: exit, crash, report its arguments,
 * directory, environment or job, use processor time or memory, start a tree
 * of its own copies, leave children and grandchildren behind, try to leave
 * its job, or connect to a socket.
 *
 * It writes its reports with WriteFile, so that the C library's text mode does
 * not turn a newline into two bytes, and it parses its arguments with the
 * UCRT's own rules, which are the rules etude_win_cmdline quotes for.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The most copies `tree` and `hangtree` start. */
#define MAX_CHILDREN 16

/** Write N bytes of S to the standard stream H whole. */
static void put(DWORD h, const char *s, size_t n)
{
    HANDLE out = GetStdHandle(h);
    DWORD done;

    while (n > 0) {
        if (!WriteFile(out, s, n > 65536 ? 65536 : (DWORD)n, &done, NULL))
            ExitProcess(90);
        s += done;
        n -= done;
    }
}

/** Write a formatted line to the standard output. */
static void say(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof line)
        ExitProcess(91);
    put(STD_OUTPUT_HANDLE, line, (size_t)n);
}

/** Return W in UTF-8, from a static buffer, or exit when it does not fit. */
static const char *utf8(const wchar_t *w)
{
    static char buf[4096];

    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, NULL, NULL) == 0)
        ExitProcess(92);
    return buf;
}

/** The processor time this process has used, in microseconds. */
static unsigned long long cpu_us(void)
{
    FILETIME ft[4];

    GetProcessTimes(GetCurrentProcess(), &ft[0], &ft[1], &ft[2], &ft[3]);
    return (((unsigned long long)ft[2].dwHighDateTime << 32 | ft[2].dwLowDateTime) +
            ((unsigned long long)ft[3].dwHighDateTime << 32 | ft[3].dwLowDateTime)) /
           10u;
}

/** Spin until this process has used MS milliseconds of processor time. */
static void burn(unsigned long ms)
{
    while (cpu_us() < (unsigned long long)ms * 1000u)
        ;
    say("burned %llu\n", cpu_us());
}

/** Commit MB mebibytes and write to every page, so that each is charged. */
static void touch(unsigned long mb)
{
    size_t size = (size_t)mb << 20;
    size_t i;
    char *p;

    p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p == NULL)
        ExitProcess(93);
    for (i = 0; i < size; i += 4096)
        p[i] = 1;
}

/**
 * Start a copy of this program with the arguments ARGS, inheriting the
 * standard handles and every other inheritable handle, with FLAGS added to
 * the creation flags.
 *
 * @return the process handle, or NULL with the error in GetLastError.
 */
static HANDLE spawn(const wchar_t *args, DWORD flags)
{
    wchar_t self[MAX_PATH];
    wchar_t line[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (GetModuleFileNameW(NULL, self, MAX_PATH) == 0)
        return NULL;
    swprintf(line, sizeof line / sizeof line[0], L"\"%ls\" %ls", self, args);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(self, line, NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi))
        return NULL;
    CloseHandle(pi.hThread);
    say("child %lu\n", (unsigned long)pi.dwProcessId);
    return pi.hProcess;
}

/** Start N copies with ARGS; exit when one cannot start. */
static void spawn_n(HANDLE *h, int n, const wchar_t *args)
{
    int i;

    if (n < 1 || n > MAX_CHILDREN)
        ExitProcess(94);
    for (i = 0; i < n; i++) {
        h[i] = spawn(args, 0);
        if (h[i] == NULL)
            ExitProcess(95);
    }
}

/** Connect to PORT on 127.0.0.1 and send TEXT. */
static int connect_send(unsigned short port, const char *text)
{
    struct sockaddr_in a;
    WSADATA d;
    SOCKET s;

    if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
        return 96;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return 97;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0 ||
        send(s, text, (int)strlen(text), 0) != (int)strlen(text))
        return 98;
    shutdown(s, SD_SEND);
    closesocket(s);
    return 0;
}

/** Copy the standard input to the standard output. */
static void cat(void)
{
    char buf[4096];
    DWORD n;

    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, sizeof buf, &n, NULL) && n > 0)
        put(STD_OUTPUT_HANDLE, buf, n);
}

/** Write N bytes of a pattern that a reader can check by offset. */
static void big(unsigned long n)
{
    char buf[4096] = {0};
    unsigned long off = 0;
    size_t i;
    size_t k;

    while (off < n) {
        k = n - off < sizeof buf ? n - off : sizeof buf;
        for (i = 0; i < k; i++)
            buf[i] = (char)('a' + (off + i) % 23);
        put(STD_OUTPUT_HANDLE, buf, k);
        off += (unsigned long)k;
    }
    put(STD_ERROR_HANDLE, "big done\n", 9);
}

/** exit N: exit with status N. */
static int do_exit(int argc, wchar_t **argv)
{
    (void)argc;
    return _wtoi(argv[2]);
}

/** crash: raise an access violation that nothing handles. */
static int do_crash(int argc, wchar_t **argv)
{
    (void)argc;
    (void)argv;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    return 99;
}

/** args ARG...: print each argument as the UCRT parsed it. */
static int do_args(int argc, wchar_t **argv)
{
    int i;

    for (i = 2; i < argc; i++)
        say("arg %d %s\n", i - 2, utf8(argv[i]));
    say("args %d\n", argc - 2);
    return 0;
}

/** cwd: print the working directory. */
static int do_cwd(int argc, wchar_t **argv)
{
    wchar_t value[1024];
    DWORD n;

    (void)argc;
    (void)argv;
    n = GetCurrentDirectoryW(sizeof value / sizeof value[0], value);
    if (n == 0 || n >= sizeof value / sizeof value[0])
        return 3;
    say("cwd %s\n", utf8(value));
    return 0;
}

/** env NAME...: print each variable's value, or that it is unset. */
static int do_env(int argc, wchar_t **argv)
{
    wchar_t value[1024];
    DWORD n;
    int i;

    for (i = 2; i < argc; i++) {
        n = GetEnvironmentVariableW(argv[i], value, sizeof value / sizeof value[0]);
        if (n == 0 || n >= sizeof value / sizeof value[0]) {
            say("env %s unset\n", utf8(argv[i]));
            continue;
        }
        say("env %s=", utf8(argv[i]));
        say("%s\n", utf8(value));
    }
    return 0;
}

/** injob: print whether this process is in a job. */
static int do_injob(int argc, wchar_t **argv)
{
    BOOL in;

    (void)argc;
    (void)argv;
    if (!IsProcessInJob(GetCurrentProcess(), NULL, &in))
        return 3;
    say("injob %d\n", in ? 1 : 0);
    return 0;
}

/** burn MS: use MS milliseconds of processor time. */
static int do_burn(int argc, wchar_t **argv)
{
    (void)argc;
    burn(wcstoul(argv[2], NULL, 10));
    return 0;
}

/** tree N MS: start N copies that each burn MS, wait for them, and burn MS. */
static int do_tree(int argc, wchar_t **argv)
{
    HANDLE h[MAX_CHILDREN];
    wchar_t args[256];

    (void)argc;
    swprintf(args, sizeof args / sizeof args[0], L"burn %ls", argv[3]);
    spawn_n(h, _wtoi(argv[2]), args);
    WaitForMultipleObjects((DWORD)_wtoi(argv[2]), h, TRUE, INFINITE);
    burn(wcstoul(argv[3], NULL, 10));
    return 0;
}

/** memory A B: hold A MiB while a copy holds B MiB. */
static int do_memory(int argc, wchar_t **argv)
{
    HANDLE h[1];
    wchar_t args[256];

    (void)argc;
    touch(wcstoul(argv[2], NULL, 10));
    swprintf(args, sizeof args / sizeof args[0], L"touch %ls", argv[3]);
    spawn_n(h, 1, args);
    WaitForSingleObject(h[0], INFINITE);
    return 0;
}

/** touch MB: hold MB MiB and exit. */
static int do_touch(int argc, wchar_t **argv)
{
    (void)argc;
    touch(wcstoul(argv[2], NULL, 10));
    return 0;
}

/** hold MB MS: hold MB MiB, burn MS, say `ready`, and hang. */
static int do_hold(int argc, wchar_t **argv)
{
    (void)argc;
    touch(wcstoul(argv[2], NULL, 10));
    burn(wcstoul(argv[3], NULL, 10));
    say("ready\n");
    Sleep(INFINITE);
    return 0;
}

/** hang: wait for ever. */
static int do_hang(int argc, wchar_t **argv)
{
    (void)argc;
    (void)argv;
    Sleep(INFINITE);
    return 0;
}

/** hangtree N: start N copies that hang, say `ready`, and hang. */
static int do_hangtree(int argc, wchar_t **argv)
{
    HANDLE h[MAX_CHILDREN];

    (void)argc;
    spawn_n(h, _wtoi(argv[2]), L"hang");
    say("ready\n");
    Sleep(INFINITE);
    return 0;
}

/** breakaway: try to start a copy outside the job, and say what happened. */
static int do_breakaway(int argc, wchar_t **argv)
{
    HANDLE h;

    (void)argc;
    (void)argv;
    h = spawn(L"exit 0", CREATE_BREAKAWAY_FROM_JOB);
    if (h == NULL) {
        say("breakaway denied %lu\n", (unsigned long)GetLastError());
        return 0;
    }
    WaitForSingleObject(h, INFINITE);
    say("breakaway allowed\n");
    return 0;
}

/**
 * late TEXT: start a copy that writes TEXT after this process has exited,
 * and exit at once. The copy waits on a handle of this process that it
 * inherits, so that a reused process ID cannot mislead it.
 */
static int do_late(int argc, wchar_t **argv)
{
    HANDLE h[1];
    wchar_t args[256];
    HANDLE dup;

    (void)argc;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &dup,
                         SYNCHRONIZE, TRUE, 0))
        return 3;
    swprintf(args, sizeof args / sizeof args[0], L"after %llu %ls",
             (unsigned long long)(ULONG_PTR)dup, argv[2]);
    spawn_n(h, 1, args);
    return 0;
}

/** after HANDLE TEXT: wait for the process HANDLE names, then write TEXT. */
static int do_after(int argc, wchar_t **argv)
{
    HANDLE dup = (HANDLE)(ULONG_PTR)wcstoull(argv[2], NULL, 10);

    (void)argc;
    if (WaitForSingleObject(dup, INFINITE) != WAIT_OBJECT_0)
        return 3;
    say("%s\n", utf8(argv[3]));
    return 0;
}

/** orphan: start a copy that hangs, report it in a `child` record, and exit. */
static int do_orphan(int argc, wchar_t **argv)
{
    HANDLE h;

    (void)argc;
    (void)argv;
    h = spawn(L"hang", 0);
    if (h == NULL)
        return 3;
    say("@etude 1 child %lu\n", (unsigned long)GetProcessId(h));
    return 0;
}

/** parent EVENT: start a copy that hangs, set the inherited EVENT, and hang. */
static int do_parent(int argc, wchar_t **argv)
{
    HANDLE ev = (HANDLE)(ULONG_PTR)wcstoull(argv[2], NULL, 10);

    (void)argc;
    if (spawn(L"hang", 0) == NULL || !SetEvent(ev))
        return 3;
    Sleep(INFINITE);
    return 0;
}

/**
 * family N END [GO]: start N copies that each start a copy that hangs, and
 * once every grandchild runs, report the first copy in a `child` record, say
 * `ready`, wait for the named event GO to be set when it is given, and END:
 * `exit`, `hang` or `crash`.
 */
static int do_family(int argc, wchar_t **argv)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE ev[MAX_CHILDREN];
    HANDLE first = NULL;
    HANDLE go;
    HANDLE h;
    wchar_t args[64];
    int n = _wtoi(argv[2]);
    int i;

    if (n < 1 || n > MAX_CHILDREN)
        return 3;
    for (i = 0; i < n; i++) {
        ev[i] = CreateEventW(&sa, TRUE, FALSE, NULL);
        if (ev[i] == NULL)
            return 3;
        swprintf(args, sizeof args / sizeof args[0], L"parent %llu",
                 (unsigned long long)(ULONG_PTR)ev[i]);
        h = spawn(args, 0);
        if (h == NULL)
            return 3;
        if (first == NULL)
            first = h;
    }
    if (WaitForMultipleObjects((DWORD)n, ev, TRUE, INFINITE) >= WAIT_OBJECT_0 + (DWORD)n)
        return 3;
    say("@etude 1 child %lu\n", (unsigned long)GetProcessId(first));
    say("ready\n");
    if (argc > 4) {
        go = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
        if (go == NULL || WaitForSingleObject(go, INFINITE) != WAIT_OBJECT_0)
            return 3;
    }
    if (wcscmp(argv[3], L"crash") == 0)
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
    if (wcscmp(argv[3], L"hang") == 0)
        Sleep(INFINITE);
    return 0;
}

/** connect PORT TEXT: send TEXT to PORT on 127.0.0.1. */
static int do_connect(int argc, wchar_t **argv)
{
    (void)argc;
    return connect_send((unsigned short)_wtoi(argv[2]), utf8(argv[3]));
}

/** cat: copy the standard input to the standard output. */
static int do_cat(int argc, wchar_t **argv)
{
    (void)argc;
    (void)argv;
    cat();
    return 0;
}

/** big N: write N bytes of a pattern, then a line to the errors. */
static int do_big(int argc, wchar_t **argv)
{
    (void)argc;
    big(wcstoul(argv[2], NULL, 10));
    return 0;
}

/* The modes: the first argument, the count of arguments after it at least,
 * and what it does. */
static const struct {
    const wchar_t *name;
    int args;
    int (*run)(int argc, wchar_t **argv);
} modes[] = {
    {L"exit", 1, do_exit},
    {L"crash", 0, do_crash},
    {L"args", 0, do_args},
    {L"cwd", 0, do_cwd},
    {L"env", 0, do_env},
    {L"injob", 0, do_injob},
    {L"burn", 1, do_burn},
    {L"tree", 2, do_tree},
    {L"memory", 2, do_memory},
    {L"touch", 1, do_touch},
    {L"hang", 0, do_hang},
    {L"hangtree", 1, do_hangtree},
    {L"breakaway", 0, do_breakaway},
    {L"late", 1, do_late},
    {L"after", 2, do_after},
    {L"orphan", 0, do_orphan},
    {L"parent", 1, do_parent},
    {L"family", 2, do_family},
    {L"connect", 2, do_connect},
    {L"cat", 0, do_cat},
    {L"big", 1, do_big},
    {L"hold", 2, do_hold},
};

int wmain(int argc, wchar_t **argv)
{
    size_t i;

    for (i = 0; argc >= 2 && i < sizeof modes / sizeof modes[0]; i++) {
        if (wcscmp(argv[1], modes[i].name) == 0 && argc >= 2 + modes[i].args)
            return modes[i].run(argc, argv);
    }
    return 2;
}
