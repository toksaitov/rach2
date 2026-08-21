/**
 * A DHCP client for the machine that has no busybox.
 *
 * os-real takes its address with busybox's `udhcpc`; this is the same job done
 * with this project's own code, for the image that has only this project's own
 * code in it. Both talk to the same kernel interface, so the two machines
 * behave the same way on a network: a packet socket to see the replies that
 * arrive before this machine owns an address, the `SIOCxIF*` ioctls to put the
 * answer on the interface, and a route for the gateway the lease names.
 *
 * `dhcpc [-i INTERFACE] [-b] [-l SECONDS]`. It has the lease for as long as
 * it runs: it renews at half the lease time, rebinds at seven eighths, and
 * when a lease finally expires it takes the address off and starts again,
 * because an address whose lease has run out belongs to somebody else now.
 * There is no configure-once mode, deliberately: a client that applied a lease
 * and exited would leave an address on the interface with nothing to take it
 * off when the lease ended, and the machine would go on answering for an
 * address the server had since given to somebody else.
 *
 * With `-b` it stays in the foreground until the first answer, so whatever
 * started it learns whether the machine is configured, and then goes to the
 * background and has the lease. A boot that found no server backgrounds too
 * instead of giving up, because a machine that gave up is one nobody can
 * reach until somebody walks to it. `dhcpc -i eth0 -k` stops the client that
 * has an interface and waits for the address to come off, which is what a
 * machine being moved to a fixed address needs first.
 *
 * Everything it sends it builds: the Ethernet payload is an IP header, a UDP
 * header and a BOOTP message, with both checksums computed. A DHCP server
 * answers a machine that has no address by broadcasting, and a machine with no
 * address cannot receive through the IP layer, which is the whole reason the
 * packet socket exists.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/route.h>
#include <netpacket/packet.h>

#include "dhcpmsg.h"

#ifndef ETH_P_IP
#define ETH_P_IP 0x0800
#endif
/* The hardware type BOOTP and ARP give Ethernet. */
#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER 1
#endif

#define RESOLV_CONF "/etc/resolv.conf"
#define RUN_DIR "/var/run"

/* The discover schedule: four tries, then a rest, forever. A machine plugged
 * into a switch that has not started forwarding must keep asking, because the
 * alternative is a machine nobody can reach until somebody walks to it. */
#define DISCOVER_TRIES 4
#define REST_SECONDS  30

/* How long `-k` waits for the client it signalled to let go. A client wakes
 * from its poll on the signal, so this is a bound on a client that is stuck,
 * not the time it takes. */
#define STOP_WAIT_SECONDS 10

/* The longest single wait. A lease may be measured in months, and a wait
 * expressed in milliseconds in an int cannot be. */
#define POLL_CHUNK 60

static const char *ifname = "eth0";
static int ifindex;
static unsigned char hwaddr[6];
static unsigned long xid;

/* The longest lease this client acts on; see dhcp_sane_lease. `-l` may lower
 * it and may not raise it, because everything above it overflows the
 * arithmetic the timers are made of. */
#define HARD_MAX_LEASE (7UL * 24 * 3600)
static unsigned long max_lease = HARD_MAX_LEASE;

/*
 * Set when a signal asks this client to stop.
 *
 * A client that was killed while holding a lease must take the address off on
 * its way out: the kernel's configuration outlives the process, and an address
 * nobody is renewing is one the server hands to somebody else while this
 * machine still answers for it. So the handler only writes this, and the loops
 * read it and leave through the same door every other failure leaves through.
 */
static volatile sig_atomic_t stopping;

static void stop_handler(int sig)
{
    (void)sig;
    stopping = 1;
}

static unsigned char out[DHCP_MAX];
static unsigned char in[1600];

/**
 * A transaction id nobody can guess.
 *
 * The kernel's random device fails closed unless the CPU seeded it, so a
 * machine that cannot give a random number says so instead of handing one
 * out; the fallback is the clock and the pid, which is weaker and is what a
 * machine without that CPU has. Two machines booted together must not choose
 * the same id, or each takes the other's answers.
 */
static unsigned long random_xid(void)
{
    unsigned long v = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        int n = (int)read(fd, &v, sizeof(v));

        close(fd);
        if (n == (int)sizeof(v) && v != 0) {
            return v;
        }
    }
    return ((unsigned long)time(0) << 8) ^ (unsigned long)getpid();
}







/** Ask the interface for its index and hardware address. */
static int read_interface(void)
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        fprintf(stderr, "dhcpc: no socket to ask the interface through\n");
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "dhcpc: %s: no such interface\n", ifname);
        close(fd);
        return -1;
    }
    ifindex = ifr.ifr_ifindex;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        fprintf(stderr, "dhcpc: %s: no hardware address\n", ifname);
        close(fd);
        return -1;
    }
    memcpy(hwaddr, ifr.ifr_hwaddr.sa_data, 6);
    close(fd);
    return 0;
}


/**
 * Build one message from this client's state and return its length.
 *
 * The transaction id and the hardware address are the client's, so only what
 * differs between messages is passed in: the type, the address in hand, and
 * the two options a request has.
 */
static unsigned int build(int type, unsigned long ciaddr, unsigned long server,
                          unsigned long requested, unsigned long dst)
{
    struct dhcp_request r;

    memset(&r, 0, sizeof(r));
    r.type = type;
    r.xid = xid;
    r.hwaddr = hwaddr;
    r.ciaddr = ciaddr;
    r.server = server;
    r.requested = requested;
    r.dst = dst;
    return dhcp_build(out, sizeof(out), &r);
}

