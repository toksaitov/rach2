/**
 * Bring the interfaces up from `/etc/network/interfaces`.
 *
 * `ifup -a` on the machine that has busybox; this is the same job for the
 * machine that has only this project's own code. It reads the same file,
 * in the same format, so one description of a network serves both images:
 *
 *     auto eth0
 *     iface eth0 inet dhcp
 *
 *     iface eth1 inet static
 *         address 192.168.0.105
 *         netmask 255.255.255.0
 *         gateway 192.168.0.1
 *     iface eth1 inet6 static
 *         address fd00::105/64
 *         gateway fd00::1
 *
 * `netup [-a] [INTERFACE ...]`. With `-a`, or with no arguments, it brings up
 * every interface an `auto` or `allow-hotplug` line names. A named interface
 * is brought up whether or not such a line names it, which is how one is
 * brought up by hand.
 *
 * A `dhcp` stanza runs `dhcpc -b`, which stays in the foreground through its
 * first round of discovery and then holds the lease, or goes on requesting
 * one, from the background. A `static` stanza is applied here, through the
 * interface and route ioctls `dhcpc` also uses.
 *
 * IPv6 needs no stanza: every card takes its link-local address when it
 * comes up and the addresses its routers advertise, made with the stable
 * secret netup puts in force first (ip6secret.h). An `inet6 auto` stanza
 * turns on `accept_ra` and `autoconf` for the interface, in case they were
 * turned off; `inet6 static` adds its address and default route through
 * the AF_INET6 ioctls, beside whatever the routers give; `inet6 manual`
 * does nothing. This image has no DHCPv6 client.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/route.h>
#include <fcntl.h>

#include "netcfg.h"
#include "ip6secret.h"

#define INTERFACES "/etc/network/interfaces"
#define RUN_DIR    "/var/run"
#define MAX_IFACES 8

/** Put an interface's name into an ifreq, bounded and terminated. */
static void set_ifr_name(struct ifreq *ifr, const char *name)
{
    size_t n = strnlen(name, IFNAMSIZ - 1);

    memset(ifr->ifr_name, 0, IFNAMSIZ);
    memcpy(ifr->ifr_name, name, n);
}

/** Put the IPv4 address `ip`, in host byte order, into `ifr`'s address. */
static void set_ifr_addr(struct ifreq *ifr, unsigned long ip)
{
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;

    memset(&ifr->ifr_addr, 0, sizeof(ifr->ifr_addr));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl((unsigned int)ip);
}

/**
 * Put back the address and mask an interface had.
 *
 * A configuration that failed part way leaves the machine on something nobody
 * chose. What it had before is not necessarily right either, but it is what the
 * administrator last saw, and on a machine across the room that is the
 * difference between a change that failed and a machine nobody can reach.
 * The netmask is put back only when both values are set, and a failure here
 * is not reported.
 */
static void restore(int fd, const char *name, unsigned long address, unsigned long netmask)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    set_ifr_name(&ifr, name);
    set_ifr_addr(&ifr, address);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        return;
    }
    if (address != 0 && netmask != 0) {
        memset(&ifr, 0, sizeof(ifr));
        set_ifr_name(&ifr, name);
        set_ifr_addr(&ifr, netmask);
        ioctl(fd, SIOCSIFNETMASK, &ifr);
    }
}

/**
 * The pid of the DHCP client that holds the interface `name`, as its claim
 * in `/var/run` says: the pid when that process exists, 0 when there is no
 * claim or its process has exited, and -1 when the claim cannot be read or
 * has no pid, as a client creating its claim leaves it for a moment. Only 0
 * lets a static stanza be applied.
 */
static long dhcp_holder(const char *name)
{
    char path[64];
    char iface[NETCFG_NAME_MAX];
    FILE *f;
    long pid = 0;

    netcfg_set_name(iface, name);
    snprintf(path, sizeof(path), "%s/dhcpc.%s.pid", RUN_DIR, iface);
    f = fopen(path, "r");
    if (!f) {
        return errno == ENOENT ? 0 : -1;
    }
    if (fscanf(f, "%ld", &pid) != 1 || ferror(f)) {
        pid = 0;
    }
    fclose(f);
    if (pid <= 0) {
        return -1;
    }
    if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) {
        return pid;
    }
    return 0;
}

