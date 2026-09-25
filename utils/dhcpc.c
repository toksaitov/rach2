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
 * `dhcpc [-i INTERFACE] [-b] [-l SECONDS]`. It holds the lease for as long as
 * it runs: it renews and rebinds at the times the server sends, or at half and
 * seven eighths of the lease when the server sends none, and when a lease
 * expires it takes the address off and starts again, since the server may
 * give an expired address to another machine. There is no configure-once
 * mode, deliberately: a client that applied a lease and exited would leave an
 * address on the interface with nothing to take it off when the lease ended,
 * and the machine would go on answering for an address the server had since
 * given to another machine.
 *
 * With `-b` it stays in the foreground through its first round of discovery,
 * which ends with a lease or without one, and then goes to the background and
 * holds the lease or goes on asking. A boot that found no server backgrounds
 * too instead of giving up, because a machine that gave up is one nobody can
 * reach until somebody walks to it. `dhcpc -i eth0 -k` stops the client that
 * has an interface and waits for the address to come off, which is what a
 * machine being moved to a fixed address needs first.
 *
 * What it sends through the packet socket it builds whole: the Ethernet
 * payload is an IP header, a UDP header and a BOOTP message, with both
 * checksums computed. A renewal goes through an ordinary socket. A DHCP server
 * answers a machine that has no address by broadcasting, and a machine with no
 * address cannot receive through the IP layer, which is why the client reads
 * its replies from a packet socket.
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/route.h>
#include <netpacket/packet.h>

#include "dhcpmsg.h"
#include "ipcheck.h"

#ifndef ETH_P_IP
#define ETH_P_IP 0x0800
#endif
/* The hardware type BOOTP and ARP give Ethernet. */
#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER 1
#endif

#define RESOLV_CONF "/etc/resolv.conf"
#define RUN_DIR     "/var/run"

/* The discover schedule: four tries, then a rest, forever. A machine plugged
 * into a switch that has not started forwarding must keep asking, because the
 * alternative is a machine nobody can reach until somebody walks to it. */
#define DISCOVER_TRIES 4
#define REST_SECONDS   30

/* How long `-k` waits for the client it signalled to let go. A client wakes
 * from its poll on the signal, so this is a bound on a client that is stuck,
 * not the time it takes. */
#define STOP_WAIT_SECONDS 10

/* The longest single wait, in seconds, so that poll's millisecond count fits
 * an int however far off the deadline is. */
#define POLL_CHUNK 60

static const char *ifname = "eth0";
static int ifindex;
static unsigned char hwaddr[6];
static unsigned long xid;

/* The longest lease this client acts on; dhcp_sane_lease says why it is a
 * week. `-l` may lower it and may not raise it. */
#define HARD_MAX_LEASE (7UL * 24 * 3600)
static unsigned long max_lease = HARD_MAX_LEASE;