/** Send a built message to `mac` through the packet socket. */
static int transmit(int fd, const unsigned char *mac, unsigned int len)
{
    struct sockaddr_ll to;

    memset(&to, 0, sizeof(to));
    to.sll_family = AF_PACKET;
    to.sll_protocol = htons(ETH_P_IP);
    to.sll_ifindex = ifindex;
    to.sll_hatype = ARPHRD_ETHER;
    to.sll_halen = 6;
    memcpy(to.sll_addr, mac, 6);
    if (sendto(fd, out, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        fprintf(stderr, "dhcpc: cannot send: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}



/**
 * Read one reply for us from the packet socket, or answer that none came before
 * `deadline`.
 *
 * Everything the machine receives arrives on this socket, so most of what it
 * reads is somebody else's: the checks here are what make a reply ours, and
 * they are made in the order that lets the cheapest one reject first.
 *
 * `want` is the message type this is waiting for, and anything else that
 * arrives is dropped, not returned: a segment with two servers on it, or
 * one server behind two relays, answers a discover twice, and a client that
 * treated the second offer as an answer to its request would abandon the
 * exchange it had just started. A refusal is returned whatever is awaited,
 * because it ends the exchange either way. Zero as `want` takes anything.
 *
 * @return the message type, 0 on timeout, or -1 on a socket that failed.
 */
static int receive(int fd, struct dhcp_lease *out_lease, int want, time_t deadline)
{
    for (;;) {
        struct pollfd p;
        time_t now = time(0);
        time_t remain;
        int ms;
        int n;
        int type;
        unsigned int ihl, udplen;

        if (stopping) {
            /* A signal wakes the poll below, and this is what that wakeup is
               for: the caller unconfigures and goes. */
            return 0;
        }
        if (now >= deadline) {
            return 0;
        }
        /* In pieces, so that a lease measured in months cannot turn a
           millisecond count into a negative number and a wait with no end. */
        remain = deadline - now;
        if (remain > POLL_CHUNK) {
            remain = POLL_CHUNK;
        }
        ms = (int)remain * 1000;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        n = poll(&p, 1, ms);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0 || !(p.revents & POLLIN)) {
            return 0;
        }
        n = (int)read(fd, in, sizeof(in));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        if (!dhcp_frame_ok(in, n, &ihl, &udplen)) {
            continue;
        }
        type = dhcp_parse(in + ihl + 8, udplen - 8, xid, hwaddr, out_lease);
        if (type == 0) {
            continue;
        }
        if (want == 0 || type == want || type == DHCP_NAK) {
            return type;
        }
    }
}


/** Format an address as dotted quad into `buf`, which needs 16 bytes. */
static void format_ip(unsigned long ip, char *buf)
{
    sprintf(buf, "%lu.%lu.%lu.%lu", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
            (ip >> 8) & 0xff, ip & 0xff);
}

/** Put an IPv4 address into an ifreq's sockaddr. */
static void set_ifr_addr(struct ifreq *ifr, unsigned long ip)
{
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;

    memset(&ifr->ifr_addr, 0, sizeof(ifr->ifr_addr));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl((unsigned int)ip);
}

/**
 * Write `/etc/resolv.conf` from the lease.
 *
 * Through a temporary beside it, so a reader never sees half of one, and on
 * every lease, not only on one that names a server: leaving the last
 * network's servers behind would point this machine at a network it is no
 * longer on.
 */
static int write_resolv(const struct dhcp_lease *l)
{
    char tmp[64];
    char buf[32];
    FILE *f;
    int i;

    sprintf(tmp, "%s.new", RESOLV_CONF);
    f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "dhcpc: resolver: cannot write %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    if (l->domain[0] && fprintf(f, "search %s\n", l->domain) < 0) {
        fclose(f);
        remove(tmp);
        return -1;
    }
    for (i = 0; i < l->dns_count; i++) {
        format_ip(l->dns[i], buf);
        if (fprintf(f, "nameserver %s\n", buf) < 0) {
            fclose(f);
            remove(tmp);
            return -1;
        }
    }
    if (fclose(f) != 0) {
        remove(tmp);
        fprintf(stderr, "dhcpc: resolver: cannot finish %s\n", tmp);
        return -1;
    }
    if (rename(tmp, RESOLV_CONF) != 0) {
        remove(tmp);
        fprintf(stderr, "dhcpc: resolver: cannot replace %s: %s\n", RESOLV_CONF,
                strerror(errno));
        return -1;
    }
    return 0;
}

/** Where this client records that it has the interface. */
static char pidpath[64];
static int holds_interface;

/* The address this client has put on the interface, 0 when none. It is written
 * into the claim so that a client killed outright can be cleaned up after. */
static unsigned long holding;

/**
 * Write a claim: the process holding the interface, and the address it has put
 * on it.
 *
 * The address is there so that a client which was killed outright can still be
 * cleaned up after. Nothing runs on SIGKILL, so such a client leaves its
 * address on the interface with no lease behind it; whoever takes the claim
 * over reads what was configured and takes it off, but only if the interface
 * still has exactly that address, since an administrator may have set
 * another one in the meantime.
 */
static int write_claim(int fd, long pid, unsigned long addr)
{
    char text[48];
    int n = snprintf(text, sizeof(text), "%ld\n%lu.%lu.%lu.%lu\n", pid,
                     (addr >> 24) & 0xff, (addr >> 16) & 0xff, (addr >> 8) & 0xff,
                     addr & 0xff);

    if (n <= 0 || write(fd, text, (size_t)n) != n) {
        return -1;
    }
    return 0;
}


/**
 * Whether a process exists.
 *
 * A signal of zero delivers nothing and reports whether it could have been
 * delivered. `EPERM` says the process is there and belongs to somebody else,
 * which is still there; only `ESRCH` says it is gone.
 */
static int pid_alive(long pid)
{
    if (pid <= 0) {
        return 0;
    }
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

/**
 * The process named in the claim, or 0 when there is none this can read.
 *
 * `present` is set when the file is there at all, which is what tells an empty
 * or unreadable claim (somebody is making it right now) from no claim.
 * It may be null.
 */
static long claim_read(unsigned long *addr, int *present)
{
    FILE *f = fopen(pidpath, "r");
    unsigned int q[4];
    long pid = 0;

    if (addr) {
        *addr = 0;
    }
    if (present) {
        *present = 0;
    }
    if (!f) {
        return 0;
    }
    if (present) {
        *present = 1;
    }
    if (fscanf(f, "%ld", &pid) != 1 || pid <= 0) {
        pid = 0;
    } else if (addr && fscanf(f, "%3u.%3u.%3u.%3u", &q[0], &q[1], &q[2], &q[3]) == 4 &&
               q[0] < 256 && q[1] < 256 && q[2] < 256 && q[3] < 256) {
        *addr = ((unsigned long)q[0] << 24) | ((unsigned long)q[1] << 16) |
                ((unsigned long)q[2] << 8) | (unsigned long)q[3];
    }
    fclose(f);
    return pid;
}

/** The process named in the claim, without asking what it configured. */
static long claim_pid(int *present)
{
    return claim_read(0, present);
}

/**
 * Replace the claim this client holds with one naming `pid` and `addr`.
 *
 * @return 0 when the claim now says that, -1 when it could not be replaced.
 */
static int record_claim(long pid, unsigned long addr)
{
    char tmp[96];
    int fd;
    int ok;

    if (!holds_interface) {
        return 0;
    }
    /* Written beside the claim and renamed over it, not written into
       it. This kernel has no ftruncate, so a rewrite in place would have to
       open with O_TRUNC, and a client that died in that instant would leave a
       claim holding nothing: both the takeover and the stop path read an empty
       claim as busy, so nothing would start and nothing would know which
       address to take off. The rename replaces one name with another inside
       one system call. */
    snprintf(tmp, sizeof(tmp), "%s.%ld", pidpath, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    ok = write_claim(fd, pid, addr) == 0;
    close(fd);
    if (!ok) {
        remove(tmp);
        return -1;
    }
    /* The claim must still be this client's to replace. The parent publishing
       its child's pid is the one caller writing a pid other than its own, and
       the claim it replaces is still its own at that moment. */
    if (claim_pid(0) != (long)getpid()) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, pidpath) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

/**
 * Take an address off the interface, but only the one named.
 *
 * This is the cleanup after a client that was killed outright: it left an
 * address behind and no lease to go with it, and the machine would go on
 * answering for that address after the server had given it to somebody else.
 * The comparison is what keeps it safe: an interface holding a different
 * address now is one somebody configured on purpose.
 *
 * The claim it was read from must not be removed until this has answered: a
 * claim removed after a cleanup that failed is an address nobody can find out
 * about, since the record of what to remove has gone with it.
 *
 * @return 0 when the address is no longer there, -1 when it could not be
 *         removed.
 */
static int drop_recorded_address(unsigned long addr)
{
    struct ifreq ifr;
    struct sockaddr_in *sin;
    char a[16];
    int rc = -1;
    int fd;

    if (addr == 0) {
        return 0;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "dhcpc: %s: no socket to clean up with: %s\n", ifname,
                strerror(errno));
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
        fprintf(stderr, "dhcpc: %s: cannot read the address in force: %s\n", ifname,
                strerror(errno));
        close(fd);
        return -1;
    }
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    if (ntohl(sin->sin_addr.s_addr) != addr) {
        close(fd);
        return 0; /* something else is there; it is not this client's to remove */
    }
    format_ip(addr, a);
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    set_ifr_addr(&ifr, 0);
    if (ioctl(fd, SIOCSIFADDR, &ifr) == 0) {
        fprintf(stderr, "dhcpc: %s: took off %s, left by a client that is gone\n",
                ifname, a);
        rc = 0;
    } else {
        fprintf(stderr, "dhcpc: %s: cannot take off %s: %s\n", ifname, a,
                strerror(errno));
    }
    close(fd);
    return rc;
}

/**
 * Stop the client holding this interface and wait for it to let go.
 *
 * There is no other way to take a leased address off this machine: the client
 * that has it renews it, so an address that is to be replaced by a fixed one,
 * or by none, needs its client stopped first. The signal is TERM, which the
 * client answers by unconfiguring the interface and dropping its claim, and
 * this waits for the claim to go so that whatever runs next does not race it.
 *
 * @return 0 when nothing has the interface afterwards.
 */
static int stop_client(void)
{
    unsigned long left = 0;
    int present = 0;
    long pid = 0;
    int attempt;
    int i;

    snprintf(pidpath, sizeof(pidpath), "%s/dhcpc.%s.pid", RUN_DIR, ifname);
    /* Twice around at most: a claim naming a process that is gone may be taken
       by a new client between the reading and the removing, and removing that
       one's file would leave a client nothing can find. So the claim is read
       once more immediately before it goes, and a claim that changed under
       this is looked at again, not removed. */
    for (attempt = 0; attempt < 2; attempt++) {
        pid = claim_pid(&present);
        if (pid <= 0) {
            if (present) {
                fprintf(stderr,
                        "dhcpc: %s: the claim names no process; leaving it alone\n",
                        ifname);
            } else {
                fprintf(stderr, "dhcpc: %s: no client is holding it\n", ifname);
            }
            return 1;
        }
        if (pid_alive(pid)) {
            break;
        }
        if (claim_read(&left, 0) != pid) {
            continue; /* it changed hands; read it again */
        }
        /* The client is gone and its claim outlived it, which is what a
           machine that lost power looks like. The address it left goes first:
           the claim is the only record of which address that is, so a claim
           removed before it would leave an address nothing can find. */
        if (drop_recorded_address(left) != 0) {
            fprintf(stderr, "dhcpc: %s: the claim of process %ld is left in place\n",
                    ifname, pid);
            return 1;
        }
        if (remove(pidpath) != 0) {
            fprintf(stderr, "dhcpc: %s: cannot remove the claim of process %ld: %s\n",
                    ifname, pid, strerror(errno));
            return 1;
        }
        printf("dhcpc: %s: process %ld was gone; its claim is removed\n", ifname, pid);
        fflush(stdout);
        return 0;
    }
    if (!pid_alive(pid)) {
        fprintf(stderr, "dhcpc: %s: the claim keeps changing hands; nothing was "
                        "stopped\n", ifname);
        return 1;
    }
    if (kill((pid_t)pid, SIGTERM) != 0) {
        fprintf(stderr, "dhcpc: %s: cannot stop process %ld: %s\n", ifname, pid,
                strerror(errno));
        return 1;
    }
    for (i = 0; i < STOP_WAIT_SECONDS; i++) {
        long now;

        sleep(1);
        now = claim_pid(&present);
        if (now <= 0 && !present) {
            printf("dhcpc: %s: stopped process %ld\n", ifname, pid);
            fflush(stdout);
            return 0;
        }
        if (now > 0 && now != pid) {
            /* The claim changed hands while this was waiting, which is what a
               client backgrounding itself looks like: the parent published its
               child and went. The signal has to follow it. */
            pid = now;
            if (kill((pid_t)pid, SIGTERM) != 0 && errno != ESRCH) {
                fprintf(stderr, "dhcpc: %s: cannot stop process %ld: %s\n", ifname, pid,
                        strerror(errno));
                return 1;
            }
        }
    }
    fprintf(stderr, "dhcpc: %s: process %ld has not let go after %d seconds\n", ifname,
            pid, STOP_WAIT_SECONDS);
    return 1;
}

/**
 * Confirm this client still holds the interface, and take the claim back if
 * nothing has it.
 *
 * Two clients started at the same instant over a claim left by a machine that
 * lost power can both find it dead and both take it; one of them then has a
 * claim the other removed. This is what makes that visible: the claim is read
 * again before the interface is touched, and a client that finds another live
 * client named there leaves without configuring anything and without removing
 * a claim that is not its own.
 *
 * A claim that is simply gone is a different thing and must not be read as a
 * loss: this client is still the one the interface is configured for, and
 * walking away would leave an address with nobody to take it off when its
 * lease ends. So it is written again.
 *
 * It is a check and not a lock, because this file system has no lock to offer:
 * `fcntl` locking is granted unconditionally here. One administrator at a time
 * is the contract; this is what keeps a lost race from becoming two clients
 * renewing one address.
 *
 * `strict` is set before the first lease is applied and clear afterwards.
 * Nothing is configured yet at the first call, so a claim this client cannot
 * establish is a reason not to start; once an address is in force, walking away
 * from it would leave it with nobody to take it off, which is worse than two
 * clients renewing one address.
 *
 * @return 1 when the interface is still this client's to configure.
 */
static int claim_is_ours(int strict)
{
    long who;
    int present = 0;
    int fd;

    if (!holds_interface) {
        return 0;
    }
    who = claim_pid(&present);
    if (who == (long)getpid()) {
        return 1;
    }
    if (who > 0 && pid_alive(who)) {
        fprintf(stderr, "dhcpc: %s: process %ld holds the claim on it now\n", ifname,
                who);
        holds_interface = 0; /* not ours to release */
        return 0;
    }
    if (present && who <= 0) {
        /* There but holding nothing readable, which is what another client
           creating its claim this instant looks like. Before a lease is
           applied that is a reason to stop; once one is in force this client
           keeps the address it is renewing, and if that other client means to
           take over, the check above will say so on the next renewal. */
        fprintf(stderr, "dhcpc: %s: the claim is unreadable; %s\n", ifname,
                strict ? "not configuring the interface" : "holding the lease");
        return !strict;
    }
    if (present) {
        remove(pidpath); /* it names a process that is gone */
    }
    fd = open(pidpath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        int ok = write_claim(fd, (long)getpid(), holding) == 0;

        close(fd);
        if (ok) {
            fprintf(stderr, "dhcpc: %s: the claim had gone; it is written again\n",
                    ifname);
            return 1;
        }
        remove(pidpath);
        fprintf(stderr, "dhcpc: %s: the claim had gone and cannot be written again: %s\n",
                ifname, strerror(errno));
        return !strict;
    }
    who = claim_pid(0);
    if (who > 0 && who != (long)getpid() && pid_alive(who)) {
        fprintf(stderr, "dhcpc: %s: process %ld holds the claim on it now\n", ifname,
                who);
        holds_interface = 0;
        return 0;
    }
    /* Whatever is there, it names nobody alive but this client. Before a lease
       is applied that is a reason to stop: nothing is configured yet, so
       stopping costs nothing and starting would put an address on an interface
       this client cannot claim. Once a lease is in force it is the other way
       around, and by some distance: two clients on one interface ask with the
       same hardware address and are given the same address, while an address
       nobody renews is one the server hands to another machine. So a client
       that is already holding one goes on holding it. */
    return !strict;
}

/**
 * Claim the interface, or report who has it.
 *
 * Two clients on one interface fight: each renews, either may be refused, and
 * either one's expiry takes an address the other still holds. The claim is the
 * creation of the file, which either happens or does not, so two clients
 * starting at the same moment cannot both believe they won. The pid goes in
 * before the descriptor is let go, so the file is never there and empty, and a
 * file that IS empty is read as a claim being made by somebody else rather than
 * as one to take over.
 *
 * A claim naming a process that is not there is taken over once. Anything else
 * that goes wrong is a refusal: a client that cannot say it has an interface
 * must not act as though it does.
 *
 * Init empties `/var/run` at boot, so a claim does not outlive the machine.
 *
 * @return 0 when the interface is this client's.
 */
static int claim_interface(void)
{
    int tries;

    snprintf(pidpath, sizeof(pidpath), "%s/dhcpc.%s.pid", RUN_DIR, ifname);
    for (tries = 0; tries < 2; tries++) {
        unsigned long left = 0;
        long other;
        int fd = open(pidpath, O_WRONLY | O_CREAT | O_EXCL, 0644);

        if (fd >= 0) {
            if (write_claim(fd, (long)getpid(), 0) != 0) {
                close(fd);
                remove(pidpath);
                fprintf(stderr, "dhcpc: %s: cannot record the claim: %s\n", ifname,
                        strerror(errno));
                return -1;
            }
            close(fd);
            holds_interface = 1;
            return 0;
        }
        if (errno != EEXIST) {
            fprintf(stderr, "dhcpc: %s: cannot claim it: %s\n", ifname, strerror(errno));
            return -1;
        }
        other = claim_read(&left, 0);
        if (other <= 0) {
            /* Empty, or not a number: somebody is making this claim right now,
               or left something this cannot judge. Either way it is not
               this client's to take. */
            fprintf(stderr, "dhcpc: %s: another client is claiming it\n", ifname);
            return -1;
        }
        if (pid_alive(other)) {
            fprintf(stderr, "dhcpc: %s: process %ld is already holding it\n", ifname,
                    other);
            return -1;
        }
        /* Read once more, immediately before removing it: between the first
           read and here another client may have taken the claim, and removing
           its file would leave two clients on one interface. This narrows that
           window instead of closing it; `claim_is_ours` above is what makes
           the loss visible if it happens anyway. */
        if (claim_read(&left, 0) != other) {
            continue;
        }
        /* The address it left goes first: the claim is the only record of
           which address that is, so removing the claim before the address
           would leave one nothing can find. */
        if (drop_recorded_address(left) != 0) {
            fprintf(stderr, "dhcpc: %s: the claim of process %ld is left in place\n",
                    ifname, other);
            return -1;
        }
        remove(pidpath); /* the client that left this behind is gone */
    }
    fprintf(stderr, "dhcpc: %s: cannot claim it\n", ifname);
    return -1;
}

/** Give the interface back. */
static void release_interface(void)
{
    if (holds_interface) {
        remove(pidpath);
        holds_interface = 0;
    }
}

/**
 * Put the lease on the interface: the address, the mask, the route, the
 * resolver.
 *
 * The address goes on before the mask, which is the order the two ioctls are
 * meant to be issued in, and the route last because a gateway must be on a
 * subnet this machine already has.
 *
 * `strict` is set for the first lease and clear for a renewal, and it decides
 * what a claim this client cannot write means: before anything is configured,
 * it means do not configure; after, it means carry on holding what is there.
 */
static int apply(int fd, const struct dhcp_lease *l, int strict)
{
    struct ifreq ifr;
    struct rtentry rt;
    struct sockaddr_in *sin;
    char a[16], b[16];

    /* Recorded before it is applied, not after: a client that dies in between
       leaves a claim naming an address the interface does not hold, and the
       cleanup compares before it removes, so nothing is taken off that this
       client never put on. A first lease is not applied at all if the record
       cannot be written, since nothing would then know what to take off; a
       renewal goes on, because the address is already there and stopping would
       leave it with nobody at all. */
    if (record_claim((long)getpid(), l->address) != 0) {
        fprintf(stderr, "dhcpc: %s: cannot record the address in the claim: %s\n",
                ifname, strerror(errno));
        if (strict) {
            return -1;
        }
    }
    holding = l->address;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    set_ifr_addr(&ifr, l->address);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        fprintf(stderr, "dhcpc: cannot set the address: %s\n", strerror(errno));
        return -1;
    }
    if (l->netmask) {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
        set_ifr_addr(&ifr, l->netmask);
        if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
            fprintf(stderr, "dhcpc: cannot set the netmask: %s\n", strerror(errno));
            return -1;
        }
    }

    /* The old default route goes whether or not this lease replaces it: a
       gateway from the network this machine was on before is not one here. */
    memset(&rt, 0, sizeof(rt));
    sin = (struct sockaddr_in *)&rt.rt_dst;
    sin->sin_family = AF_INET;
    sin = (struct sockaddr_in *)&rt.rt_genmask;
    sin->sin_family = AF_INET;
    sin = (struct sockaddr_in *)&rt.rt_gateway;
    sin->sin_family = AF_INET;
    rt.rt_flags = RTF_UP;
    rt.rt_dev = (char *)ifname;
    while (ioctl(fd, SIOCDELRT, &rt) == 0) {
        ;
    }
    if (l->router) {
        sin = (struct sockaddr_in *)&rt.rt_gateway;
        sin->sin_addr.s_addr = htonl((unsigned int)l->router);
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        if (ioctl(fd, SIOCADDRT, &rt) < 0) {
            fprintf(stderr, "dhcpc: cannot set the default route: %s\n", strerror(errno));
            return -1;
        }
    } else {
        fprintf(stderr, "dhcpc: the lease names no router: "
                        "nothing off this subnet is reachable\n");
    }

    if (write_resolv(l) != 0) {
        return -1;
    }

    format_ip(l->address, a);
    format_ip(l->netmask ? l->netmask : 0, b);
    printf("dhcpc: %s: lease of %s netmask %s for %lu seconds\n", ifname, a, b, l->seconds);
    fflush(stdout);
    return 0;
}

/**
 * Take the address off, which is what an expired lease leaves behind.
 *
 * Through the socket this client already has, not a new one: the machine has
 * a fixed number of sockets and this runs at the moment things are going wrong,
 * which is exactly when there may be none left. A removal that fails is said
 * out loud, because an address left behind is one the server will hand to
 * somebody else.
 */
static void unconfigure(int fd)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    set_ifr_addr(&ifr, 0);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        fprintf(stderr, "dhcpc: cannot take the address off: %s\n", strerror(errno));
    }
    holding = 0;
    (void)record_claim((long)getpid(), 0);
}

