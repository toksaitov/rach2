/* platform.h: sockets, threads and parent lifetime on the three hosts. */
#ifndef FAKEAPI_PLATFORM_H
#define FAKEAPI_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include "../../../../tools/etude/win/files.h"
typedef SOCKET service_socket;
typedef HANDLE service_thread;
typedef CRITICAL_SECTION service_mutex;
typedef CONDITION_VARIABLE service_condition;
#define SERVICE_INVALID     INVALID_SOCKET
#define service_close       closesocket
#define service_shutdown(s) shutdown(s, SD_BOTH)
#define service_mkdir(p)    etude_win_mkdir(p, 0700)
#define service_mode(p)     ((void)(p))
#else
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int service_socket;
typedef pthread_t service_thread;
typedef pthread_mutex_t service_mutex;
typedef pthread_cond_t service_condition;
#define SERVICE_INVALID     (-1)
#define service_close       close
#define service_shutdown(s) shutdown(s, SHUT_RDWR)
#define service_mkdir(p)    mkdir(p, 0700)
#define service_mode(p)     chmod(p, 0600)
#endif
int service_init(void);
int service_parent_alive(void);
int service_stop_listen(void);
int service_stop_requested(void);
uint64_t service_monotonic(void);
void service_pause(unsigned ms);
int service_thread_start(service_thread *thread, void (*fn)(void *), void *arg);
void service_thread_join(service_thread thread);
void service_mutex_init(service_mutex *lock);
void service_lock(service_mutex *lock);
void service_unlock(service_mutex *lock);
void service_condition_init(service_condition *condition);
void service_wait(service_condition *condition, service_mutex *lock);
void service_wake(service_condition *condition);
int service_read(service_socket fd, void *data, size_t n);
int service_write(service_socket fd, const void *data, size_t n);
service_socket service_bind(unsigned port, int datagram, unsigned *actual);
int service_ready(service_socket fd, unsigned ms);
unsigned service_port(const char *text);
#endif