/*
 * Set when a signal asks this client to stop.
 *
 * A client that was killed while holding a lease must take the address off on
 * its way out: the kernel's configuration outlives the process, and an address
 * nobody is renewing is one the server hands to somebody else while this
 * machine still answers for it. So the handler only writes this; the loops
 * read it and return, and `main` takes the address off and removes the claim
 * as it does after a failure.
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
 * Choose a transaction id, at random where the machine can.
 *
 * The kernel's random device fails closed unless the CPU seeded it, so a
 * machine that cannot give a random number says so instead of handing one
 * out; the fallback is the clock and the pid, which is weaker and is what a
 * machine without that CPU has. A reply is matched on this id and on the
 * hardware address, so two machines booted together with the same hardware
 * address must not choose the same id, or each takes the other's answers.
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
 * Build one message into `out` from this client's state and return its length.
 *
 * The transaction id and the hardware address are the client's, so only what
 * differs between messages is passed in: the type, the address in hand, the
 * two options a request has, and the destination address.
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
 * Every IPv4 packet the interface receives arrives on this socket, so most of
 * what it reads is somebody else's: `dhcp_frame_ok` and `dhcp_parse` are what
 * make a reply ours.
 *
 * `want` is the message type this is waiting for, and anything else that
 * arrives is dropped, not returned: a segment with two servers on it, or
 * one server behind two relays, answers a discover twice, and a client that
 * treated the second offer as an answer to its request would abandon the
 * exchange it had just started. A refusal is returned whatever is awaited,
 * because it ends the exchange either way. Zero as `want` takes anything.
 *
 * Each poll waits at most `POLL_CHUNK` seconds, and the polls go on until
 * `deadline`. A signal wakes a poll, and once a stop has been asked for this
 * returns 0 at once, so that the caller can take the address off and leave.
 *
 * @return the message type, 0 once `deadline` has passed or a stop has been
 *         asked for, or -1 on a socket that failed. A poll that reports an
 *         error condition without input also returns 0; this kernel's packet
 *         socket reports none.
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
            return 0;
        }
        if (now >= deadline) {
            return 0;
        }
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
        if (n == 0) {
            continue;
        }
        if (!(p.revents & POLLIN)) {
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
    sprintf(buf, "%lu.%lu.%lu.%lu", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff,
            ip & 0xff);
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
 * Replace `/etc/resolv.conf` with the lease's resolver settings.
 *
 * Write and close a private temporary file before renaming it, so readers see
 * a complete version. Replace the file even when the lease supplies no DNS
 * servers. Return 0, or -1 with the old file in place.
 */
