/*
 * nc.c - minimal TCP client (netcat-style) for Rach2.
 *
 * Usage: nc <host> <port>
 *
 * Connects to host:port over TCP, then relays a line of input to the socket
 * and prints the reply, using read()/write() on the socket descriptor so the
 * connection is just another file descriptor. Half-duplex (send a line, read a
 * reply), which suits line-oriented servers and the TCP echo server.
 *
 * The host argument may be an IPv4 address or a name resolved through /hosts and
 * DNS (see resolv.c).
 */

#include "netdb.h"

typedef unsigned int   size_t;
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_CLOSE      6
#define SYS_SOCKETCALL 102

#define SC_SOCKET   1
#define SC_CONNECT  3
#define AF_INET     2
#define SOCK_STREAM 1

static int sys_read(int fd, void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_READ), "b"(fd), "c"(buf), "d"(count) : "memory");
    return r;
}

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count) : "memory");
    return r;
}

static int sys_close(int fd)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_CLOSE), "b"(fd));
    return r;
}

static int socketcall(int call, uint32_t *args)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_SOCKETCALL), "b"(call), "c"(args) : "memory");
    return r;
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s) { sys_write(STDOUT_FILENO, s, strlen(s)); }

static int parse_port(const char *s)
{
    int p = 0;
    while (*s >= '0' && *s <= '9') { p = p * 10 + (*s - '0'); s++; }
    return p;
}

int main(int argc, char **argv)
{
    uint32_t ip, args[3];
    uint8_t  sa[16];
    char     buf[1024];
    int      fd, port, i, n, m;

    if (argc < 3) {
        print("usage: nc <host> <port>\n");
        return 1;
    }

    port = parse_port(argv[2]);
    if (port <= 0) {
        print("nc: invalid port\n");
        return 1;
    }
    if (resolve_host(argv[1], &ip) != 0) {
        print("nc: cannot resolve host\n");
        return 1;
    }

    args[0] = AF_INET; args[1] = SOCK_STREAM; args[2] = 0;
    fd = socketcall(SC_SOCKET, args);
    if (fd < 0) { print("nc: socket failed\n"); return 1; }

    for (i = 0; i < 16; i++) sa[i] = 0;
    sa[0] = AF_INET;
    sa[2] = (uint8_t)(port >> 8); sa[3] = (uint8_t)port;
    sa[4] = (uint8_t)(ip >> 24); sa[5] = (uint8_t)(ip >> 16);
    sa[6] = (uint8_t)(ip >> 8);  sa[7] = (uint8_t)ip;
    args[0] = (uint32_t)fd; args[1] = (uint32_t)sa; args[2] = 16;
    if (socketcall(SC_CONNECT, args) < 0) {
        print("nc: connect failed\n");
        sys_close(fd);
        return 1;
    }
    print("nc: connected\n");

    while ((n = sys_read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        if (sys_write(fd, buf, (size_t)n) < 0) break;
        m = sys_read(fd, buf, sizeof(buf));
        if (m <= 0) break;
        sys_write(STDOUT_FILENO, buf, (size_t)m);
    }

    sys_close(fd);
    return 0;
}
