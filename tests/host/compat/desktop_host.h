/* Native transport and process operations for desktop controller fixtures. */
#ifndef RACH2_DESKTOP_HOST_H
#define RACH2_DESKTOP_HOST_H
#include <stddef.h>
#include <stdint.h>
#include "etude/exec.h"

/* A socket is not a CRT file descriptor on Windows. */
typedef intptr_t host_socket;
#define HOST_SOCKET_INVALID ((host_socket) - 1)

/** Connect to a QEMU chardev endpoint: a Unix path or a loopback TCP port. */
host_socket host_qemu_connect(const char *endpoint);
/** Read a chardev endpoint from QEMU's native command line, or return NULL. */
const char *host_qemu_endpoint(int argc, char **argv, const char *id);
/** Read or write socket bytes; return the byte count, 0 at EOF, or -1. */
int host_socket_read(host_socket socket, void *data, size_t size);
int host_socket_write(host_socket socket, const void *data, size_t size);
/** Close a socket; file descriptors remain the caller's responsibility. */
void host_socket_close(host_socket socket);
/** Read this process's user and kernel CPU time in microseconds. */
int host_process_cpu_us(uint64_t *used);
/** Return the monotonic clock in microseconds. */
uint64_t host_clock_us(void);
/** Sleep for an interval, or wait indefinitely for process termination. */
void host_sleep_ms(unsigned long milliseconds);
void host_wait_forever(void);
/** Execute this fixture's quiet-child role and wait for its real exit code. */
int host_quiet_child(const char *self, const char *mode);
/** Check the platform's actual stopped status; HARD selects SIGKILL on POSIX. */
int host_process_stopped(const struct etude_exec_result *result, int hard);
/** Locate a native program on PATH, including Windows' executable suffix. */
const char *host_program(const char *name, char *out, size_t size);
#endif