static int write_resolv(const struct dhcp_lease *l)
{
    char tmp[64];
    char buf[32];
    FILE *f;
    int fd;
    int i;

    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", RESOLV_CONF);
    fd = mkstemp(tmp);
    if (fd < 0) {
        fprintf(stderr, "dhcpc: resolver: cannot create %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    if (fchmod(fd, 0644) != 0) {
        close(fd);
        remove(tmp);
        return -1;
    }
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        remove(tmp);
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
        fprintf(stderr, "dhcpc: resolver: cannot replace %s: %s\n", RESOLV_CONF, strerror(errno));
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
/* The lease's address was on the interface before this client took the
 * lease, so the claim names nothing for a cleanup to take off. */
static int adopted;

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
    int n = snprintf(text, sizeof(text), "%ld\n%lu.%lu.%lu.%lu\n", pid, (addr >> 24) & 0xff,
                     (addr >> 16) & 0xff, (addr >> 8) & 0xff, addr & 0xff);

    if (n <= 0 || write(fd, text, (size_t)n) != n) {
        return -1;
    }
    return 0;
}

/**
 * Whether a process exists.
 *
 * A signal of zero delivers nothing and reports whether it could have been
 * delivered. `EPERM` says the process exists and belongs to another user, so
 * it counts as alive; any other failure, `ESRCH` in practice, counts as gone.
 */
static int pid_alive(long pid)
{
    if (pid <= 0) {
        return 0;
    }
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

/**
 * Read the claim's pid and optional address.
 *
 * Return zero if no valid pid can be read. Set `present` unless the claim is
 * known to be absent; an unreadable claim remains present. Either output
 * pointer may be null.
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
        *present = f != NULL || errno != ENOENT;
    }
    if (!f) {
        return 0;
    }
    if (fscanf(f, "%ld", &pid) != 1 || pid <= 0) {
        pid = 0;
    } else if (addr && fscanf(f, "%3u.%3u.%3u.%3u", &q[0], &q[1], &q[2], &q[3]) == 4 &&
               q[0] < 256 && q[1] < 256 && q[2] < 256 && q[3] < 256) {
        *addr = ((unsigned long)q[0] << 24) | ((unsigned long)q[1] << 16) |
                ((unsigned long)q[2] << 8) | (unsigned long)q[3];
    }
    if (ferror(f)) {
        pid = 0;
        if (addr) {
            *addr = 0;
        }
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
 * The new claim is written beside the old one and renamed over it, since a
 * rename replaces one name with another in one system call. A claim rewritten
 * in place is empty for a moment, and a client that died then would leave one
 * that the takeover and the stop path both read as busy, with no record of
 * which address to take off.
 *
 * Only a claim that names the calling process is replaced. The parent in
 * `go_background` is the one caller that writes a pid other than its own, and
 * the claim still names the parent when it does. A client that does not hold
 * the interface writes nothing.
 *
 * @return 0 when the claim now says that or this client holds none, -1 when it
 *         could not be replaced, which leaves the old claim in place.
 */
static int record_claim(long pid, unsigned long addr)
{
    char tmp[96];
    int fd;
    int ok;

    if (!holds_interface) {
        return 0;
    }
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
        fprintf(stderr, "dhcpc: %s: no socket to clean up with: %s\n", ifname, strerror(errno));
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
        return 0;
    }
    format_ip(addr, a);
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    set_ifr_addr(&ifr, 0);
    if (ioctl(fd, SIOCSIFADDR, &ifr) == 0) {
        fprintf(stderr, "dhcpc: %s: took off %s, left by a client that is gone\n", ifname, a);
        rc = 0;
    } else {
        fprintf(stderr, "dhcpc: %s: cannot take off %s: %s\n", ifname, a, strerror(errno));
    }
    close(fd);
    return rc;
}

/**
 * Stop the client holding this interface and wait for it to let go.
 *
 * A leased address is renewed by the client that holds it, so an address that
 * is to be replaced by a fixed one, or by none, needs its client stopped
 * first. The signal is TERM, which the client answers by unconfiguring the
 * interface and dropping its claim, and this waits for the claim to go so
 * that whatever runs next does not race it. A claim that changes hands during
 * the wait is a client going to the background, whose parent published its
 * child and exited, and the signal is sent again to the process the claim
 * names now.
 *
 * A claim naming a process that is gone was left by a client killed outright.
 * The address it records is taken off before the claim is removed, since the
 * claim is the only record of which address that is. A new client may take
 * such a claim between the reading and the removing, so it is read again just
 * before it goes, and a claim that changed is read afresh, not removed; this
 * happens twice at most.
 *
 * @return 0 when a client was stopped or a dead client's claim was removed, 1
 *         otherwise, including when no client held the interface.
 */
static int stop_client(void)
{
    unsigned long left = 0;
    int present = 0;
    long pid = 0;
    int attempt;
    int i;

    snprintf(pidpath, sizeof(pidpath), "%s/dhcpc.%s.pid", RUN_DIR, ifname);
    for (attempt = 0; attempt < 2; attempt++) {
        pid = claim_pid(&present);
        if (pid <= 0) {
            if (present) {
                fprintf(stderr, "dhcpc: %s: the claim names no process; leaving it alone\n",
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
            continue;
        }
        if (drop_recorded_address(left) != 0) {
            fprintf(stderr, "dhcpc: %s: the claim of process %ld is left in place\n", ifname, pid);
            return 1;
        }
        if (remove(pidpath) != 0) {
            fprintf(stderr, "dhcpc: %s: cannot remove the claim of process %ld: %s\n", ifname, pid,
                    strerror(errno));
            return 1;
        }
        printf("dhcpc: %s: process %ld was gone; its claim is removed\n", ifname, pid);
        fflush(stdout);
        return 0;
    }
    if (!pid_alive(pid)) {
        fprintf(stderr,
                "dhcpc: %s: the claim keeps changing hands; nothing was "
                "stopped\n",
                ifname);
        return 1;
    }
    if (kill((pid_t)pid, SIGTERM) != 0) {
        fprintf(stderr, "dhcpc: %s: cannot stop process %ld: %s\n", ifname, pid, strerror(errno));
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
            pid = now;
            if (kill((pid_t)pid, SIGTERM) != 0 && errno != ESRCH) {
                fprintf(stderr, "dhcpc: %s: cannot stop process %ld: %s\n", ifname, pid,
                        strerror(errno));
                return 1;
            }
        }
    }
    fprintf(stderr, "dhcpc: %s: process %ld has not let go after %d seconds\n", ifname, pid,
            STOP_WAIT_SECONDS);
    return 1;
}

/**
 * Confirm this client still holds the interface, and take the claim back if
 * nothing has it.
 *
 * Two clients started at the same instant over the claim of a client killed
 * outright can both find it dead and both take it; one of them then has a
 * claim the other removed. This is what makes that visible: the claim is read
 * again before the interface is touched, and a client that finds another live
 * client named there leaves the interface to that client and does not remove
 * a claim that is not its own.
 *
 * A claim that is simply gone is a different thing and must not be read as a
 * loss: this client is still the one the interface is configured for, and
 * walking away would leave an address with nobody to take it off when its
 * lease ends. So it is written again.
 *
 * It is a check and not a lock, because this kernel has no lock to offer:
 * `fcntl` locking is granted unconditionally here. One administrator at a time
 * is the contract; this is what keeps a lost race from becoming two clients
 * renewing one address.
 *
 * `strict` is set before the first lease is applied and clear afterwards. It
 * decides what a claim this client cannot establish means: one that is
 * unreadable, which is what another client creating its claim this instant
 * looks like, or one that cannot be written again. Nothing is configured yet
 * at the first call, so such a claim is a reason not to start. Once an address
 * is in force the client goes on holding it: two clients on one interface ask
 * with the same hardware address and are given the same address, while an
 * address nobody renews is one the server hands to another machine. Another
 * client that means to take over is seen at the next renewal, once its claim
 * names it.
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
        fprintf(stderr, "dhcpc: %s: process %ld holds the claim on it now\n", ifname, who);
        holds_interface = 0;
        return 0;
    }
    if (present && who <= 0) {
        fprintf(stderr, "dhcpc: %s: the claim is unreadable; %s\n", ifname,
                strict ? "not configuring the interface" : "holding the lease");
        return !strict;
    }
    if (present) {
        remove(pidpath);
    }
    fd = open(pidpath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        int ok = write_claim(fd, (long)getpid(), adopted ? 0 : holding) == 0;

        close(fd);
        if (ok) {
            fprintf(stderr, "dhcpc: %s: the claim had gone; it is written again\n", ifname);
            return 1;
        }
        remove(pidpath);
        fprintf(stderr, "dhcpc: %s: the claim had gone and cannot be written again: %s\n", ifname,
                strerror(errno));
        return !strict;
    }
    who = claim_pid(0);
    if (who > 0 && who != (long)getpid() && pid_alive(who)) {
        fprintf(stderr, "dhcpc: %s: process %ld holds the claim on it now\n", ifname, who);
        holds_interface = 0;
        return 0;
    }
    return !strict;
}

/**
 * Claim the interface, or report who has it.
 *
 * Two clients on one interface interfere: each renews, either may be refused,
 * and either one's expiry takes off an address the other still holds. The
 * claim is the creation of the file, which either happens or does not, so two
 * clients starting at the same moment cannot both create it. The file is
 * visible before its pid is written. An empty or unreadable claim is treated
 * as busy; closing the descriptor does not publish its contents atomically.
 *
 * A claim naming a process that is not there is taken over once. It is read
 * again immediately before it is removed, since another client may have taken
 * it in between; that narrows the window without closing it, and
 * `claim_is_ours` makes the loss visible when it happens. The address it
 * records is taken off first, since the claim is the only record of which
 * address that is. Anything else that goes wrong is a refusal: a client that
 * cannot say it has an interface must not act as though it does.
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
            fprintf(stderr, "dhcpc: %s: another client is claiming it\n", ifname);
            return -1;
        }
        if (pid_alive(other)) {
            fprintf(stderr, "dhcpc: %s: process %ld is already holding it\n", ifname, other);
            return -1;
        }
        if (claim_read(&left, 0) != other) {
            continue;
        }
        if (drop_recorded_address(left) != 0) {
            fprintf(stderr, "dhcpc: %s: the claim of process %ld is left in place\n", ifname,
                    other);
            return -1;
        }
        remove(pidpath);
    }
    fprintf(stderr, "dhcpc: %s: cannot claim it\n", ifname);
    return -1;
}

/** Release the claim only after its recorded address has been removed. */
static void release_interface(void)
{
    if (holds_interface && holding == 0) {
        remove(pidpath);
        holds_interface = 0;
    }
}

/** Whether the interface has `addr` now; 0 when it cannot be read. */
static int iface_has(int fd, unsigned long addr)
{
    struct ifreq ifr;
    struct sockaddr_in *sin;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
        return 0;
    }
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    return ntohl(sin->sin_addr.s_addr) == addr;
}

/**
 * Put the lease on the interface: the address, the mask, the route, the
 * resolver.
 *
 * The address is recorded in the claim before it is applied: a client that
 * dies in between leaves a claim naming an address the interface does not
 * have, and the cleanup compares before it removes, so nothing is taken off
 * that this client never put on. An address the interface had before the
 * first lease, a fixed one the offer happens to repeat, is recorded as none,
 * since the comparison could not tell it from this client's; it stays so
 * until a renewal brings another address. `strict` is set for the first
 * lease and clear for a renewal. A first lease is not applied at all if the
 * record cannot be written, since nothing would then know what to take off;
 * a renewal goes on, because the address is already there and stopping
 * would leave it with nobody at all.
 *
 * The address goes on before the mask, which is the order the two ioctls are
 * meant to be issued in, and the route after both, because a gateway must be
 * on a subnet this machine already has. The old default route is deleted
 * whether or not the lease names a router: a gateway from the network this
 * machine was on before is not one here.
 *
 * @return 0, or -1 with the interface perhaps half configured, which the
 *         caller undoes with `unconfigure`.
 */
static int apply(int fd, const struct dhcp_lease *l, int strict)
{
    struct ifreq ifr;
    struct rtentry rt;
    struct sockaddr_in *sin;
    char a[16], b[16];
    unsigned long mask = ipcheck_mask(l->address, l->netmask);

    if (strict) {
        adopted = iface_has(fd, l->address);
    } else if (l->address != holding) {
        adopted = 0;
    }
    if (record_claim((long)getpid(), adopted ? 0 : l->address) != 0) {
        fprintf(stderr, "dhcpc: %s: cannot record the address in the claim: %s\n", ifname,
                strerror(errno));
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
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    set_ifr_addr(&ifr, mask);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
        fprintf(stderr, "dhcpc: cannot set the netmask: %s\n", strerror(errno));
        return -1;
    }

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
    format_ip(mask, b);
    printf("dhcpc: %s: lease of %s netmask %s for %lu seconds\n", ifname, a, b, l->seconds);
    fflush(stdout);
    return 0;
}

/**
 * Remove the interface address through the existing socket.
 *
 * Preserve the recorded address and claim if removal fails so a later client
 * can retry cleanup. Return zero on removal, or -1 on failure.
 */
static int unconfigure(int fd)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    set_ifr_addr(&ifr, 0);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        fprintf(stderr, "dhcpc: cannot take the address off: %s\n", strerror(errno));
        return -1;
    }
    holding = 0;
    (void)record_claim((long)getpid(), 0);
    return 0;
}

