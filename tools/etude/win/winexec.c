/*
 * winexec.c: etude's Windows processes. Each child starts suspended, enters a
 * Job Object of its own that it cannot leave, is registered and then runs;
 * one completion port carries its output and its job's messages.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "winexec.h"
#include "winfmt.h"

/* The longest wait of etude_win_poll between two questions to every job. */
#define POLL_SLICE 100

/* The completions etude_win_poll takes from the port at once. */
#define POLL_BATCH 16

/** Close HANDLE when it is open, and mark it closed. */
static void close_handle(HANDLE *h)
{
    if (*h != NULL && *h != INVALID_HANDLE_VALUE)
        CloseHandle(*h);
    *h = INVALID_HANDLE_VALUE;
}

DWORD etude_win_sup_open(struct etude_win_sup *s)
{
    memset(s, 0, sizeof *s);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    s->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (s->port == NULL)
        return GetLastError();
    return 0;
}

/**
 * Queue the next read of P. A read that completes at once still posts its
 * completion to the port, so every queued read ends in etude_win_poll.
 */
static void read_next(struct etude_win_pipe *p)
{
    DWORD err;

    memset(&p->ov, 0, sizeof p->ov);
    if (ReadFile(p->handle, p->buf, sizeof p->buf, NULL, &p->ov)) {
        p->pending = 1;
        return;
    }
    err = GetLastError();
    if (err == ERROR_IO_PENDING) {
        p->pending = 1;
        return;
    }
    if (err != ERROR_BROKEN_PIPE)
        p->error = err;
    p->eof = 1;
}

/**
 * Make the pipe of stream S of child C: an overlapped server end that the
 * adapter reads, and a client end for the child to inherit, in `*client`.
 */
static DWORD make_pipe(struct etude_win_sup *s, struct etude_win_child *c, enum etude_win_stream st,
                       HANDLE *client)
{
    struct etude_win_pipe *p = st == ETUDE_WIN_STDOUT ? &c->out : &c->err;
    SECURITY_ATTRIBUTES sa;
    wchar_t name[96];

    p->child = c;
    p->stream = st;
    swprintf(name, sizeof name / sizeof name[0], L"\\\\.\\pipe\\etude-%lu-%lu-%d",
             (unsigned long)GetCurrentProcessId(), s->serial, (int)st);
    p->handle = CreateNamedPipeW(
        name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0,
        ETUDE_WIN_READ_MAX, 0, NULL);
    if (p->handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    *client =
        CreateFileW(name, GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, &sa, OPEN_EXISTING, 0, NULL);
    if (*client == INVALID_HANDLE_VALUE)
        return GetLastError();
    return 0;
}

/**
 * Make the environment block of ENVP: the entries sorted by name, in UTF-16,
 * each ending in a NUL and the block in a second one.
 */
static DWORD make_env(const char *const *envp, wchar_t **out)
{
    const char **vars;
    wchar_t **wide;
    wchar_t *block;
    size_t n;
    size_t i;
    size_t len;
    DWORD err = 0;

    *out = NULL;
    for (n = 0; envp[n] != NULL; n++)
        ;
    vars = calloc(n + 1, sizeof *vars);
    wide = calloc(n + 1, sizeof *wide);
    if (vars == NULL || wide == NULL) {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto out;
    }
    memcpy(vars, envp, n * sizeof *vars);
    if (etude_win_envsort(vars, n) != 0) {
        err = ERROR_INVALID_PARAMETER;
        goto out;
    }
    len = 1;
    for (i = 0; i < n; i++) {
        wide[i] = etude_win_wide(vars[i]);
        if (wide[i] == NULL) {
            err = GetLastError();
            goto out;
        }
        len += wcslen(wide[i]) + 1;
    }
    block = malloc((len + 1) * sizeof *block);
    if (block == NULL) {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto out;
    }
    len = 0;
    for (i = 0; i < n; i++) {
        memcpy(block + len, wide[i], (wcslen(wide[i]) + 1) * sizeof *block);
        len += wcslen(wide[i]) + 1;
    }
    block[len++] = L'\0';
    block[len] = L'\0';
    *out = block;
out:
    if (wide != NULL) {
        for (i = 0; i < n; i++)
            free(wide[i]);
    }
    free(wide);
    free(vars);
    return err;
}

/**
 * Make C's job: its processes die when its last handle closes, an unhandled
 * exception ends a process without the error report's dialog, and no process
 * may break away. The job's messages go to the supervisor's port.
 */
static DWORD make_job(struct etude_win_sup *s, struct etude_win_child *c)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp;

    c->job = CreateJobObjectW(NULL, NULL);
    if (c->job == NULL) {
        c->job = INVALID_HANDLE_VALUE;
        return GetLastError();
    }
    memset(&lim, 0, sizeof lim);
    lim.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(c->job, JobObjectExtendedLimitInformation, &lim, sizeof lim))
        return GetLastError();
    acp.CompletionKey = c;
    acp.CompletionPort = s->port;
    if (!SetInformationJobObject(c->job, JobObjectAssociateCompletionPortInformation, &acp,
                                 sizeof acp))
        return GetLastError();
    return 0;
}

