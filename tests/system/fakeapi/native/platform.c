/* platform.c: the service owns sockets and joins every connection thread. */
#include "platform.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <tlhelp32.h>
#include <direct.h>
#include "../../../../tools/etude/win/winexec.h"
static HANDLE parent, stop_event;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <time.h>
static pid_t parent;
#endif

int service_init(void)
{
#ifdef _WIN32
    WSADATA data;
    PROCESSENTRY32 entry;
    HANDLE snapshot;
    if (WSAStartup(MAKEWORD(2, 2), &data))
        return -1;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (snapshot == INVALID_HANDLE_VALUE)
        return -1;
    if (Process32First(snapshot, &entry))
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                parent = OpenProcess(SYNCHRONIZE, FALSE, entry.th32ParentProcessID);
                break;
            }
        } while (Process32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return parent ? 0 : -1;
#else
    parent = getppid();
    return 0;
#endif
}
int service_parent_alive(void)
{
#ifdef _WIN32
    return WaitForSingleObject(parent, 0) == WAIT_TIMEOUT;
#else
    return getppid() == parent;
#endif
}
/* Only a service that polls this event advertises cooperative stopping. */
int service_stop_listen(void)
{
#ifdef _WIN32
    return stop_event ? 0 : etude_win_service_listen(&stop_event) ? -1 : 0;
#else
    return 0;
#endif
}
int service_stop_requested(void)
{
#ifdef _WIN32
    return stop_event && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
#else
    return 0;
#endif
}
uint64_t service_monotonic(void)
{
#ifdef _WIN32
    LARGE_INTEGER now, rate;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&rate);
    return (uint64_t)(now.QuadPart / rate.QuadPart) * UINT64_C(1000000000) +
           (uint64_t)(now.QuadPart % rate.QuadPart) * UINT64_C(1000000000) / rate.QuadPart;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
#endif
}
void service_pause(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000;
    while (nanosleep(&ts, &ts) && errno == EINTR) {
    }
#endif
}
struct start {
    void (*fn)(void *);
    void *arg;
};
#ifdef _WIN32
static DWORD WINAPI entry(void *arg)
#else
static void *entry(void *arg)
#endif
{
    struct start start = *(struct start *)arg;
    free(arg);
    start.fn(start.arg);
    return 0;
}
int service_thread_start(service_thread *thread, void (*fn)(void *), void *arg)
{
    struct start *start = malloc(sizeof(*start));
    if (!start)
        return -1;
    start->fn = fn;
    start->arg = arg;
#ifdef _WIN32
    *thread = CreateThread(NULL, 0, entry, start, 0, NULL);
    if (*thread)
        return 0;
#else
    if (!pthread_create(thread, NULL, entry, start))
        return 0;
#endif
    free(start);
    return -1;
}
void service_thread_join(service_thread thread)
{
#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
}
void service_mutex_init(service_mutex *lock)
{
#ifdef _WIN32
    InitializeCriticalSection(lock);
#else
    pthread_mutex_init(lock, NULL);
#endif
}
void service_lock(service_mutex *lock)
{
#ifdef _WIN32
    EnterCriticalSection(lock);
#else
    pthread_mutex_lock(lock);
#endif
}
void service_unlock(service_mutex *lock)
{
#ifdef _WIN32
    LeaveCriticalSection(lock);
#else
    pthread_mutex_unlock(lock);
#endif
}
void service_condition_init(service_condition *condition)
{
#ifdef _WIN32
    InitializeConditionVariable(condition);
#else
    pthread_cond_init(condition, NULL);
#endif
}
void service_wait(service_condition *condition, service_mutex *lock)
{
#ifdef _WIN32
    SleepConditionVariableCS(condition, lock, INFINITE);
#else
    pthread_cond_wait(condition, lock);
#endif
}
void service_wake(service_condition *condition)
{
#ifdef _WIN32
    WakeAllConditionVariable(condition);
#else
    pthread_cond_broadcast(condition);
#endif
}
int service_read(service_socket fd, void *data, size_t n)
{
    int got;
    do {
        got = recv(fd, data, (int)(n > INT_MAX ? INT_MAX : n), 0);
    } while (got < 0 && errno == EINTR);
    return got;
}
int service_write(service_socket fd, const void *data, size_t n)
{
    const char *p = data;
    while (n) {
        int sent = send(fd, p, (int)(n > INT_MAX ? INT_MAX : n), 0);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return -1;
        p += sent;
        n -= (size_t)sent;
    }
    return 0;
}
service_socket service_bind(unsigned port, int datagram, unsigned *actual)
{
    service_socket fd = socket(AF_INET, datagram ? SOCK_DGRAM : SOCK_STREAM, 0);
    struct sockaddr_in at;
    int yes = 1;
#ifdef _WIN32
    int size = sizeof(at);
#else
    socklen_t size = sizeof(at);
#endif
    if (fd == SERVICE_INVALID)
        return fd;
    if (!datagram)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    memset(&at, 0, sizeof(at));
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(UINT32_C(0x7f000001));
    at.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&at, sizeof(at)) || (!datagram && listen(fd, 32)) ||
        getsockname(fd, (struct sockaddr *)&at, &size)) {
        service_close(fd);
        return SERVICE_INVALID;
    }
    *actual = ntohs(at.sin_port);
    return fd;
}
int service_ready(service_socket fd, unsigned ms)
{
    struct timeval timeout;
    fd_set readset;
    int n;
    FD_ZERO(&readset);
    FD_SET(fd, &readset);
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (long)(ms % 1000) * 1000;
    n = select((int)fd + 1, &readset, NULL, NULL, &timeout);
    return n > 0;
}
unsigned service_port(const char *text)
{
    char *end;
    unsigned long n;
    errno = 0;
    n = strtoul(text, &end, 10);
    return errno || end == text || *end || n > 65535 ? 0 : (unsigned)n;
}
