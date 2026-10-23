/* Native process and socket operations for the scenario fixture. */
#ifndef RACH2_SCENARIO_HOST_H
#define RACH2_SCENARIO_HOST_H
#include "desktop_host.h"
#ifdef _WIN32
#include "win/files.h"
#ifndef SIGKILL
#define SIGKILL 9
#endif
#define SH_POLLIN POLLRDNORM
typedef int sh_socklen;
#else
#include <poll.h>
#include <sys/socket.h>
#define SH_POLLIN POLLIN
typedef socklen_t sh_socklen;
#endif
struct sh_poll {
    host_socket fd;
    short events, revents;
};
int sh_poll(struct sh_poll *items, size_t count, int milliseconds);
host_socket sh_socket(int type);
int sh_bound_error(void);
int sh_signal(long pid, int signal);
int sh_arm_stop(int cooperative);
int sh_dead_status(void);
#ifdef _WIN32
struct sh_child {
    HANDLE process, job, output;
    long pid;
};
int sh_start(struct sh_child *child, const char *self, char *const *argv, char **env,
             const char *error, int owned);
int sh_read(struct sh_child *child, void *data, size_t size);
int sh_wait(struct sh_child *child);
int sh_send_socket(host_socket connection, host_socket socket);
host_socket sh_receive_socket(host_socket connection);
#else
#define sh_dead_status() SIGKILL
#endif
#endif