/**
 * Apply the first lease or remove any partial configuration.
 *
 * Return 1 when configured, zero when discovery may retry, or -1 when the
 * claim is lost or cleanup fails. Failed cleanup preserves the recovery record.
 * A first lease whose claim could not be recorded was never applied, and
 * `holding` is still zero: nothing is taken off then, so an address the
 * interface already had, such as a static one from the boot line, stays.
 */
static int apply_initial(int fd, const struct dhcp_lease *l)
{
    if (!claim_is_ours(1)) {
        return -1;
    }
    if (apply(fd, l, 1) == 0) {
        return 1;
    }
    if (holding == 0) {
        return 0;
    }
    return unconfigure(fd) == 0 ? 0 : -1;
}

/**
 * Ask for a lease and return it in `l` for the caller to apply.
 *
 * Discover, then request the address the first offer names, and wait for the
 * acknowledgement; each try waits twice as long as the one before, from two
 * seconds. `*asked` is set as the request goes out: the server starts counting
 * when it grants, and an answer that took ten seconds to arrive has spent ten
 * seconds of the lease.
 *
 * A refusal starts again, and so does an acknowledgement this client cannot
 * use. One without a lease time `dhcp_sane_lease` accepts would have the
 * machine keep an address until somebody else is given it; one that
 * `dhcp_lease_usable` refuses would change the address and then be refused
 * half way, leaving the machine with neither. A server that keeps answering so
 * is asked again, not given up on. Silence after the schedule runs out
 * returns, and the caller decides whether to try another round.
 *
 * @return 0 with `l` filled, or -1 when no usable lease came, a send failed,
 *         or a stop was asked for.
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
        *asked = time(0);
        if (transmit(fd, bcast, len) != 0) {
            return -1;
        }
        type = receive(fd, l, DHCP_ACK, time(0) + wait);
        if (type == DHCP_ACK) {
            l->seconds = l->has_seconds ? dhcp_sane_lease(l->seconds, max_lease) : 0;
        }
        if (type == DHCP_ACK && l->seconds == 0) {
            fprintf(stderr, "dhcpc: the server acknowledged without a lease time this "
                            "client can hold the address for\n");
            continue;
        }
        if (type == DHCP_ACK && l->address != 0) {
            char why[96];

            if (dhcp_lease_usable(l, why, sizeof(why)) != 0) {
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
 * connected datagram socket receives only from its peer.
 *
 * The message is the BOOTP part alone; the kernel writes the IP and UDP headers
 * and finds the route. The wait ends at `limit`, the boundary that decides
 * what to do next, or after ten seconds if that is sooner.
 *
 * @return the message type, 0 when no reply for this client came, or -1 on a
 *         socket that failed.
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
 * Wait two seconds before asking again, or until `limit` if that is sooner.
 *
 * Every path in `hold` that did not get an answer it can use comes through
 * here, so a server that answers wrongly at once is asked again two seconds
 * later, not at once, unless `limit` comes first.
 */