/** Open standard input: PATH, the inherited handle for "-", or NUL. */
static DWORD open_stdin(const char *path, HANDLE *in)
{
    SECURITY_ATTRIBUTES sa;
    wchar_t *w = NULL;

    if (path != NULL && strcmp(path, "-") == 0) {
        if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_INPUT_HANDLE),
                             GetCurrentProcess(), in, 0, TRUE, DUPLICATE_SAME_ACCESS))
            return GetLastError();
        return 0;
    }
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (path != NULL) {
        w = etude_win_long_path(path);
        if (w == NULL)
            return GetLastError();
    }
    *in = CreateFileW(w != NULL ? w : L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (*in == INVALID_HANDLE_VALUE)
        return GetLastError();
    return 0;
}

/* The caller retains its pipe handle; the child inherits only our duplicate. */
static DWORD spawn_stdin(const struct etude_win_spawn *sp, HANDLE *in)
{
    if (sp->stdin_handle && sp->stdin_handle != INVALID_HANDLE_VALUE) {
        if (!DuplicateHandle(GetCurrentProcess(), sp->stdin_handle, GetCurrentProcess(), in, 0,
                             TRUE, DUPLICATE_SAME_ACCESS))
            return GetLastError();
        return 0;
    }
    return open_stdin(sp->stdin_path, in);
}

/** Free a child that never ran, with everything it holds. */
static void discard(struct etude_win_child *c)
{
    close_handle(&c->out.handle);
    close_handle(&c->err.handle);
    close_handle(&c->process);
    close_handle(&c->job);
    free(c);
}

/* What etude_win_start makes on the way to the child's process. */
struct launch {
    wchar_t *prog;
    wchar_t *line;
    wchar_t *cwd;
    wchar_t *env;
    HANDLE in; /* the child's ends: standard input, output and error */
    HANDLE outw;
    HANDLE errw;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs;
};

/** Free what L holds, closing the parent's copies of the child's ends. */
static void launch_free(struct launch *l)
{
    close_handle(&l->in);
    close_handle(&l->outw);
    close_handle(&l->errw);
    if (l->attrs != NULL) {
        DeleteProcThreadAttributeList(l->attrs);
        free(l->attrs);
    }
    free(l->prog);
    free(l->line);
    free(l->cwd);
    free(l->env);
}

/** Convert the strings of SP to the wide forms CreateProcessW takes. */
static DWORD convert(const struct etude_win_spawn *sp, struct launch *l)
{
    const char *const noargs[1] = {NULL};
    char *line;
    int err;

    err = etude_win_cmdline(sp->program, sp->argv != NULL ? sp->argv : noargs, &line);
    if (err != 0)
        return err == ENOMEM ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER;
    l->line = etude_win_wide(line);
    free(line);
    l->prog = etude_win_wide(sp->program);
    if (l->prog == NULL || l->line == NULL)
        return GetLastError();
    if (wcslen(l->line) >= ETUDE_WIN_CMDLINE_MAX)
        return ERROR_FILENAME_EXCED_RANGE;
    if (sp->cwd != NULL && (l->cwd = etude_win_wide(sp->cwd)) == NULL)
        return GetLastError();
    if (sp->envp != NULL)
        return make_env(sp->envp, &l->env);
    return 0;
}

