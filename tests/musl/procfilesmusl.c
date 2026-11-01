/*
 * procfilesmusl.c: the installed programs that read the machine's /proc
 * files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "../probe_report.h"

extern char **environ;
static int failures;

/** Report one row and remember whether it failed. */
static void check(const char *id, int held, const char *why)
{
    if (held) {
        report_pass(id);
    } else {
        report_fail(id, why);
        failures++;
    }
}

/** Run command, collect its output and report its exit status independently. */
static char *command(const char *id, const char *command)
{
    static char text[32768];
    size_t n = 0, got;
    char run[512];
    FILE *f;
    int status = -1, clean = 1;
    char *p;

    snprintf(run, sizeof(run), "%s 2>&1", command);
    f = popen(run, "r");
    text[0] = '\0';
    if (f != NULL) {
        while (n < sizeof(text) - 1 && (got = fread(text + n, 1, sizeof(text) - 1 - n, f)) > 0) {
            n += got;
        }
        text[n] = '\0';
        status = pclose(f);
    }
    for (p = text; p != NULL && *p != '\0'; p = strchr(p, '\n'), p = p == NULL ? NULL : p + 1) {
        char *end = strchr(p, '\n');
        char saved = end == NULL ? '\0' : *end;

        if (end != NULL) {
            *end = '\0';
        }
        if (strstr(p, "can't open") != NULL || strstr(p, "No such file") != NULL ||
            strstr(p, "not found") != NULL) {
            clean = 0;
        }
        if (end != NULL) {
            *end = saved;
        }
    }
    check(id,
          status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && clean &&
              n < sizeof(text) - 1,
          "the command failed, lacked a file or wrote too much");
    printf("procfiles: %s:\n%s\n", id, text);
    return text;
}

/** Find the first line beginning with prefix. */
static char *line(char *text, const char *prefix)
{
    char *p = text;
    size_t n = strlen(prefix);

    while (*p != '\0') {
        if (strncmp(p, prefix, n) == 0) {
            return p + n;
        }
        p = strchr(p, '\n');
        if (p == NULL) {
            break;
        }
        p++;
    }
    return NULL;
}

/** Check the process list against this program and its parent shell. */
static void ps_rows(void)
{
    char *text, *p;
    int found, parent, pid;

    text = command("ps-status", "ps");
    found = parent = 0;
    for (p = text; p != NULL && *p != '\0'; p = strchr(p, '\n'), p = p == NULL ? NULL : p + 1) {
        if (sscanf(p, "%d", &pid) == 1) {
            if (pid == (int)getpid()) {
                found = 1;
            }
            if (pid == (int)getppid()) {
                parent = 1;
            }
        }
    }
    check("ps-self", found && parent, "ps did not list this program and its parent shell");
}

/** Check the root size and the memory total. */
static void space_rows(unsigned long image_kib)
{
    char *text, *p;
    unsigned long a, b, d;
    int found;
    char dev[32], root[32], path[32];

    text = command("df-status", "df /");
    found = 0;
    for (p = text; p != NULL && *p != '\0'; p = strchr(p, '\n'), p = p == NULL ? NULL : p + 1) {
        if (sscanf(p, "%31s %lu %lu %lu %31s %31s", dev, &a, &b, &d, path, root) == 6 &&
            strcmp(root, "/") == 0 && a == image_kib && image_kib != 0) {
            found = 1;
        }
    }
    check("df-size", found, "df did not give the image's independently known size");
    text = command("free-status", "free");
    p = line(text, "Mem:");
    check("free-memory", p != NULL && sscanf(p, "%lu %lu", &a, &b) == 2 && a > 0 && a >= b,
          "free gave no nonzero consistent memory total");
}

/** Check uptime and each form of the kernel log. */
static void log_rows(void)
{
    char *text, *p;
    unsigned long a;
    int found;

    text = command("uptime-status", "uptime");
    p = strstr(text, "up ");
    check("uptime", p != NULL && sscanf(p + 3, "%lu", &a) == 1, "uptime gave no time");
    text = command("dmesg-status", "dmesg");
    p = strstr(text, "Rach2 Booting");
    check("dmesg-banner", p != NULL && (strchr(text, '\n') == NULL || p < strchr(text, '\n')),
          "the first boot record has no kernel banner");
    check("dmesg-console", strstr(text, "Type 'help'") == NULL,
          "dmesg included the debug shell's own output");
    text = command("raw-status", "dmesg -r");
    found = 0;
    for (p = text; *p != '\0'; p++) {
        if ((p == text || p[-1] == '\n') && p[0] == '<' && p[1] >= '0' && p[1] <= '9' &&
            p[2] == '>' && p[3] == '[') {
            found++;
        }
    }
    check("dmesg-records", found >= 20, "dmesg had fewer than twenty whole records");
    text = command("info-status", "dmesg -t -l info");
    check("dmesg-info", strncmp(text, "boot: Rach2 Booting", 19) == 0,
          "the info records retained a timestamp or lost the banner");
}

