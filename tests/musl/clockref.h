/** Host clock samples bracketed by guest clock reads, without serial timing. */
#ifndef TESTS_CLOCKREF_H
#define TESTS_CLOCKREF_H

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../clockref.h"

/** Read clock `id` in nanoseconds, or return -1 on an error. */
static long long clockref_ns(clockid_t id)
{
    struct timespec t;

    if (clock_gettime(id, &t) != 0)
        return -1;
    return (long long)t.tv_sec * CLOCKREF_NS + t.tv_nsec;
}

/**
 * Obtain a host sample from the reference at IPv4 address `host` and `port`.
 * Each exchange has a distinct sequence. Reads before send and after receive
 * enclose the host sample. A one-second receive timeout only bounds
 * completion. Up to 32 exchanges seek an uncertainty at most 100 ms; none
 * means an unavailable reference, never evidence that a clock is correct or
 * wrong.
 */
static int clockref_read_at(const char *host, int port, struct clockref_sample *s)
{
    static unsigned sequence;
    struct sockaddr_in address;
    int fd, attempt;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(host);
    address.sin_port = htons((unsigned short)port);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return 0;
    }
    for (attempt = 0; attempt < 32; attempt++) {
        char packet[160], tail;
        unsigned echoed;
        int length;
        fd_set ready;
        struct timeval timeout = {1, 0};

        sequence++;
        length = snprintf(packet, sizeof(packet), "CLOCKREF %u", sequence);
        s->mono_before = clockref_ns(CLOCK_MONOTONIC);
        s->real_before = clockref_ns(CLOCK_REALTIME);
        if (send(fd, packet, length, 0) != length)
            break;
        FD_ZERO(&ready);
        FD_SET(fd, &ready);
        if (select(fd + 1, &ready, NULL, NULL, &timeout) <= 0)
            continue;
        length = recv(fd, packet, sizeof(packet) - 1, 0);
        s->real_after = clockref_ns(CLOCK_REALTIME);
        s->mono_after = clockref_ns(CLOCK_MONOTONIC);
        if (length <= 0)
            continue;
        packet[length] = '\0';
        if (sscanf(packet, "CLOCKREF %u %lld %lld %c", &echoed, &s->host_mono, &s->host_real,
                   &tail) != 3 ||
            echoed != sequence)
            continue;
        if (s->mono_before < 0 || s->real_before < 0 || s->host_mono < 0 || s->host_real < 0)
            break;
        if (s->mono_after >= s->mono_before && s->real_after >= s->real_before &&
            s->mono_after - s->mono_before <= CLOCKREF_UNCERTAINTY &&
            s->real_after - s->real_before <= CLOCKREF_UNCERTAINTY) {
            close(fd);
            return 1;
        }
    }
    close(fd);
    return 0;
}

/** Obtain a host sample from QEMU's host, 10.0.2.2, at `port`. */
static int clockref_read(int port, struct clockref_sample *s)
{
    return clockref_read_at("10.0.2.2", port, s);
}

#endif