/**
 * Ask for a lease and take it.
 *
 * Discover, then request the address the first offer names, then apply the
 * acknowledgement. A refusal starts again, and so does silence after the
 * schedule runs out; the caller decides whether to try another round.
 *
 * @return 0 with `l` filled, or -1 when nobody answered.
 */
static int obtain(int fd, struct dhcp_lease *l, time_t *asked)
{
    static const unsigned char bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    struct dhcp_lease offer;
    int try;

    for (try = 0; try < DISCOVER_TRIES; try++) {
        unsigned int len;
        int type;
        int wait = 2 << try;

        if (stopping) {
            return -1;
        }

        xid = random_xid();
        len = build(DHCP_DISCOVER, 0, 0, 0, 0xFFFFFFFFUL);
        if (transmit(fd, bcast, len) != 0) {
            return -1;
        }
        printf("dhcpc: %s: asking for an address\n", ifname);
        fflush(stdout);

        type = receive(fd, &offer, DHCP_OFFER, time(0) + wait);
        if (type != DHCP_OFFER) {
            continue;
        }
        if (offer.address == 0 || offer.server == 0) {
            continue;
        }

        len = build(DHCP_REQUEST, 0, offer.server, offer.address, 0xFFFFFFFFUL);
        /* The lease runs from here, not from the answer: the server starts
           counting when it grants, and an answer that took ten seconds to
           arrive has spent ten seconds of the lease. */
        *asked = time(0);
        if (transmit(fd, bcast, len) != 0) {
            return -1;
        }
        type = receive(fd, l, DHCP_ACK, time(0) + wait);
        if (type == DHCP_ACK) {
            l->seconds = l->has_seconds ? dhcp_sane_lease(l->seconds, max_lease) : 0;
        }
        if (type == DHCP_ACK && l->seconds == 0) {
            /* An acknowledgement must say how long the address is this
               machine's, and say a length there is room to renew inside. One
               that does not has told this machine to keep an address until
               somebody else is given it. */
            fprintf(stderr, "dhcpc: the server acknowledged without a lease time this "
                            "client can hold the address for\n");
            continue;
        }
        if (type == DHCP_ACK && l->address != 0) {
            char why[96];

            if (dhcp_lease_usable(l, why, sizeof(why)) != 0) {
                /* Applying it would change the address and then be refused
                   half way, leaving the machine with neither. Asking again is
                   the answer: a server that keeps saying this is one to keep
                   asking, not one to give up on. */
                fprintf(stderr, "dhcpc: %s: the lease cannot be used: %s\n", ifname, why);
                continue;
            }
            if (l->server == 0) {
                l->server = offer.server;
            }
            return 0;
        }
        if (type == DHCP_NAK) {
            fprintf(stderr, "dhcpc: the server refused the address it offered\n");
        }
    }
    return -1;
}