/** Check the route table against the actual gateway and subnet. */
static void route_rows(void)
{
    char *text, *p;

    text = command("route-status", "route -n");
    {
        int gateway = 0, subnet = 0;

        for (p = text; p != NULL && *p != '\0'; p = strchr(p, '\n'), p = p == NULL ? NULL : p + 1) {
            char destination[32], next[32], mask[32];

            if (sscanf(p, "%31s %31s %31s", destination, next, mask) == 3) {
                if (strcmp(destination, "0.0.0.0") == 0 && strcmp(next, "10.0.2.2") == 0) {
                    gateway = 1;
                }
                if (strcmp(destination, "10.0.2.0") == 0 && strcmp(mask, "255.255.255.0") == 0) {
                    subnet = 1;
                }
            }
        }
        check("route-default", gateway, "route lost the actual default gateway");
        check("route-subnet", subnet, "route lost the on-link subnet");
    }
}

/** Check the held connection, its neighbour and the interface state. */
static void socket_rows(void)
{
    char *text, *p;
    unsigned long a, b;
    int found;

    text = command("netstat-status", "netstat -tn 2>&1");
    found = 0;
    for (p = text; p != NULL && *p != '\0'; p = strchr(p, '\n'), p = p == NULL ? NULL : p + 1) {
        char local[64], remote[64], state[32];

        if (sscanf(p, "tcp %lu %lu %63s %63s %31s", &a, &b, local, remote, state) == 5 &&
            strncmp(remote, "10.0.2.2:", 9) == 0 && strcmp(state, "ESTABLISHED") == 0) {
            found = 1;
        }
    }
    check("netstat-peer", found, "netstat did not show the held host connection as established");
    text = command("arp-status", "arp -n");
    p = strstr(text, "(10.0.2.");
    check("arp-neighbour", p != NULL && strstr(p, " at ") != NULL && strstr(p, ":") != NULL,
          "arp had no resolved neighbour on the actual subnet");
    text = command("ifconfig-status", "ifconfig eth0");
    check("link-running", strstr(text, "RUNNING") != NULL, "the working interface is not RUNNING");
}

/** Bind `fd` to ::1 and return the port it has, or -1. */
static int bind_loopback6(int fd)
{
    struct sockaddr_in6 a;
    socklen_t n = sizeof(a);

    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &n) != 0) {
        return -1;
    }
    return ntohs(a.sin6_port);
}

/** Connect `fd` to ::1 at `port`: 0, or -1. */
static int connect_loopback6(int fd, int port)
{
    struct sockaddr_in6 a;

    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    a.sin6_port = htons((unsigned short)port);
    return fd < 0 ? -1 : connect(fd, (struct sockaddr *)&a, sizeof(a));
}

/**
 * Open on ::1 a TCP listener and a connection to it, a bound UDP socket and
 * one connected to it, and a raw ICMPv6 socket, and check that netstat
 * lists each from /proc/net/tcp6, udp6 and raw6. A raw socket here cannot
 * connect.
 */
