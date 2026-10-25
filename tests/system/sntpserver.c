/* sntpserver.c: SNTP and CLOCKREF on one portable UDP socket.
 * EPOCH is the Unix date when the helper starts. Both replies advance from
 * it by the monotonic clock, so NTP and the independent probe agree. */
#include "fakeapi/native/platform.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
static volatile sig_atomic_t stopped;
static void stop(int sig)
{
    (void)sig;
    stopped = 1;
}
static void stamp(unsigned char *out, uint64_t ns)
{
    uint32_t whole = (uint32_t)(ns / UINT64_C(1000000000) + UINT64_C(2208988800));
    uint32_t part =
        (uint32_t)((ns % UINT64_C(1000000000)) * UINT64_C(4294967296) / UINT64_C(1000000000));
    unsigned i;
    for (i = 0; i < 4; i++) {
        out[i] = (unsigned char)(whole >> (24 - i * 8));
        out[4 + i] = (unsigned char)(part >> (24 - i * 8));
    }
}
int main(int argc, char **argv)
{
    char *end, packet[513];
    unsigned port;
    unsigned long long epoch;
    uint64_t start, now, real;
    service_socket fd;
    struct sockaddr_in peer;
    if (argc != 3)
        goto usage;
    epoch = strtoull(argv[1], &end, 10);
    if (*end || end == argv[1] || epoch > UINT64_MAX / UINT64_C(1000000000))
        goto usage;
    port = service_port(argv[2]);
    if (!port)
        goto usage;
    if (service_init())
        return 1;
    start = service_monotonic();
    fd = service_bind(port, 1, &port);
    if (fd == SERVICE_INVALID)
        return 1;
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("sntpserver: port %u\n", port);
    while (!stopped && service_parent_alive()) {
#ifdef _WIN32
        int size = sizeof(peer);
#else
        socklen_t size = sizeof(peer);
#endif
        int n;
        unsigned char reply[48];
        if (!service_ready(fd, 200))
            continue;
        n = recvfrom(fd, packet, 512, 0, (struct sockaddr *)&peer, &size);
        if (n < 0)
            continue;
        packet[n] = 0;
        now = service_monotonic();
        real = epoch * UINT64_C(1000000000) + now - start;
        if (!strncmp(packet, "CLOCKREF ", 9)) {
            char *token = packet + 9, *p, *tail;
            char answer[1024];
            while (*token && strchr(" \t\r\n\v\f", *token))
                token++;
            p = token;
            while (*p >= '0' && *p <= '9')
                p++;
            if (p == token)
                continue;
            tail = p;
            while (*tail && strchr(" \t\r\n\v\f", *tail))
                tail++;
            if (*tail)
                continue;
            *p = 0;
            n = snprintf(answer, sizeof(answer), "CLOCKREF %s %llu %llu", token,
                         (unsigned long long)now, (unsigned long long)real);
            sendto(fd, answer, n, 0, (struct sockaddr *)&peer, size);
            continue;
        }
        if (n < 48 || ((unsigned char)packet[0] & 7) != 3)
            continue;
        memset(reply, 0, sizeof(reply));
        reply[0] = ((unsigned char)packet[0] & 0x38) | 4;
        reply[1] = 1;
        reply[2] = (unsigned char)packet[2];
        reply[3] = (unsigned char)-20;
        memcpy(reply + 12, "RCH2", 4);
        stamp(reply + 16, real);
        memcpy(reply + 24, packet + 40, 8);
        stamp(reply + 32, real);
        stamp(reply + 40, epoch * UINT64_C(1000000000) + service_monotonic() - start);
        if (sendto(fd, (const char *)reply, 48, 0, (struct sockaddr *)&peer, size) == 48)
            printf("sntpserver: answered %s:%u\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
    }
    service_close(fd);
    return 0;
usage:
    fputs("usage: sntpserver EPOCH PORT\n", stderr);
    return 2;
}