/**
 * Send a renewal to the server that granted the lease, and wait for its answer.
 *
 * This one goes through the IP layer on an ordinary datagram socket connected
 * to the server, which is what a renewal is: the machine has an address by
 * then, the server has one, and there is nothing about it that needs the
 * packet socket. Connecting is what makes the answer this socket's own, since a
 * connected datagram socket hears from its peer and from nobody else.
 *
 * The message is the BOOTP part alone; the kernel writes the IP and UDP headers
 * and finds the route, which is the whole difference from a discover. The wait
 * ends at `limit`, which is the boundary that decides what to do next.
 *
 * @return the message type, 0 on silence, or -1 on a socket that failed.
 */
static int renew_unicast(struct dhcp_lease *l, struct dhcp_lease *got, unsigned int bootp_len,
                        time_t limit)
{
    struct sockaddr_in local, peer;
    struct pollfd p;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    int type = 0;
    int n;

    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(DHCP_CLIENT_PORT);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(fd);
        return -1;
    }
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(DHCP_SERVER_PORT);
    peer.sin_addr.s_addr = htonl((unsigned int)l->server);
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        close(fd);
        return -1;
    }
    if (write(fd, out + 28, bootp_len) < 0) {
        close(fd);
        return -1;
    }

    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    {
        time_t now = time(0);
        time_t wait = limit > now ? limit - now : 0;

        if (wait > 10) {
            wait = 10;
        }
        n = poll(&p, 1, (int)wait * 1000);
    }
    if (n > 0 && (p.revents & POLLIN)) {
        n = (int)read(fd, in, sizeof(in));
        if (n >= (int)(OPTIONS_OFF)) {
            type = dhcp_parse(in, (unsigned int)n, xid, hwaddr, got);
        }
    }
    close(fd);
    return type;
}