/**
 * Apply a static stanza: the address, then the mask, then the route.
 *
 * An interface a DHCP client holds is left alone, and the stanza is checked
 * whole before anything changes. The address and mask in force are read
 * first, so that a failure part way can put them back; an unset interface
 * reads as 0.0.0.0, so a failure to read them is a real one and ends the
 * change. A stanza with no gateway removes the default route, since a
 * gateway that belonged to another network would send everything bound off
 * the subnet to a machine that is not there. A stanza with one sets it over
 * the old route without removing that first, so a failure to set it leaves
 * whatever the machine had.
 *
 * The route itself is never put back. The kernel drops a gateway that the new
 * address puts on another subnet, and the route in force is not read before
 * the change, so a failure to set the route puts back the address and mask
 * and says so: a machine on its own subnet is still reachable, and the route
 * can be set by hand.
 *
 * @return 0 when the stanza is in force, -1 after a message.
 */
static int apply_static(const struct netcfg_iface *s)
{
    struct ifreq ifr;
    struct rtentry rt;
    struct sockaddr_in *sin;
    unsigned long had_address = 0;
    unsigned long had_netmask = 0;
    int fd;

    {
        long holder = dhcp_holder(s->name);

        if (holder > 0) {
            fprintf(stderr,
                    "netup: %s: process %ld is holding it with a lease; stop it "
                    "with `dhcpc -i %s -k` before configuring a fixed address\n",
                    s->name, holder, s->name);
            return -1;
        }
        if (holder < 0) {
            fprintf(stderr,
                    "netup: %s: a DHCP client is claiming it; stop it with "
                    "`dhcpc -i %s -k` before configuring a fixed address\n",
                    s->name, s->name);
            return -1;
        }
    }
    {
        char err[96];

        if (netcfg_check_static(s, err, sizeof(err)) != 0) {
            fprintf(stderr, "netup: %s: %s\n", s->name, err);
            return -1;
        }
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "netup: %s: no socket: %s\n", s->name, strerror(errno));
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    set_ifr_name(&ifr, s->name);
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        fprintf(stderr, "netup: %s: cannot read the address in force: %s\n", s->name,
                strerror(errno));
        close(fd);
        return -1;
    }
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    had_address = ntohl(sin->sin_addr.s_addr);
    memset(&ifr, 0, sizeof(ifr));
    set_ifr_name(&ifr, s->name);
    if (ioctl(fd, SIOCGIFNETMASK, &ifr) < 0) {
        fprintf(stderr, "netup: %s: cannot read the netmask in force: %s\n", s->name,
                strerror(errno));
        close(fd);
        return -1;
    }
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    had_netmask = ntohl(sin->sin_addr.s_addr);

    memset(&ifr, 0, sizeof(ifr));
    set_ifr_name(&ifr, s->name);
    set_ifr_addr(&ifr, s->address);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        fprintf(stderr, "netup: %s: cannot set the address: %s\n", s->name, strerror(errno));
        close(fd);
        return -1;
    }
    if (s->netmask) {
        memset(&ifr, 0, sizeof(ifr));
        set_ifr_name(&ifr, s->name);
        set_ifr_addr(&ifr, s->netmask);
        if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
            fprintf(stderr, "netup: %s: cannot set the netmask: %s\n", s->name, strerror(errno));
            restore(fd, s->name, had_address, had_netmask);
            close(fd);
            return -1;
        }
    }
    if (!s->gateway) {
        memset(&rt, 0, sizeof(rt));
        sin = (struct sockaddr_in *)&rt.rt_dst;
        sin->sin_family = AF_INET;
        sin = (struct sockaddr_in *)&rt.rt_genmask;
        sin->sin_family = AF_INET;
        sin = (struct sockaddr_in *)&rt.rt_gateway;
        sin->sin_family = AF_INET;
        rt.rt_flags = RTF_UP;
        rt.rt_dev = (char *)s->name;
        while (ioctl(fd, SIOCDELRT, &rt) == 0) {
            ;
        }
    }

    if (s->gateway) {
        memset(&rt, 0, sizeof(rt));
        sin = (struct sockaddr_in *)&rt.rt_dst;
        sin->sin_family = AF_INET;
        sin = (struct sockaddr_in *)&rt.rt_genmask;
        sin->sin_family = AF_INET;
        sin = (struct sockaddr_in *)&rt.rt_gateway;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = htonl((unsigned int)s->gateway);
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        rt.rt_dev = (char *)s->name;
        if (ioctl(fd, SIOCADDRT, &rt) < 0) {
            fprintf(stderr, "netup: %s: cannot set the default route: %s\n", s->name,
                    strerror(errno));
            restore(fd, s->name, had_address, had_netmask);
            fprintf(stderr,
                    "netup: %s: the address in force is back; set the default "
                    "route by hand if the machine needs one\n",
                    s->name);
            close(fd);
            return -1;
        }
    }
    close(fd);
    printf("netup: %s: configured from %s\n", s->name, INTERFACES);
    fflush(stdout);
    return 0;
}

