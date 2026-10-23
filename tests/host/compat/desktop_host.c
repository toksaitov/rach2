/* Native transport and process operations for desktop controller fixtures. */
#ifdef _WIN32
#include "win/files.h"
#include <process.h>
#else
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "desktop_host.h"

host_socket host_qemu_connect(const char *endpoint)
{
#ifdef _WIN32
    char *end;
    unsigned long port;
    if (endpoint == NULL || etude_win_net_start() != 0)
        return HOST_SOCKET_INVALID;
    port = strtoul(endpoint, &end, 10);
    if (*endpoint == '\0' || *end != '\0' || port == 0 || port > 65535)
        return HOST_SOCKET_INVALID;
    return (host_socket)etude_win_connect((unsigned short)port, INFINITE);
#else
    struct sockaddr_un address;
    int fd;
    if (endpoint == NULL || strlen(endpoint) >= sizeof(address.sun_path))
        return HOST_SOCKET_INVALID;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, endpoint, strlen(endpoint) + 1);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
        return fd;
    if (fd >= 0)
        close(fd);
    return HOST_SOCKET_INVALID;
#endif
}

const char *host_qemu_endpoint(int argc, char **argv, const char *id)
{
    char prefix[128];
    int i;
#ifdef _WIN32
    snprintf(prefix, sizeof(prefix), "socket,id=%s,host=127.0.0.1,port=", id);
#else
    snprintf(prefix, sizeof(prefix), "socket,id=%s,path=", id);
#endif
    for (i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "-chardev") == 0 && strncmp(argv[i + 1], prefix, strlen(prefix)) == 0)
            return argv[i + 1] + strlen(prefix);
    return NULL;
}

int host_socket_read(host_socket socket, void *data, size_t size)
{
    int count;
    if (size > INT_MAX)
        size = INT_MAX;
#ifdef _WIN32
    count = recv((SOCKET)socket, data, (int)size, 0);
#else
    do {
        count = (int)read((int)socket, data, size);
    } while (count < 0 && errno == EINTR);
#endif
    return count;
}

int host_socket_write(host_socket socket, const void *data, size_t size)
{
    int count;
    if (size > INT_MAX)
        size = INT_MAX;
#ifdef _WIN32
    count = send((SOCKET)socket, data, (int)size, 0);
#else
    do {
        count = (int)write((int)socket, data, size);
    } while (count < 0 && errno == EINTR);
#endif
    return count;
}

void host_socket_close(host_socket socket)
{
#ifdef _WIN32
    closesocket((SOCKET)socket);
#else
    close((int)socket);
#endif
}

int host_process_cpu_us(uint64_t *used)
{
#ifdef _WIN32
    FILETIME created, ended, kernel, user;
    ULARGE_INTEGER k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
        return -1;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    *used = k.QuadPart / 10 + u.QuadPart / 10;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return -1;
    *used = (uint64_t)usage.ru_utime.tv_sec * 1000000 + usage.ru_utime.tv_usec +
            (uint64_t)usage.ru_stime.tv_sec * 1000000 + usage.ru_stime.tv_usec;
#endif
    return 0;
}

uint64_t host_clock_us(void)
{
#ifdef _WIN32
    return etude_win_mono_us();
#else
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
        abort();
    return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_nsec / 1000;
#endif
}

void host_sleep_ms(unsigned long milliseconds)
{
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec interval;
    interval.tv_sec = milliseconds / 1000;
    interval.tv_nsec = (long)(milliseconds % 1000) * 1000000;
    while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {
    }
#endif
}

void host_wait_forever(void)
{
    for (;;) {
#ifdef _WIN32
        Sleep(INFINITE);
#else
        pause();
#endif
    }
}

int host_quiet_child(const char *self, const char *mode)
{
#ifdef _WIN32
    wchar_t *program = etude_win_wide(self), *argument = etude_win_wide(mode);
    intptr_t status = -1;
    if (program != NULL && argument != NULL)
        status = _wspawnl(_P_WAIT, program, program, L"--quiet-child", argument, NULL);
    free(program);
    free(argument);
    return (int)status;
#else
    pid_t pid = fork(), waited;
    int status;
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execl(self, self, "--quiet-child", mode, (char *)NULL);
        _exit(121);
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}

int host_process_stopped(const struct etude_exec_result *result, int hard)
{
#ifdef _WIN32
    (void)hard;
    return result->exit_end == ETUDE_END_EXITED &&
           (unsigned long)(unsigned int)result->exit_value == ETUDE_WIN_STOP_STATUS;
#else
    return result->exit_end == ETUDE_END_SIGNALED &&
           result->exit_value == (hard ? SIGKILL : SIGTERM);
#endif
}

const char *host_program(const char *name, char *out, size_t size)
{
#ifdef _WIN32
    return etude_win_program(name, out, size) == 0 ? out : NULL;
#else
    const char *path = getenv("PATH"), *end;
    size_t length;
    while (path != NULL && *path) {
        end = strchr(path, ':');
        length = end != NULL ? (size_t)(end - path) : strlen(path);
        if (length && snprintf(out, size, "%.*s/%s", (int)length, path, name) < (int)size &&
            access(out, X_OK) == 0)
            return out;
        path = end != NULL ? end + 1 : NULL;
    }
    return NULL;
#endif
}