/**
 * Wait a little before asking again, and never past the next boundary.
 *
 * Every path that did not get an answer it can use comes through here, so a
 * server that answers wrongly as fast as it can is asked no faster than a
 * server that says nothing at all.
 */
static void rest_until(time_t limit)
{
    time_t left = limit - time(0);

    if (left > 0) {
        sleep(left < 2 ? (unsigned int)left : 2);
    }
}

/**
 * Hold a lease: renew at the moment the lease names, rebind at the next, and
 * give the address up when it runs out.
 *
 * A renewal goes to the server that granted the lease and nowhere else; a
 * rebinding goes to everyone through the packet socket, because by then the
 * server that granted it has stopped answering and there may be another.
 *
 * @return 0 when the lease was let go and a new one should be asked for.
 */
static int hold(int fd, struct dhcp_lease *l, time_t asked)
{
    static const unsigned char bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    time_t start = asked;
    time_t t1, t2, end;

    dhcp_boundaries(l, start, &t1, &t2, &end);

    for (;;) {
        struct dhcp_lease got;
        int type;
        time_t now = time(0);
        time_t limit;
        unsigned int len;

        if (stopping) {
            return 0;
        }
        if (now >= end) {
            fprintf(stderr, "dhcpc: the lease ran out; giving the address up\n");
            unconfigure(fd);
            return 0;
        }
        if (now < t1) {
            if (receive(fd, &got, DHCP_ACK, t1 < end ? t1 : end) < 0) {
                return -1;
            }
            continue;
        }

        /* Renewing until T2, then rebinding. Both ask for the same thing; what
           differs is who is asked and how. Every wait below is cut short by the
           next boundary, so a renewal that goes unanswered cannot carry the
           machine past the moment it should have started rebinding, or past the
           moment the address stops being its own. */
        xid = random_xid();
        if (now < t2) {
            limit = t2 < end ? t2 : end;
            len = build(DHCP_REQUEST, l->address, 0, 0, l->server);
            type = renew_unicast(l, &got, len - 28, limit);
            if (type < 0) {
                type = 0; /* the socket failed; treat it as silence and retry */
            }
        } else {
            limit = end;
            len = build(DHCP_REQUEST, l->address, 0, 0, 0xFFFFFFFFUL);
            if (transmit(fd, bcast, len) != 0) {
                unconfigure(fd);
                return 0; /* ask again, not leave the machine with nothing */
            }
            /* Not the whole way to the expiry: one lost request would then cost
               the last eighth of the lease in silence, which is exactly the
               interval rebinding exists for. Half of what is left, which is
               what RFC 2131 asks for, and never less than a second, so that
               even the shortest lease this client accepts is asked about more
               than once before it runs out. */
            {
                time_t left_now = end - time(0);
                time_t wait = left_now / 2;

                if (wait < 1) {
                    wait = 1;
                }
                limit = time(0) + wait;
                if (limit > end) {
                    limit = end;
                }
            }
            type = receive(fd, &got, DHCP_ACK, limit);
        }
        if (type == DHCP_ACK && got.address == l->address) {
            /* The lease runs from the moment this machine asked, not from the
               moment the answer arrived: the server started counting first. */
            {
                char why[96];

                if (dhcp_lease_usable(&got, why, sizeof(why)) != 0) {
                    /* The lease in force stays: it is working, and this answer
                       is one the machine cannot be configured from. */
                    fprintf(stderr, "dhcpc: the renewal cannot be used: %s; the lease "
                                    "in force is unchanged\n", why);
                    rest_until(limit);
                    continue;
                }
            }
            got.seconds = got.has_seconds ? dhcp_sane_lease(got.seconds, max_lease) : 0;
            if (got.seconds == 0) {
                /* Nothing to compute a new expiry from. The lease in force
                   keeps its own boundaries: moving them on an answer that
                   named no duration would let a malformed server extend an
                   address forever while it hands that address to somebody
                   else. The wait below is taken all the same, because a server
                   answering every renewal this way would otherwise be asked
                   again the moment its answer arrived. */
                fprintf(stderr, "dhcpc: the renewal was acknowledged without a lease "
                                "time this client can hold; the lease in force is "
                                "unchanged\n");
                rest_until(limit);
                continue;
            }
            if (got.server == 0) {
                got.server = l->server;
            }
            *l = got;
            if (!claim_is_ours(0)) {
                return -2;
            }
            if (apply(fd, l, 0) != 0) {
                /* The address is off and there is no lease in force, but the
                   machine still has to end up reachable, so this is the same
                   answer as a lease that ran out: ask for one again. */
                fprintf(stderr, "dhcpc: the renewal could not be applied; asking for an "
                                "address again\n");
                unconfigure(fd);
                return 0;
            }
            /* Said in its own words, because a renewal and a fresh lease are
               different things and a reader of the log should not have to
               guess which one happened. */
            printf("dhcpc: %s: renewed for %lu seconds\n", ifname, l->seconds);
            fflush(stdout);
            start = now;
            dhcp_boundaries(l, start, &t1, &t2, &end);
            continue;
        }
        if (type == DHCP_NAK) {
            fprintf(stderr, "dhcpc: the server refused the renewal; giving the address up\n");
            unconfigure(fd);
            return 0;
        }
        /* Nothing answered. Wait a little and ask again, but never past the
           boundary that decides what to ask next. */
        rest_until(limit);
    }
}