/**
 * Create the child's process from L, suspended, with its three standard
 * handles the only ones it inherits.
 */
static DWORD create(struct launch *l, PROCESS_INFORMATION *pi)
{
    HANDLE inherit[3];
    STARTUPINFOEXW si;
    SIZE_T size = 0;

    inherit[0] = l->in;
    inherit[1] = l->outw;
    inherit[2] = l->errw;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    l->attrs = malloc(size);
    if (l->attrs == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (!InitializeProcThreadAttributeList(l->attrs, 1, 0, &size)) {
        free(l->attrs);
        l->attrs = NULL;
        return GetLastError();
    }
    if (!UpdateProcThreadAttribute(l->attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                   sizeof inherit, NULL, NULL))
        return GetLastError();
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = l->in;
    si.StartupInfo.hStdOutput = l->outw;
    si.StartupInfo.hStdError = l->errw;
    si.lpAttributeList = l->attrs;
    if (!CreateProcessW(l->prog, l->line, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        l->env, l->cwd, &si.StartupInfo, pi))
        return GetLastError();
    return 0;
}

/**
 * Put the suspended process of PI in C's job and name it. On failure the
 * process is ended and its handles closed.
 */
static DWORD enter_job(struct etude_win_child *c, PROCESS_INFORMATION *pi)
{
    FILETIME ft[4];
    DWORD err;

    c->process = pi->hProcess;
    if (!AssignProcessToJobObject(c->job, pi->hProcess)) {
        err = GetLastError();
        TerminateProcess(pi->hProcess, ETUDE_WIN_REFUSED_STATUS);
        CloseHandle(pi->hThread);
        return err;
    }
    c->ident.pid = pi->dwProcessId;
    if (GetProcessTimes(pi->hProcess, &ft[0], &ft[1], &ft[2], &ft[3]))
        c->ident.created = (uint64_t)ft[0].dwHighDateTime << 32 | ft[0].dwLowDateTime;
    return 0;
}

/**
 * Add C to S, have it registered, and let its thread THREAD run or end it;
 * then start reading its pipes.
 */
static void begin(struct etude_win_sup *s, struct etude_win_child *c,
                  const struct etude_win_spawn *sp, HANDLE thread)
{
    int refused;

    c->next = s->children;
    s->children = c;
    refused = sp->registered != NULL && sp->registered(sp->ctx, c, &c->ident) != 0;
    if (refused) {
        c->stopped = 1;
        TerminateJobObject(c->job, ETUDE_WIN_REFUSED_STATUS);
    } else {
        ResumeThread(thread);
    }
    CloseHandle(thread);
    if (CreateIoCompletionPort(c->out.handle, s->port, (ULONG_PTR)&c->out, 0) == NULL ||
        CreateIoCompletionPort(c->err.handle, s->port, (ULONG_PTR)&c->err, 0) == NULL) {
        c->out.error = c->err.error = GetLastError();
        c->out.eof = c->err.eof = 1;
        etude_win_stop(c);
        return;
    }
    read_next(&c->out);
    read_next(&c->err);
}

/* The children this process has started, whose count names their pipes. */
static LONG started;

DWORD etude_win_start(struct etude_win_sup *s, const struct etude_win_spawn *sp,
                      struct etude_win_child **out)
{
    struct etude_win_child *c;
    struct launch l;
    PROCESS_INFORMATION pi;
    DWORD err;

    *out = NULL;
    c = calloc(1, sizeof *c);
    if (c == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    memset(&l, 0, sizeof l);
    l.in = l.outw = l.errw = INVALID_HANDLE_VALUE;
    c->sup = s;
    c->job = c->process = c->out.handle = c->err.handle = INVALID_HANDLE_VALUE;
    c->output = sp->output;
    c->ctx = sp->ctx;
    s->serial = (unsigned long)InterlockedIncrement(&started);
    if ((err = convert(sp, &l)) != 0 || (err = make_job(s, c)) != 0 ||
        (err = make_pipe(s, c, ETUDE_WIN_STDOUT, &l.outw)) != 0 ||
        (err = make_pipe(s, c, ETUDE_WIN_STDERR, &l.errw)) != 0 ||
        (err = spawn_stdin(sp, &l.in)) != 0 || (err = create(&l, &pi)) != 0 ||
        (err = enter_job(c, &pi)) != 0) {
        launch_free(&l);
        discard(c);
        return err;
    }
    launch_free(&l);
    begin(s, c, sp, pi.hThread);
    *out = c;
    return 0;
}

/** Take the completed read of P, deliver its bytes and queue the next. */
static void pipe_done(struct etude_win_pipe *p)
{
    DWORD n = 0;
    DWORD err;

    p->pending = 0;
    if (!GetOverlappedResult(p->handle, &p->ov, &n, FALSE)) {
        err = GetLastError();
        if (err != ERROR_BROKEN_PIPE)
            p->error = err;
        p->eof = 1;
        return;
    }
    if (n > 0) {
        p->bytes += n;
        if (p->child->output != NULL)
            p->child->output(p->child->ctx, p->child, p->stream, p->buf, n);
    }
    read_next(p);
}

/**
 * Find the pipe whose completion key is KEY. A key that names no pipe of a
 * child is a job's message, which may outlive the child, so it is never read
 * through.
 */
static struct etude_win_pipe *find_pipe(struct etude_win_sup *s, ULONG_PTR key)
{
    struct etude_win_child *c;

    for (c = s->children; c != NULL; c = c->next) {
        if ((struct etude_win_pipe *)key == &c->out)
            return &c->out;
        if ((struct etude_win_pipe *)key == &c->err)
            return &c->err;
    }
    return NULL;
}

/**
 * Ask C's process and job for their state, and mark C ended when its first
 * process has exited, its job is empty and its pipes are at their end.
 *
 * @return 1 when C ended now, otherwise 0.
 */
static int refresh(struct etude_win_child *c)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION a;

    if (c->ended)
        return 0;
    if (!c->main_ended && WaitForSingleObject(c->process, 0) == WAIT_OBJECT_0) {
        GetExitCodeProcess(c->process, &c->status);
        c->main_ended = 1;
    }
    if (!c->main_ended || !c->out.eof || !c->err.eof)
        return 0;
    if (!QueryInformationJobObject(c->job, JobObjectBasicAccountingInformation, &a, sizeof a,
                                   NULL) ||
        a.ActiveProcesses != 0)
        return 0;
    c->ended = 1;
    return 1;
}

/** Refresh every child of S, and return how many ended. */
static int refresh_all(struct etude_win_sup *s)
{
    struct etude_win_child *c;
    int ended = 0;

    for (c = s->children; c != NULL; c = c->next)
        ended += refresh(c);
    return ended;
}

int etude_win_poll(struct etude_win_sup *s, DWORD timeout)
{
    OVERLAPPED_ENTRY e[POLL_BATCH];
    struct etude_win_pipe *p;
    ULONGLONG start = GetTickCount64();
    ULONGLONG waited;
    DWORD slice;
    ULONG n;
    ULONG i;
    int ended;

    for (;;) {
        ended = refresh_all(s);
        if (ended != 0)
            return ended;
        waited = GetTickCount64() - start;
        slice = POLL_SLICE;
        if (timeout != INFINITE && waited >= timeout)
            slice = 0;
        else if (timeout != INFINITE && timeout - waited < slice)
            slice = (DWORD)(timeout - waited);
        if (!GetQueuedCompletionStatusEx(s->port, e, POLL_BATCH, &n, slice, FALSE)) {
            if (GetLastError() != WAIT_TIMEOUT)
                return -1;
            if (slice == 0)
                return 0;
            continue;
        }
        for (i = 0; i < n; i++) {
            p = find_pipe(s, e[i].lpCompletionKey);
            if (p != NULL && e[i].lpOverlapped == &p->ov)
                pipe_done(p);
        }
        return refresh_all(s);
    }
}

DWORD etude_win_stop(struct etude_win_child *c)
{
    if (!c->ended)
        c->stopped = 1;
    if (!TerminateJobObject(c->job, ETUDE_WIN_STOP_STATUS))
        return GetLastError();
    return 0;
}

DWORD etude_win_usage(const struct etude_win_child *c, struct etude_win_usage *u)
{
    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION a;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;

    if (!QueryInformationJobObject(c->job, JobObjectBasicAndIoAccountingInformation, &a, sizeof a,
                                   NULL) ||
        !QueryInformationJobObject(c->job, JobObjectExtendedLimitInformation, &lim, sizeof lim,
                                   NULL))
        return GetLastError();
    u->user_us = (uint64_t)a.BasicInfo.TotalUserTime.QuadPart / 10u;
    u->kernel_us = (uint64_t)a.BasicInfo.TotalKernelTime.QuadPart / 10u;
    u->peak_job_commit = lim.PeakJobMemoryUsed;
    u->peak_process_commit = lim.PeakProcessMemoryUsed;
    u->read_bytes = a.IoInfo.ReadTransferCount;
    u->write_bytes = a.IoInfo.WriteTransferCount;
    u->processes = a.BasicInfo.TotalProcesses;
    u->active = a.BasicInfo.ActiveProcesses;
    return 0;
}

DWORD etude_win_pids(const struct etude_win_child *c, struct etude_win_ident *ids, size_t max,
                     size_t *n)
{
    JOBOBJECT_BASIC_PROCESS_ID_LIST *list;
    size_t size = sizeof *list + max * sizeof list->ProcessIdList[0];
    FILETIME ft[4];
    HANDLE h;
    BOOL in;
    DWORD err = 0;
    DWORD i;

    *n = 0;
    list = malloc(size);
    if (list == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (!QueryInformationJobObject(c->job, JobObjectBasicProcessIdList, list, (DWORD)size, NULL)) {
        err = GetLastError();
        free(list);
        return err;
    }
    if (list->NumberOfAssignedProcesses > list->NumberOfProcessIdsInList)
        err = ERROR_MORE_DATA;
    for (i = 0; i < list->NumberOfProcessIdsInList && *n < max; i++) {
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)list->ProcessIdList[i]);
        if (h == NULL)
            continue;
        if (IsProcessInJob(h, c->job, &in) && in &&
            GetProcessTimes(h, &ft[0], &ft[1], &ft[2], &ft[3])) {
            ids[*n].pid = (DWORD)list->ProcessIdList[i];
            ids[*n].created = (uint64_t)ft[0].dwHighDateTime << 32 | ft[0].dwLowDateTime;
            (*n)++;
        }
        CloseHandle(h);
    }
    free(list);
    return err;
}