static void rest_until(time_t limit)
{
    time_t left = limit - time(0);

    if (left > 0) {
        sleep(left < 2 ? (unsigned int)left : 2);
    }
}

/**
 * Hold a lease: renew at T1, rebind at T2, and give the address up when the
 * lease runs out.
 *
 * Until T1 the client waits on the packet socket, where a stop request wakes
 * it. A renewal goes to the server that granted the lease and nowhere else; a
 * rebinding goes to everyone through the packet socket, because by then the
 * server that granted it has stopped answering and there may be another. Both
 * ask for the same thing. Every wait is cut short by the next boundary, so a
 * renewal that goes unanswered cannot keep the machine past the moment it
 * should have started rebinding, or past the moment the address stops being
 * its own. A renewal waits ten seconds for its answer, and a renewal whose
 * socket fails counts as unanswered. A rebinding request waits for half of
 * what is left of the lease, as RFC 2131 asks, but at least a second and not
 * the RFC's sixty, so that even the shortest lease this client accepts, ten
 * seconds, is asked about more than once; waiting the whole way to the expiry
 * would let one lost request cost the last eighth of the lease.
 *
 * An acknowledgement of the same address moves the boundaries, counted from
 * the moment this machine asked, since the server started counting first. It
 * is applied again and logged as `renewed for N seconds`, a line the dhcp
 * suite counts apart from a new lease's. One that `dhcp_lease_usable`
 * refuses, or that has no lease time `dhcp_sane_lease` accepts, leaves the
 * lease in force and its boundaries alone: moving them on an answer that gave
 * no duration would let a malformed server extend an address forever while it
 * hands that address to another machine. An acknowledgement of another
 * address counts as no answer. A refusal, a rebinding request that cannot be
 * sent, or a renewal that `apply` fails on gives the address up as an expiry
 * does, and the caller asks for a new lease.
 *
 * @return 0 when the address was given up or a stop was asked for, -1 on a
 *         failed socket or an address that could not be taken off, or -2 when
 *         another client has the claim.
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
            return unconfigure(fd);
        }
        if (now < t1) {
            if (receive(fd, &got, DHCP_ACK, t1 < end ? t1 : end) < 0) {
                return -1;
            }
            continue;
        }

        xid = random_xid();
        if (now < t2) {
            limit = t2 < end ? t2 : end;
            len = build(DHCP_REQUEST, l->address, 0, 0, l->server);
            type = renew_unicast(l, &got, len - 28, limit);
            if (type < 0) {
                type = 0;
            }
        } else {
            limit = end;
            len = build(DHCP_REQUEST, l->address, 0, 0, 0xFFFFFFFFUL);
            if (transmit(fd, bcast, len) != 0) {
                return unconfigure(fd);
            }
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
            {
                char why[96];

                if (dhcp_lease_usable(&got, why, sizeof(why)) != 0) {
                    fprintf(stderr,
                            "dhcpc: the renewal cannot be used: %s; the lease "
                            "in force is unchanged\n",
                            why);
                    rest_until(limit);
                    continue;
                }
            }
            got.seconds = got.has_seconds ? dhcp_sane_lease(got.seconds, max_lease) : 0;
            if (got.seconds == 0) {
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
                fprintf(stderr, "dhcpc: the renewal could not be applied; asking for an "
                                "address again\n");
                return unconfigure(fd);
            }
            printf("dhcpc: %s: renewed for %lu seconds\n", ifname, l->seconds);
            fflush(stdout);
            start = now;
            dhcp_boundaries(l, start, &t1, &t2, &end);
            continue;
        }
        if (type == DHCP_NAK) {
            fprintf(stderr, "dhcpc: the server refused the renewal; giving the address up\n");
            return unconfigure(fd);
        }
        rest_until(limit);
    }
}

/**
 * Replace inherited standard descriptors with the console or `/dev/null`.
 *
 * Close the inherited descriptors before opening the replacement. On failure,
 * leave all three closed and return -1.
 */