/**
 * Let the caller go and carry on in a child.
 *
 * The parent exits reporting success, so a boot script learns the machine is
 * configured (or that it will keep trying) and moves on; the child has the
 * lease for as long as the machine runs.
 *
 * The child does not begin holding the lease until the parent has published
 * its pid, which is what the pipe between them is for.
 *
 * The child takes a session of its own and gives back the three standard
 * descriptors, replacing them with the console. Keeping them would hold open a
 * pipe whatever started this is reading, so a command substitution around
 * `netup` would wait for a client that never exits; and a reader that went away
 * would leave every later line going nowhere. SIGPIPE itself is ignored from
 * the start, in `main`, since a client can be holding a lease well before it
 * reaches here.
 *
 * @return 0 in the child; the parent does not return.
 */
static int go_background(void)
{
    long parent = (long)getpid();
    pid_t pid;
    int handshake[2];
    char answer = 0;
    int fd;

    fflush(stdout);
    fflush(stderr);
    if (pipe(handshake) != 0) {
        fprintf(stderr, "dhcpc: cannot make a handshake: %s\n", strerror(errno));
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "dhcpc: cannot fork: %s\n", strerror(errno));
        close(handshake[0]);
        close(handshake[1]);
        return -1;
    }
    if (pid > 0) {
        /* The parent publishes the child's pid before it goes, so the claim
           never names a process that has already exited. A child it cannot
           publish is a client nothing can find and nothing can stop, so it is
           refused instead and cleans up after itself; and a stop that arrived
           during the handoff is passed on, since the signal reached the parent
           and the child is what is left. */
        close(handshake[0]);
        if (record_claim((long)pid, holding) != 0) {
            /* The claim still names this process, and it is left that way on
               purpose: the child is about to read the refusal below, and that
               claim is how it knows the address is still its own to take off.
               A parent that removed it here would leave a configured address
               with no claim, which is exactly what nothing could then find. */
            fprintf(stderr, "dhcpc: cannot hand the claim to the background client\n");
            (void)write(handshake[1], "n", 1);
            close(handshake[1]);
            _exit(1);
        }
        (void)write(handshake[1], "y", 1);
        close(handshake[1]);
        if (stopping) {
            kill(pid, SIGTERM);
        }
        _exit(0);
    }

    /* The child waits to be published. Until the claim names it, it is a
       client holding a lease that nothing can find and nothing can stop: a
       `dhcpc -k` in that instant would read the parent's pid, and if the
       parent has died it would take the address off, remove the claim, and
       report that it had stopped a client that is still running. An end of
       file here is exactly that death, and the answer to it is to leave. */
    close(handshake[1]);
    for (;;) {
        ssize_t n = read(handshake[0], &answer, 1);

        if (n == 1) {
            break;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        answer = 0;
        break;
    }
    close(handshake[0]);
    if (answer != 'y') {
        fprintf(stderr, "dhcpc: %s: the claim was never handed over; giving the "
                        "address up\n", ifname);
        /* Who holds the claim is asked first, and nothing is touched unless
           the answer is still the parent this was forked from. By now another
           client may have taken the interface over and been given the same
           address, since the server answers the same hardware address with the
           same address; taking that off, or removing that client's claim,
           would leave a running client with neither. With no lease in force
           there is nothing to take off at all, which is what keeps a static
           address set by somebody else out of this. */
        if (claim_pid(0) == parent) {
            /* The claim goes only if the address it names is gone, the same
               order every other cleanup here follows: a claim removed after a
               failed cleanup is an address nothing can find, since the claim
               was the only record of which address it was. Left in place, it
               names a process that has exited, which the next client or the
               next `dhcpc -k` will read and finish. */
            if (holding == 0 || drop_recorded_address(holding) == 0) {
                remove(pidpath);
            } else {
                fprintf(stderr, "dhcpc: %s: the claim is left in place for the next "
                                "client to finish\n", ifname);
            }
        }
        holds_interface = 0;
        _exit(1);
    }

    setsid();
    fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) {
            close(fd);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct dhcp_lease l;
    time_t asked = 0;
    struct sockaddr_ll bind_to;
    int background = 0;
    int stop = 0;
    int backgrounded = 0;
    int fd;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "-b") == 0) {
            background = 1;
        } else if (strcmp(argv[i], "-k") == 0) {
            stop = 1;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            /* Hold no lease longer than this, whatever the server says. A
               server that hands out days makes the renewal path unreachable in
               anything short of days, which is no way to know it works. It can
               only lower the cap: the cap is there because the arithmetic
               above it stops being arithmetic. */
            char *end = 0;
            unsigned long v;

            errno = 0;
            v = strtoul(argv[++i], &end, 10);
            if (errno != 0 || !end || *end != '\0' || argv[i][0] == '-' ||
                v < DHCP_MIN_LEASE || v > HARD_MAX_LEASE) {
                fprintf(stderr, "dhcpc: -l takes %d to %lu seconds\n", DHCP_MIN_LEASE,
                        (unsigned long)HARD_MAX_LEASE);
                return 2;
            }
            max_lease = v;
        } else {
            fprintf(stderr,
                    "usage: dhcpc [-i interface] [-b] [-l seconds]\n"
                    "       dhcpc [-i interface] -k\n");
            return 2;
        }
    }

    if (stop) {
        if (background) {
            fprintf(stderr, "dhcpc: -k stops a client; it takes no other option\n");
            return 2;
        }
        return stop_client();
    }

    /* Before the claim exists, not after: a signal arriving in between would
       otherwise take the default action, which is to terminate, and leave a
       claim behind with no process to answer for it. A signal that arrives
       before there is anything to clean up costs nothing; the loops below see
       the flag and leave. */
    signal(SIGTERM, stop_handler);
    signal(SIGINT, stop_handler);
    signal(SIGHUP, stop_handler);
    /* And a write to a console nobody is reading any more must not kill a
       client that has a lease: the default action for SIGPIPE terminates,
       and a client that printed its lease line into a closed pipe would leave
       the address on the interface with nothing to take it off. */
    signal(SIGPIPE, SIG_IGN);

    if (read_interface() != 0) {
        return 1;
    }
    if (claim_interface() != 0) {
        return 1;
    }

    fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (fd < 0) {
        fprintf(stderr, "dhcpc: cannot open a packet socket: %s\n", strerror(errno));
        release_interface();
        return 1;
    }
    memset(&bind_to, 0, sizeof(bind_to));
    bind_to.sll_family = AF_PACKET;
    bind_to.sll_protocol = htons(ETH_P_IP);
    bind_to.sll_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&bind_to, sizeof(bind_to)) < 0) {
        fprintf(stderr, "dhcpc: cannot bind to %s: %s\n", ifname, strerror(errno));
        close(fd);
        release_interface();
        return 1;
    }

    for (;;) {
        int configured = 0;

        if (obtain(fd, &l, &asked) == 0) {
            if (!claim_is_ours(1)) {
                /* Another client has the interface, or this one could not
                   write its claim. Nothing is configured yet, so there is
                   nothing to undo and nothing to promise. */
                close(fd);
                return 1;
            }
            if (apply(fd, &l, 1) == 0) {
                configured = 1;
            } else {
                /* Half a configuration is worse than none: an address left on
                   an interface with no client behind it is one the server will
                   hand to somebody else. Taken off, and asked for again: the
                   machine has to end up reachable. */
                unconfigure(fd);
            }
        } else if (stopping) {
            close(fd);
            release_interface();
            return 0;
        }

        if (background && !backgrounded) {
            /* Whatever started this is waiting on it, and it has waited long
               enough: either the machine is configured or it is still asking,
               and both are answers. Going to the background here rather than
               only on success is what keeps a machine that found no server
               from holding up its own boot. */
            if (!configured) {
                fprintf(stderr, "dhcpc: no answer yet; asking from the background\n");
            }
            if (go_background() != 0) {
                if (configured) {
                    unconfigure(fd);
                }
                close(fd);
                release_interface();
                return 1;
            }
            backgrounded = 1;
        }
        if (!configured) {
            sleep(REST_SECONDS);
            continue;
        }
        {
            int held = hold(fd, &l, asked);

            if (held == -2) {
                /* The claim is another client's: the address on the interface
                   is now that client's business and not this one's. */
                close(fd);
                return 1;
            }
            if (held != 0) {
                unconfigure(fd);
                close(fd);
                release_interface();
                return 1;
            }
        }
        if (stopping) {
            /* Asked to stop while holding a lease: the address is this
               machine's only for as long as something is renewing it. */
            unconfigure(fd);
            close(fd);
            release_interface();
            return 0;
        }
    }
}