DWORD etude_win_release(struct etude_win_child *c, DWORD timeout)
{
    struct etude_win_sup *s = c->sup;
    struct etude_win_child **pp;
    ULONGLONG start = GetTickCount64();
    ULONGLONG waited;

    if (!c->ended)
        etude_win_stop(c);
    while (!c->ended) {
        waited = GetTickCount64() - start;
        if (timeout != INFINITE && waited >= timeout)
            return ERROR_TIMEOUT;
        if (etude_win_poll(s, timeout == INFINITE ? INFINITE : (DWORD)(timeout - waited)) < 0)
            return GetLastError();
    }
    for (pp = &s->children; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    discard(c);
    return 0;
}

DWORD etude_win_sup_close(struct etude_win_sup *s, DWORD timeout)
{
    struct etude_win_child *c;
    struct etude_win_child *next;
    ULONGLONG start = GetTickCount64();
    ULONGLONG waited;
    DWORD left;

    for (c = s->children; c != NULL; c = c->next)
        etude_win_stop(c);
    for (c = s->children; c != NULL; c = next) {
        next = c->next;
        waited = GetTickCount64() - start;
        left = timeout == INFINITE ? INFINITE : waited >= timeout ? 0 : (DWORD)(timeout - waited);
        etude_win_release(c, left);
    }
    if (s->children != NULL)
        return ERROR_TIMEOUT;
    close_handle(&s->port);
    return 0;
}