/* Linux's struct in6_ifreq, which the C library does not declare. */
struct netup_in6_ifreq {
    struct in6_addr addr;
    unsigned int prefixlen;
    int ifindex;
};

/** Write `value` to the IPv6 setting `key` of the interface `name` under
 * /proc/sys/net/ipv6/conf: 0, or -1 with errno. */
static int set_conf6(const char *name, const char *key, const char *value)
{
    char path[96];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/%s", name, key);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    n = write(fd, value, strlen(value));
    close(fd);
    return n == (ssize_t)strlen(value) ? 0 : -1;
}

/**
 * Apply an inet6 stanza: for `auto`, turn on the interface's `accept_ra`
 * and `autoconf`; for `static`, check it whole, then add the address and,
 * with a gateway, a default route through it at metric 1024, as Linux's
 * `route -A inet6` would. An address or route there already is taken as
 * applied. Nothing is undone after a failure, since nothing IPv4 has is
 * touched and an IPv6 address alone takes nothing away.
 *
 * @return 0 when the stanza is in force, -1 after a message.
 */
static int apply_inet6(const struct netcfg_iface *s)
{
    struct netup_in6_ifreq ifr6;
    struct in6_rtmsg rt;
    unsigned int index;
    char err[96];
    int fd;

    if (s->method6 == NETCFG6_AUTO) {
        if (set_conf6(s->name, "accept_ra", "1\n") != 0 ||
            set_conf6(s->name, "autoconf", "1\n") != 0) {
            fprintf(stderr, "netup: %s: cannot turn on IPv6 autoconfiguration: %s\n", s->name,
                    strerror(errno));
            return -1;
        }
        return 0;
    }
    if (s->method6 != NETCFG6_STATIC) {
        return 0;
    }
    if (netcfg_check_static6(s, err, sizeof(err)) != 0) {
        fprintf(stderr, "netup: %s: %s\n", s->name, err);
        return -1;
    }
    index = if_nametoindex(s->name);
    if (index == 0) {
        fprintf(stderr, "netup: %s: no such interface\n", s->name);
        return -1;
    }
    fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "netup: %s: no IPv6 socket: %s\n", s->name, strerror(errno));
        return -1;
    }
    memset(&ifr6, 0, sizeof(ifr6));
    memcpy(&ifr6.addr, s->address6, sizeof(ifr6.addr));
    ifr6.prefixlen = (unsigned int)s->prefix6;
    ifr6.ifindex = (int)index;
    if (ioctl(fd, SIOCSIFADDR, &ifr6) < 0 && errno != EEXIST) {
        fprintf(stderr, "netup: %s: cannot add the IPv6 address: %s\n", s->name, strerror(errno));
        close(fd);
        return -1;
    }
    if (s->gateway6[0] != 0 || memcmp(s->gateway6, s->gateway6 + 1, 15) != 0) {
        memset(&rt, 0, sizeof(rt));
        memcpy(&rt.rtmsg_gateway, s->gateway6, sizeof(rt.rtmsg_gateway));
        rt.rtmsg_flags = RTF_UP | RTF_GATEWAY;
        rt.rtmsg_metric = 1024;
        rt.rtmsg_ifindex = (int)index;
        if (ioctl(fd, SIOCADDRT, &rt) < 0 && errno != EEXIST) {
            fprintf(stderr, "netup: %s: cannot set the IPv6 default route: %s\n", s->name,
                    strerror(errno));
            close(fd);
            return -1;
        }
    }
    close(fd);
    printf("netup: %s: IPv6 configured from %s\n", s->name, INTERFACES);
    fflush(stdout);
    return 0;
}