static void ipv6_rows(void)
{
    int fd[5], tcp, udp, i;
    char *text, *p;
    char tcp_at[32], udp_at[32];
    int listening = 0, connected = 0, bound = 0, peered = 0, raw = 0;

    fd[0] = socket(AF_INET6, SOCK_STREAM, 0);
    fd[1] = socket(AF_INET6, SOCK_STREAM, 0);
    fd[2] = socket(AF_INET6, SOCK_DGRAM, 0);
    fd[3] = socket(AF_INET6, SOCK_DGRAM, 0);
    fd[4] = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    tcp = bind_loopback6(fd[0]);
    udp = bind_loopback6(fd[2]);
    check("ipv6-sockets",
          tcp > 0 && udp > 0 && listen(fd[0], 1) == 0 && connect_loopback6(fd[1], tcp) == 0 &&
              connect_loopback6(fd[3], udp) == 0 && bind_loopback6(fd[4]) >= 0,
          "the IPv6 sockets on ::1 could not be opened, bound or connected");
    snprintf(tcp_at, sizeof(tcp_at), "::1:%d", tcp);
    snprintf(udp_at, sizeof(udp_at), "::1:%d", udp);
    text = command("netstat6-status", "netstat -tuwan");
    for (p = text; p != NULL && *p != '\0'; p = strchr(p, '\n'), p = p == NULL ? NULL : p + 1) {
        char proto[16], local[64], remote[64], state[32];
        unsigned long a, b;
        int n = sscanf(p, "%15s %lu %lu %63s %63s %31s", proto, &a, &b, local, remote, state);

        if (n < 5) {
            continue;
        }
        if (n == 5) {
            state[0] = '\0';
        }
        if (strcmp(proto, "tcp") == 0) {
            listening |= strcmp(local, tcp_at) == 0 && strcmp(state, "LISTEN") == 0;
            connected |= strcmp(remote, tcp_at) == 0 && strcmp(state, "ESTABLISHED") == 0;
        } else if (strcmp(proto, "udp") == 0) {
            bound |= strcmp(local, udp_at) == 0 && strcmp(remote, ":::*") == 0;
            peered |= strcmp(remote, udp_at) == 0 && strcmp(state, "ESTABLISHED") == 0;
        } else if (strcmp(proto, "raw") == 0) {
            raw |= strcmp(local, "::1:58") == 0;
        }
    }
    check("netstat-tcp6", listening && connected,
          "netstat did not list the IPv6 listener and its connection");
    check("netstat-udp6", bound && peered,
          "netstat did not list the bound and the connected IPv6 UDP socket");
    check("netstat-raw6", raw, "netstat did not list the raw ICMPv6 socket");
    for (i = 0; i < 5; i++) {
        if (fd[i] >= 0) {
            close(fd[i]);
        }
    }
}

/** Check the network table field count and traffic counters. */
static void netdev_rows(void)
{
    char *text, *p;
    unsigned long long fields[17];
    unsigned i;
    int found;

    text = command("netdev-status", "cat /proc/net/dev");
    p = strstr(text, "eth0:");
    found = 0;
    if (p != NULL) {
        p += 5;
        for (i = 0; i < 17; i++) {
            char *end;

            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p < '0' || *p > '9') {
                break;
            }
            fields[i] = strtoull(p, &end, 10);
            p = end;
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        found = i == 16 && *p == '\n' && fields[0] > 0 && fields[8] > 0;
    }
    check("netdev-counters", found,
          "eth0 lacks sixteen numeric fields or traffic in both directions");
}

/** Check the timer vector and the sum of the vector counts. */
static void interrupt_rows(void)
{
    char *text, *p;
    unsigned long a, b, total = 0, timer = 0, intr = 0;
    int found;
    char path[32], name[32];

    text = command("interrupts-status", "cat /proc/interrupts");
    found = 0;
    for (p = text; p != NULL && *p != '\0'; p = strchr(p, '\n'), p = p == NULL ? NULL : p + 1) {
        if (sscanf(p, "%lu: %lu", &a, &b) == 2) {
            total += b;
        }
        if (sscanf(p, "%lu: %lu %31s %31s", &a, &b, path, name) == 4) {
            if (a == 32 && strcmp(path, "LAPIC") == 0 && strcmp(name, "timer") == 0) {
                timer = b;
                found = 1;
            }
        }
    }
    check("timer", found && timer > 0, "vector 32 has no counted LAPIC timer");
    text = command("stat-status", "cat /proc/stat");
    p = line(text, "intr ");
    check("interrupt-total",
          p != NULL && sscanf(p, "%lu", &intr) == 1 && total >= timer && intr >= total,
          "the interrupt total is below the vector sum");
}

/** Run the applet rows with the independently known root size in KiB. */
int main(int argc, char **argv)
{
    unsigned long image_kib = argc == 2 ? strtoul(argv[1], NULL, 10) : 0;

    report_begin(report_case(environ, "machine.procfiles.applets"));
    ps_rows();
    space_rows(image_kib);
    log_rows();
    route_rows();
    socket_rows();
    ipv6_rows();
    netdev_rows();
    interrupt_rows();
    report_end();
    return failures != 0;
}
