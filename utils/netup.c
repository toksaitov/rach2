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
 *
 * `netup [-a] [INTERFACE ...]`. With `-a`, or with no arguments, it brings up
 * every interface an `auto` line names. A named interface is brought up whether
 * or not `auto` names it, which is how one is brought up by hand.
 *
 * A `dhcp` stanza runs `dhcpc`, which stays in the foreground until the first
 * answer and then has the lease from the background. A `static` stanza is
 * applied here, through the same ioctls.
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

#include "netcfg.h"

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
 * difference between a change that failed and a machine that is gone.
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
 * Whether a DHCP client is holding this interface.
 *
 * A static stanza applied while one is running is undone by that client's next
 * renewal, and its next refusal or expiry takes the static address off. So the
 * static path asks first and refuses, naming the process instead of fighting
 * something the administrator has forgotten about.
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
        return 0;
    }
    if (fscanf(f, "%ld", &pid) != 1) {
        pid = 0;
    }
    fclose(f);
    /* A signal of zero delivers nothing and reports whether it could have been
       delivered; `EPERM` is a process that is there and somebody else's, and
       only `ESRCH` is one that is gone. */
    if (pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM)) {
        return pid;
    }
    if (pid <= 0) {
        /* The file is there and says nothing this can read, which is what a
           claim being made right now looks like: the client creates it and
           writes into it. Busy is the safe reading; the alternative is
           configuring an interface a client is about to take. */
        return -1;
    }
    return 0;
}

/** Apply a static stanza: the address, then the mask, then the route. */
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
    /* What is there now, so a failure part way can put it back. An unset
       interface answers 0.0.0.0 instead of failing, so a failure here is a
       real one, and going on from it would mean restoring a machine to an
       address it never had. */
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
        /* A default route from before goes when this stanza names none: a
           gateway that belonged to another network is not a gateway here, and
           leaving it would send everything off-subnet to a machine that is not
           there. Where the stanza does name one, the route is not removed
           first: setting it replaces what is there, so a failure to set it
           leaves whatever the machine had instead of nothing. */
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
            /* The address is back; the route may not be. The kernel drops a
               gateway that the new address put on another subnet, and there is
               no call that reads the one in force, so there is nothing to put
               back from. Say so: a machine on its own subnet is still reachable
               and an administrator can set the route by hand. */
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

/** Run the DHCP client and wait for its first answer. */
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
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "netup: %s: dhcpc did not configure the interface\n", s->name);
        return -1;
    }
    return 0;
}

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

    for (i = 0; i < count; i++) {
        if (!want[i].found) {
            fprintf(stderr, "netup: %s: %s has no stanza for it\n", want[i].name, INTERFACES);
            failures++;
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