/**
 * Run `dhcpc -b` for the interface and wait for its first round of discovery.
 *
 * The client exits 0 once it has gone to the background, with a lease or still
 * requesting one; any other status means it did not get that far. The message
 * for that case gives the status or the signal and nothing more, since a
 * client that stopped early may have left its address on the interface.
 *
 * @return 0 when the client reached the background, -1 after a message.
 */
static int run_dhcp(const struct netcfg_iface *s)
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) {
        fprintf(stderr, "netup: %s: cannot fork: %s\n", s->name, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        char *argv[6];

        argv[0] = (char *)"dhcpc";
        argv[1] = (char *)"-i";
        argv[2] = (char *)s->name;
        argv[3] = (char *)"-b";
        argv[4] = NULL;
        execv("/bin/dhcpc", argv);
        fprintf(stderr, "netup: %s: cannot run /bin/dhcpc: %s\n", s->name, strerror(errno));
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "netup: %s: cannot wait for dhcpc\n", s->name);
            return -1;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    if (WIFEXITED(status)) {
        fprintf(stderr, "netup: %s: dhcpc exited with status %d before reaching the background\n",
                s->name, WEXITSTATUS(status));
    } else {
        fprintf(stderr, "netup: %s: dhcpc was killed by signal %d before reaching the background\n",
                s->name, WTERMSIG(status));
    }
    return -1;
}

/**
 * Bring up each interface named on the command line and, with `-a` or with no
 * operand, each one an `auto` or `allow-hotplug` line declares.
 *
 * IPv6's stable secret is put in force before any interface is touched
 * (`ip6secret_keep`); a failure there is reported and the interfaces are
 * still brought up. Every interface is tried even after one fails, and its
 * inet stanza and its inet6 stanza each on its own. The status is 0 when
 * all of them came up, 1 when any failed or the file could not be used,
 * and 2 for a usage error or a bad list of names: one given twice, one no
 * interface can have, or more than eight.
 */
int main(int argc, char **argv)
{
    struct netcfg_iface want[MAX_IFACES];
    int count = 0;
    int all = (argc == 1);
    int failures = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            all = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: netup [-a] [interface ...]\n");
            return 2;
        } else if (count < MAX_IFACES) {
            int k;

            for (k = 0; k < count; k++) {
                if (strcmp(want[k].name, argv[i]) == 0) {
                    fprintf(stderr, "netup: %s named twice\n", argv[i]);
                    return 2;
                }
            }
            memset(&want[count], 0, sizeof(want[count]));
            if (!netcfg_name_ok(argv[i])) {
                fprintf(stderr, "netup: '%s' is not a name an interface can have\n", argv[i]);
                return 2;
            }
            netcfg_set_name(want[count].name, argv[i]);
            count++;
        } else {
            fprintf(stderr, "netup: too many interfaces named\n");
            return 2;
        }
    }

    count = netcfg_read(INTERFACES, want, count, MAX_IFACES, all);
    if (count < 0) {
        return 1;
    }
    if (count == 0) {
        fprintf(stderr, "netup: %s names no interface to bring up\n", INTERFACES);
        return 1;
    }

    (void)ip6secret_keep("netup");
    for (i = 0; i < count; i++) {
        if (!want[i].found && want[i].method6 == NETCFG6_NONE) {
            fprintf(stderr, "netup: %s: %s has no stanza for it\n", want[i].name, INTERFACES);
            failures++;
            continue;
        }
        if (want[i].bad6) {
            fprintf(stderr, "netup: %s: its inet6 stanza has something this cannot use\n",
                    want[i].name);
            failures++;
        } else if (apply_inet6(&want[i]) != 0) {
            failures++;
        }
        if (!want[i].found) {
            continue;
        }
        if (want[i].bad) {
            fprintf(stderr, "netup: %s: its stanza has something this cannot use\n", want[i].name);
            failures++;
            continue;
        }
        if (want[i].dhcp) {
            if (run_dhcp(&want[i]) != 0) {
                failures++;
            }
        } else if (apply_static(&want[i]) != 0) {
            failures++;
        }
    }
    return failures == 0 ? 0 : 1;
}
