/* A service writes its own verdict only after its real stop request. */
#ifdef _WIN32
#include "win/winexec.h"
#endif
static volatile sig_atomic_t service_requested;
static void service_note(int sig)
{
    (void)sig;
    service_requested = 1;
}

static int service_status_path(char path[PATH_MAX])
{
    const char *mark = getenv("ETUDE_TEST_MARK");
    int n = mark ? snprintf(path, PATH_MAX, "%s.service-status", mark) : -1;
    return n < 0 || n >= PATH_MAX ? -1 : 0;
}

static int service_stop_tool(const char *mode)
{
    int ignore = !strcmp(mode, "ignore"), result = !strcmp(mode, "fail");
    char path[PATH_MAX];
    FILE *status;
#ifdef _WIN32
    HANDLE event = NULL;
    if (etude_win_service_listen(&event))
        return 121;
    FILETIME birth, ended, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &birth, &ended, &kernel, &user))
        return CloseHandle(event), 121;
    uint64_t created = (uint64_t)birth.dwHighDateTime << 32 | birth.dwLowDateTime;
    if (!etude_win_service_stop(GetCurrentProcessId(), created + 1))
        return CloseHandle(event), 121;
#endif
    if (service_status_path(path) || signal(SIGTERM, ignore ? SIG_IGN : service_note) == SIG_ERR)
        return 121;
#ifndef _WIN32
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous))
        return 121;
#endif
    puts("peer: listening");
    fflush(stdout);
#ifdef _WIN32
    if (WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0 && !ignore)
        service_note(SIGTERM);
    CloseHandle(event);
#endif
    if (ignore)
        host_wait_forever();
#ifndef _WIN32
    while (!service_requested)
        sigsuspend(&previous);
    sigprocmask(SIG_SETMASK, &previous, NULL);
#else
    if (!service_requested)
        return 121;
#endif
    printf("service: verdict %s\n", result ? "fail" : "pass");
    fflush(stdout);
    status = fopen(path, "wb");
    if (!status)
        return 121;
    int bad = fprintf(status, "%d\n", result) < 0;
    bad |= fclose(status) != 0;
    return bad ? 121 : result;
}

static int service_status_is(const char *expected)
{
    char path[PATH_MAX], text[8];
    if (service_status_path(path))
        return 0;
    FILE *file = fopen(path, "rb");
    if (!file)
        return expected == NULL;
    int ok = expected && fgets(text, sizeof text, file) && !strcmp(text, expected) &&
             fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return ok;
}

static int scenario_service_stop(struct etude_case *c, const char *row)
{
    const char *mode = !strcmp(row, "service-stop-ignore") ? "ignore"
                       : !strcmp(row, "service-stop-fail") ? "fail"
                                                           : "pass";
    struct etude_case_service *service = NULL;
    struct etude_termination end;
    char *args[] = {"tool", "service-stop", (char *)mode, NULL};
    if (etude_step(c, "serve") != ETUDE_OK ||
        etude_service(c, "peer", args, &service, &end) != ETUDE_OK)
        return 1;
    etude_assert(c, "ready", service != NULL, "the declared service did not listen");
    if (etude_step(c, "finish") != ETUDE_OK || etude_service_stop(service, &end) != ETUDE_OK)
        return 1;
    int ignore = !strcmp(mode, "ignore");
    const char *expected = ignore ? NULL : !strcmp(mode, "fail") ? "1\n" : "0\n";
    const char *verdict = !strcmp(mode, "fail") ? "service: verdict fail" : "service: verdict pass";
    const char *output = etude_evidence(c, "peer.1.out");
    etude_assert(c, "verdict",
                 ignore ? !file_has(output, "service: verdict") : file_has(output, verdict),
                 "forced termination is not a service verdict");
    etude_assert(c, "status", service_status_is(expected), "the exact service status is missing");
    int cleaned = end.end == ETUDE_END_STOPPED;
#ifdef _WIN32
    if (!ignore)
        cleaned = end.end == ETUDE_END_EXITED && end.value == (!strcmp(mode, "fail") ? 1 : 0);
#endif
    etude_assert(c, "cleanup", cleaned, "the service did not have its observed expected end");
    return 0;
}