static int detach_stdio(void)
{
    int fd;
    int i;

    for (i = 0; i < 3; i++) {
        close(i);
    }
    fd = open("/dev/console", O_RDWR);
    if (fd < 0) {
        fd = open("/dev/null", O_RDWR);
    }
    if (fd < 0) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        if (dup2(fd, i) < 0) {
            int j;

            for (j = 0; j < 3; j++) {
                close(j);
            }
            if (fd > 2) {
                close(fd);
            }
            return -1;
        }
    }
    if (fd > 2) {
        close(fd);
    }
    return 0;
}

/**
 * Continue in a child after the parent publishes its pid in the claim.
 *
 * The parent writes the child's pid into the claim, tells the child through a
 * pipe, and then exits with the handoff status, so a claim that was handed
 * over never names a process that has exited. A stop that reached the parent
 * during the handoff is passed on to the child. A parent that cannot publish
 * the child leaves the claim naming itself: that is how the child knows the
 * address is still its own to take off, and a configured address with no
 * claim is one nothing could find.
 *
 * The child does nothing until it has the answer. Until the claim names it, a
 * `dhcpc -k` would read the parent's pid, and if the parent had died it would
 * take the address off, remove the claim and report a client stopped that is
 * still running. A refusal, an end of file or a failed read makes the child
 * give up and exit with status 1. It touches nothing unless the claim still
 * names the parent, since by then another client may have taken the interface
 * over and been given the same address. When the claim does name the parent,
 * the child takes off the address in `holding`, if the interface still has
 * it and the first lease did not find it there, and removes the claim, which
 * it keeps when that address cannot be taken off.
 *
 * A published child starts its own session and detaches the inherited
 * standard descriptors; one that cannot redirect exits with its claim retained
 * for cleanup.
 *
 * @return 0 in the child, or -1 if pipe creation or fork fails. The parent
 *         and a child that gives up exit instead of returning.
 */
