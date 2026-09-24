/**
 * Check the address the `httpd` status page gives for the server, and the
 * port operand it takes.
 *
 * The server's own `main` runs over modeled calls, and no socket is opened on
 * the host. `accept` hands it one connection and ends the run at its next
 * call, `recv` supplies one request, `send` keeps the page, `getsockname`
 * answers with the address and port a row chooses, and `exit` ends the run
 * with its status.
 */
#include <netinet/in.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static jmp_buf run_end;
static int failures, exit_status, accepts, name_fails;
static char page[8192], logged[1024];
static size_t page_len, logged_len;
static uint32_t local_ip;
static uint16_t local_port;
static const char request[] = "GET / HTTP/1.0\r\nHost: test\r\nUser-Agent: test\r\n\r\n";

static int model_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    return 3;
}

static int model_bind(int fd, const struct sockaddr *address, socklen_t size)
{
    (void)fd;
    (void)address;
    (void)size;
    return 0;
}

static int model_listen(int fd, int backlog)
{
    (void)fd;
    (void)backlog;
    return 0;
}

/** Hand over one connection from 10.0.2.2:5555, and end the run after it. */
static int model_accept(int fd, struct sockaddr *address, socklen_t *size)
{
    struct sockaddr_in *peer = (struct sockaddr_in *)address;

    (void)fd;
    if (accepts++ > 0) {
        longjmp(run_end, 1);
    }
    memset(peer, 0, sizeof(*peer));
    peer->sin_family = AF_INET;
    peer->sin_addr.s_addr = htonl(0x0a000202UL);
    peer->sin_port = htons(5555);
    *size = sizeof(*peer);
    return 4;
}

static ssize_t model_recv(int fd, void *buffer, size_t size, int flags)
{
    (void)fd;
    (void)flags;
    if (size < sizeof(request) - 1) {
        exit(2);
    }
    memcpy(buffer, request, sizeof(request) - 1);
    return (ssize_t)(sizeof(request) - 1);
}

/** Keep what the server sends, which is the response and its page. */
static ssize_t model_send(int fd, const void *buffer, size_t size, int flags)
{
    (void)fd;
    (void)flags;
    if (size > sizeof(page) - 1 - page_len) {
        exit(2);
    }
    memcpy(page + page_len, buffer, size);
    page_len += size;
    page[page_len] = '\0';
    return (ssize_t)size;
}

/** Answer with the address and port the row chose, or fail when it asks. */
static int model_getsockname(int fd, struct sockaddr *address, socklen_t *size)
{
    struct sockaddr_in *local = (struct sockaddr_in *)address;

    (void)fd;
    if (name_fails) {
        return -1;
    }
    memset(local, 0, sizeof(*local));
    local->sin_family = AF_INET;
    local->sin_addr.s_addr = htonl(local_ip);
    local->sin_port = htons(local_port);
    *size = sizeof(*local);
    return 0;
}

static int model_close(int fd)
{
    (void)fd;
    return 0;
}

/** Keep what the server logs to standard output. */
static ssize_t model_write(int fd, const void *buffer, size_t size)
{
    (void)fd;
    if (size > sizeof(logged) - 1 - logged_len) {
        size = sizeof(logged) - 1 - logged_len;
    }
    memcpy(logged + logged_len, buffer, size);
    logged_len += size;
    logged[logged_len] = '\0';
    return (ssize_t)size;
}

static void model_exit(int status)
{
    exit_status = status;
    longjmp(run_end, 1);
}

static int model_gettimeofday(struct timeval *tv, void *zone)
{
    (void)zone;
    tv->tv_sec = 1000;
    tv->tv_usec = 0;
    return 0;
}

#define socket       model_socket
#define bind         model_bind
#define listen       model_listen
#define accept       model_accept
#define recv         model_recv
#define send         model_send
#define getsockname  model_getsockname
#define close        model_close
#define write        model_write
#define exit         model_exit
#define gettimeofday model_gettimeofday
#define main         httpd_main
#include "httpd.c"
#undef main
#undef socket
#undef bind
#undef listen
#undef accept
#undef recv
#undef send
#undef getsockname
#undef close
#undef write
#undef exit
#undef gettimeofday

#define CHECK(condition, detail)                                   \
    do {                                                           \
        if (!(condition)) {                                        \
            fprintf(stderr, "httpd-hosttest: fail: %s\n", detail); \
            failures++;                                            \
        }                                                          \
    } while (0)

/** Run the server with one operand until it serves a page or exits. */
static void run(const char *operand)
{
    char *argv[3];

    argv[0] = (char *)"httpd";
    argv[1] = (char *)operand;
    argv[2] = NULL;
    page_len = 0;
    page[0] = '\0';
    logged_len = 0;
    logged[0] = '\0';
    accepts = 0;
    exit_status = -1;
    if (setjmp(run_end) == 0) {
        httpd_main(2, argv);
    }
}

/** Require the operand to be refused with the usage line and status 2. */
static void check_refused(const char *operand, const char *detail)
{
    run(operand);
    CHECK(exit_status == 2 && strstr(logged, "httpd: usage: httpd [port]") != NULL && page_len == 0,
          detail);
}

/**
 * Run the rows, the twenty-digit operand last.
 *
 * That row runs only once the others have passed. A port read with no bound
 * overflows on it, and the sanitizer reports an overflow by ending the run,
 * so the rows above are left to name a fault they can see first.
 */
int main(void)
{
    local_ip = 0x0a090807UL;
    local_port = 8080;
    run("8080");
    CHECK(strstr(logged, "httpd: listening on 8080\n") != NULL,
          "the server listens on the port given");
    CHECK(strstr(page, "<div class=stat>10.9.8.7:8080<br>") != NULL,
          "the page names the address and port the connection reached");
    CHECK(strstr(page, "10.0.2.2:5555") != NULL, "the page names the client");
    name_fails = 1;
    run("8080");
    name_fails = 0;
    CHECK(strstr(page, "<div class=stat>0.0.0.0:8080<br>") != NULL,
          "without getsockname the page names no address and the bound port");
    check_refused("0", "port 0 is refused");
    check_refused("65536", "a port past 65535 is refused");
    check_refused("80x", "a trailing character is refused");
    check_refused("", "an empty port is refused");
    if (failures == 0) {
        check_refused("99999999999999999999", "a port of twenty digits is refused");
    }
    printf("httpd-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