static int go_background(void)
{
    long parent = (long)getpid();
    pid_t pid;
    int handshake[2];
    char answer = 0;

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
        close(handshake[0]);
        if (record_claim((long)pid, adopted ? 0 : holding) != 0) {
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
        fprintf(stderr,
                "dhcpc: %s: the claim was never handed over; giving the "
                "address up\n",
                ifname);
        if (claim_pid(0) == parent) {
            if (holding == 0 || adopted || drop_recorded_address(holding) == 0) {
                remove(pidpath);
            } else {
                fprintf(stderr,
                        "dhcpc: %s: the claim is left in place for the next "
                        "client to finish\n",
                        ifname);
            }
        }
        holds_interface = 0;
        _exit(1);
    }

    setsid();
    if (detach_stdio() != 0) {
        _exit(1);
    }
    return 0;
}

/**
 * Claim the interface, then ask for a lease and hold it until stopped.
 *
 * `-k` stops the client that holds the interface and does nothing else. `-l`
 * lowers the cap on every lease, which is how the renewal path is exercised
 * on a network that hands out days; it cannot raise the cap. With `-b` the
 * client goes to the background after its first round of discovery,
 * configured or not, so that a machine that found no server does not hold up
 * its own boot. The foreground process then exits 0 once the claim names the
 * background client, and 1 if the handoff failed; `netup` reads that status.
 *
 * The stop signals are caught before the claim is made, so that one arriving
 * later cannot terminate the client and leave a claim with no process to
 * answer for it. SIGPIPE is ignored, so that a lease line written into a pipe
 * nobody reads cannot kill a client and leave its address on the interface. A
 * client that finds the claim is another's exits and leaves the address to
 * that client.
 *
 * @return 0 after a stop request that took the address off, 1 on a failure, 2
 *         on a usage error; with `-k`, what `stop_client` returns.
 */
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
            char *end = 0;
            unsigned long v;

            errno = 0;
            v = strtoul(argv[++i], &end, 10);
            if (errno != 0 || !end || *end != '\0' || argv[i][0] == '-' || v < DHCP_MIN_LEASE ||
                v > HARD_MAX_LEASE) {
                fprintf(stderr, "dhcpc: -l takes %d to %lu seconds\n", DHCP_MIN_LEASE,
                        (unsigned long)HARD_MAX_LEASE);
                return 2;
            }
            max_lease = v;
        } else {
            fprintf(stderr, "usage: dhcpc [-i interface] [-b] [-l seconds]\n"
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

    signal(SIGTERM, stop_handler);
    signal(SIGINT, stop_handler);
    signal(SIGHUP, stop_handler);
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
            configured = apply_initial(fd, &l);
            if (configured < 0) {
                close(fd);
                return 1;
            }
        } else if (stopping) {
            close(fd);
            release_interface();
            return 0;
        }

        if (background && !backgrounded) {
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
            int status = unconfigure(fd);

            close(fd);
            release_interface();
            return status != 0;
        }
    }
}
