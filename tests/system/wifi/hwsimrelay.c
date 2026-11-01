/* Bridge Linux mac80211_hwsim's netlink medium to version 1 WSIM UDP.
 * The hwsim ABI is from Linux v6.18 mac80211_hwsim.h; no Linux code is used. */
#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../../rach2/sys/network/wireless/relay.h"

#define MAX_RADIOS       32
#define MAX_PENDING      256
#define MAX_INJECT       (MAX_PENDING * MAX_RADIOS)
#define MSG_SIZE         8192
#define TX_LIFETIME      1000 /* milliseconds before a lost medium completion fails */
#define H_REGISTER       1
#define H_FRAME          2
#define H_TX_INFO        3
#define A_RECEIVER       1
#define A_TRANSMITTER    2
#define A_FRAME          3
#define A_FLAGS          4
#define A_RX_RATE        5
#define A_SIGNAL         6
#define A_TX_INFO        7
#define A_COOKIE         8
#define A_FREQ           19
#define A_RATE_FLAGS     21
#define H_NO_ACK         2
#define H_ACKED          4
#define R_SHORT_PREAMBLE 4
#define R_MCS            8
#define R_SHORT_GI       128
#define W_TX             WLAN_RELAY_F_TX
#define W_ACK            WLAN_RELAY_F_ACKED
#define W_NOACK          WLAN_RELAY_F_NO_ACK
#define W_FAIL           WLAN_RELAY_F_FAILED
#define W_MCS            WLAN_RELAY_F_MCS
#define W_GI             WLAN_RELAY_F_SHORT_GI
#define W_PRE            WLAN_RELAY_F_SHORT_PREAMBLE
#define W_ALL            (W_TX | W_ACK | W_NOACK | W_FAIL | W_MCS | W_GI | W_PRE)

struct radio {
    unsigned id; /* endpoint-local phy number, never a kernel pointer */
    char name[32];
    unsigned char medium[6]; /* second address in the wiphy's addresses file */
};
struct packet {
    unsigned type, radio, flags, freq, rate, len, attempts;
    int signal;
    uint64_t cookie;
    unsigned char frame[WLAN_RELAY_MAX_FRAME_LEN];
};
struct pending {
    int used, remote, source; /* remote: peer transmit, otherwise local hwsim */
    struct packet packet;
    uint64_t kernel_cookie;
    unsigned char rates[8], rate_flags[12]; /* four packed hwsim rate records */
    unsigned kernel_flags;
    unsigned injects; /* netlink receive acknowledgements still outstanding */
    int acked, peer_done;
    uint64_t expires; /* local monotonic milliseconds */
};
struct injection {
    uint32_t seq;
    int pending, addressed; /* -1 pending means a receive with no completion */
};
struct completed {
    int used;
    struct packet packet; /* last peer completion, for duplicate requests */
};
struct message {
    union {
        struct nlmsghdr align;
        unsigned char bytes[MSG_SIZE];
    } data;
    size_t len;
};
static struct radio radios[MAX_RADIOS];
static unsigned nradios;
static struct pending pending[MAX_PENDING];
static struct injection injections[MAX_INJECT];
static struct completed completed[MAX_PENDING];
static unsigned completed_next;
static int nl = -1, udp = -1, family;
static uint32_t sequence;
static uint64_t next_cookie;
static volatile sig_atomic_t stopped;
static const unsigned legacy[] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};

/** Record a stop request; the main loop drains completions before closing. */
static void stop_handler(int signo)
{
    (void)signo;
    stopped = 1;
}

/** Return monotonic milliseconds; exit if the clock cannot be read. */
static uint64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) {
        perror("hwsimrelay: clock");
        exit(1);
    }
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/** Read an unaligned big-endian integer of n bytes, where n is at most eight. */
static uint64_t getbe(const unsigned char *p, unsigned n)
{
    uint64_t v = 0;
    while (n--)
        v = (v << 8) | *p++;
    return v;
}

/** Store the low n bytes of v in big-endian order, where n is at most eight. */
static void putbe(unsigned char *p, unsigned n, uint64_t v)
{
    while (n) {
        p[--n] = (unsigned char)v;
        v >>= 8;
    }
}

/** Return the legacy rate index, or -1 for a rate outside the 2.4 GHz band. */
static int rate_index(unsigned rate)
{
    unsigned i;
    for (i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++)
        if (legacy[i] == rate)
            return (int)i;
    return -1;
}

/** Validate one complete wire record and copy it to p; return -1 on refusal. */
static int decode(struct packet *p, const unsigned char *b, size_t n)
{
    unsigned outcome;
    if (n < WLAN_RELAY_HEADER_LEN || getbe(b, 4) != WLAN_RELAY_MAGIC ||
        b[4] != WLAN_RELAY_VERSION || getbe(b + 6, 2) != WLAN_RELAY_HEADER_LEN || b[37] || b[38] ||
        b[39])
        return -1;
    memset(p, 0, sizeof(*p));
    p->type = b[5];
    p->radio = (unsigned)getbe(b + 8, 2);
    p->flags = (unsigned)getbe(b + 10, 2);
    p->freq = (unsigned)getbe(b + 12, 2);
    p->rate = (unsigned)getbe(b + 14, 2);
    p->signal = (int)getbe(b + 16, 2);
    if (p->signal >= 32768)
        p->signal -= 65536;
    p->len = (unsigned)getbe(b + 18, 2);
    p->cookie = getbe(b + 20, 8);
    p->attempts = b[36];
    if (p->flags & ~W_ALL || p->len > sizeof(p->frame) || n != WLAN_RELAY_HEADER_LEN + p->len ||
        (p->freq != 2484 && (p->freq < 2412 || p->freq > 2472 || (p->freq - 2412) % 5)) ||
        ((p->flags & W_MCS) ? p->rate > 15 : rate_index(p->rate) < 0) ||
        ((p->flags & W_GI) && !(p->flags & W_MCS)) || ((p->flags & W_PRE) && (p->flags & W_MCS)))
        return -1;
    if (p->type == WLAN_RELAY_FRAME) {
        if (p->len < 10 || p->flags & (W_ACK | W_FAIL))
            return -1;
        if (p->flags & W_TX) {
            if (!p->cookie)
                return -1;
        } else if (p->cookie || p->attempts || p->flags & W_NOACK) {
            return -1;
        }
        memcpy(p->frame, b + WLAN_RELAY_HEADER_LEN, p->len);
    } else if (p->type == WLAN_RELAY_TX_STATUS) {
        outcome = p->flags & (W_ACK | W_NOACK | W_FAIL);
        if (p->len || !(p->flags & W_TX) || !p->cookie ||
            (outcome != W_ACK && outcome != W_NOACK && outcome != W_FAIL))
            return -1;
    } else {
        return -1;
    }
    return 0;
}

/** Send one wire record to the configured peer; return -1 on transport error. */
static int send_packet(const struct packet *p)
{
    unsigned char b[WLAN_RELAY_HEADER_LEN + WLAN_RELAY_MAX_FRAME_LEN];
    size_t n = WLAN_RELAY_HEADER_LEN + p->len;
    memset(b, 0, WLAN_RELAY_HEADER_LEN);
    putbe(b, 4, WLAN_RELAY_MAGIC);
    b[4] = WLAN_RELAY_VERSION;
    b[5] = (unsigned char)p->type;
    putbe(b + 6, 2, WLAN_RELAY_HEADER_LEN);
    putbe(b + 8, 2, p->radio);
    putbe(b + 10, 2, p->flags);
    putbe(b + 12, 2, p->freq);
    putbe(b + 14, 2, p->rate);
    putbe(b + 16, 2, (unsigned)p->signal);
    putbe(b + 18, 2, p->len);
    putbe(b + 20, 8, p->cookie);
    b[36] = (unsigned char)p->attempts;
    memcpy(b + WLAN_RELAY_HEADER_LEN, p->frame, p->len);
    return send(udp, b, n, 0) == (ssize_t)n ? 0 : -1;
}

/** Begin a generic-netlink request; return its nonzero sequence number. */
static uint32_t begin(struct message *m, unsigned type, unsigned cmd, unsigned flags)
{
    struct nlmsghdr *h;
    struct genlmsghdr *g;
    memset(m, 0, sizeof(*m));
    h = (struct nlmsghdr *)m->data.bytes;
    g = (struct genlmsghdr *)(m->data.bytes + NLMSG_HDRLEN);
    m->len = NLMSG_HDRLEN + GENL_HDRLEN;
    h->nlmsg_type = (unsigned short)type;
    h->nlmsg_flags = (unsigned short)(NLM_F_REQUEST | flags);
    if (!++sequence)
        ++sequence;
    h->nlmsg_seq = sequence;
    g->cmd = (unsigned char)cmd;
    g->version = 1;
    return sequence;
}

/** Append a native-endian netlink attribute; all callers fit MSG_SIZE. */
static void attr(struct message *m, unsigned type, const void *data, size_t n)
{
    struct nlattr a;
    size_t size = NLA_HDRLEN + n, padded = NLA_ALIGN(size);
    if (m->len + padded > sizeof(m->data.bytes))
        abort();
    a.nla_len = (unsigned short)size;
    a.nla_type = (unsigned short)type;
    memcpy(m->data.bytes + m->len, &a, NLA_HDRLEN);
    memcpy(m->data.bytes + m->len + NLA_HDRLEN, data, n);
    memset(m->data.bytes + m->len + size, 0, padded - size);
    m->len += padded;
}

/** Submit a constructed request to the kernel; return -1 on a short send. */
static int submit(struct message *m)
{
    struct sockaddr_nl addr;
    struct nlmsghdr *h = (struct nlmsghdr *)m->data.bytes;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    h->nlmsg_len = (uint32_t)m->len;
    return sendto(nl, m->data.bytes, m->len, 0, (struct sockaddr *)&addr, sizeof(addr)) ==
                   (ssize_t)m->len
               ? 0
               : -1;
}

/** Parse bounded attributes into borrowed pointers; reject malformed duplicates. */
static int attributes(const unsigned char *b, size_t n, const unsigned char **a, size_t *sizes,
                      unsigned max)
{
    struct nlattr h;
    unsigned type;
    size_t step;
    memset(a, 0, (max + 1) * sizeof(*a));
    memset(sizes, 0, (max + 1) * sizeof(*sizes));
    while (n) {
        if (n < NLA_HDRLEN)
            return -1;
        memcpy(&h, b, NLA_HDRLEN);
        if (h.nla_len < NLA_HDRLEN || h.nla_len > n)
            return -1;
        step = NLA_ALIGN(h.nla_len);
        if (step > n)
            return -1;
        type = h.nla_type & NLA_TYPE_MASK;
        if (type && type <= max) {
            if (a[type])
                return -1;
            a[type] = b + NLA_HDRLEN;
            sizes[type] = h.nla_len - NLA_HDRLEN;
        }
        b += step;
        n -= step;
    }
    return 0;
}

/** Read a netlink datagram from the kernel, refusing loss or a foreign sender. */
static ssize_t receive_nl(unsigned char *b, size_t size)
{
    struct sockaddr_nl from;
    struct iovec iov;
    struct msghdr msg;
    ssize_t n;
    memset(&msg, 0, sizeof(msg));
    memset(&from, 0, sizeof(from));
    iov.iov_base = b;
    iov.iov_len = size;
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    n = recvmsg(nl, &msg, 0);
    if (n >= 0 && (msg.msg_flags & MSG_TRUNC || from.nl_pid)) {
        errno = EMSGSIZE;
        return -1;
    }
    return n;
}

/** Resolve MAC80211_HWSIM before registering for asynchronous frame traffic. */
static int resolve(void)
{
    struct message m;
    unsigned char b[MSG_SIZE];
    const unsigned char *a[CTRL_ATTR_MAX + 1];
    size_t sizes[CTRL_ATTR_MAX + 1];
    struct nlmsghdr h;
    uint32_t seq;
    ssize_t n;
    size_t off;
    uint16_t id;
    seq = begin(&m, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0);
    attr(&m, CTRL_ATTR_FAMILY_NAME, "MAC80211_HWSIM", sizeof("MAC80211_HWSIM"));
    if (submit(&m) < 0)
        return -1;
    while (!stopped) {
        n = receive_nl(b, sizeof(b));
        if (n < 0)
            return -1;
        for (off = 0; off + NLMSG_HDRLEN <= (size_t)n; off += NLMSG_ALIGN(h.nlmsg_len)) {
            memcpy(&h, b + off, NLMSG_HDRLEN);
            if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > (size_t)n - off)
                return -1;
            if (h.nlmsg_seq != seq)
                continue;
            if (h.nlmsg_type == NLMSG_ERROR)
                return -1;
            if (h.nlmsg_type != GENL_ID_CTRL || h.nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN)
                continue;
            if (attributes(b + off + NLMSG_HDRLEN + GENL_HDRLEN,
                           h.nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN, a, sizes, CTRL_ATTR_MAX) < 0 ||
                sizes[CTRL_ATTR_FAMILY_ID] != 2)
                return -1;
            memcpy(&id, a[CTRL_ATTR_FAMILY_ID], 2);
            return id;
        }
    }
    return -1;
}

/** Read a six-byte colon-separated MAC address; return -1 on bad input. */
static int read_mac(FILE *f, unsigned char *mac)
{
    unsigned v[6], i;
    if (fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (i = 0; i < 6; i++) {
        if (v[i] > 255)
            return -1;
        mac[i] = (unsigned char)v[i];
    }
    return 0;
}

/** Discover hwsim radios and their medium addresses from Linux's wiphy sysfs. */
static int discover(void)
{
    DIR *dir;
    struct dirent *e;
    char path[256], driver[256], *end;
    FILE *f;
    unsigned long id;
    ssize_t n;
    unsigned char mac[6];
    dir = opendir("/sys/class/ieee80211");
    if (!dir)
        return -1;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "phy", 3))
            continue;
        id = strtoul(e->d_name + 3, &end, 10);
        if (!e->d_name[3] || *end || id > 65535 || strlen(e->d_name) >= sizeof(radios[0].name))
            continue;
        snprintf(path, sizeof(path), "/sys/class/ieee80211/%s/device/driver", e->d_name);
        n = readlink(path, driver, sizeof(driver) - 1);
        if (n < 0)
            continue;
        driver[n] = 0;
        if (!strstr(driver, "mac80211_hwsim"))
            continue;
        if (nradios == MAX_RADIOS) {
            closedir(dir);
            return -1;
        }
        snprintf(path, sizeof(path), "/sys/class/ieee80211/%s/addresses", e->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (read_mac(f, mac) == 0 && read_mac(f, radios[nradios].medium) == 0) {
            radios[nradios].id = (unsigned)id;
            strcpy(radios[nradios].name, e->d_name);
            nradios++;
        }
        fclose(f);
    }
    closedir(dir);
    return nradios ? 0 : -1;
}

/** Match a unicast receiver against the radio's current virtual interfaces. */
static int addressed(unsigned r, const unsigned char *mac)
{
    DIR *d;
    struct dirent *e;
    char path[512], target[512], *name;
    FILE *f;
    unsigned char address[6];
    ssize_t n;
    int found = 0;
    if (mac[0] & 1)
        return 0;
    d = opendir("/sys/class/net");
    if (!d)
        return 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", e->d_name);
        n = readlink(path, target, sizeof(target) - 1);
        if (n < 0)
            continue;
        target[n] = 0;
        name = strrchr(target, '/');
        if (strcmp(name ? name + 1 : target, radios[r].name))
            continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", e->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        found = read_mac(f, address) == 0 && !memcmp(address, mac, 6);
        fclose(f);
    }
    closedir(d);
    return found;
}

/** Return the slot of a discovered radio's hwsim medium address, or -1. */
static int source_radio(const unsigned char *address)
{
    unsigned i;
    for (i = 0; i < nradios; i++)
        if (!memcmp(address, radios[i].medium, 6))
            return (int)i;
    return -1;
}

/** Complete a local hwsim transmission, preserving its cookie and rate flags. */
static int local_status(struct pending *p)
{
    struct message m;
    uint32_t flags = p->kernel_flags & ~H_ACKED;
    int32_t signal = -30;
    unsigned char rates[8];
    unsigned i;
    memcpy(rates, p->rates, sizeof(rates));
    rates[1] = 1;
    for (i = 1; i < 4; i++) {
        rates[i * 2] = 255;
        rates[i * 2 + 1] = 0;
    }
    if (p->acked && !(flags & H_NO_ACK))
        flags |= H_ACKED;
    begin(&m, family, H_TX_INFO, 0);
    attr(&m, A_TRANSMITTER, radios[p->source].medium, 6);
    attr(&m, A_FLAGS, &flags, sizeof(flags));
    attr(&m, A_SIGNAL, &signal, sizeof(signal));
    attr(&m, A_COOKIE, &p->kernel_cookie, sizeof(p->kernel_cookie));
    attr(&m, A_TX_INFO, rates, sizeof(rates));
    attr(&m, A_RATE_FLAGS, p->rate_flags, sizeof(p->rate_flags));
    return submit(&m);
}

/** Retire a transaction once injections and the peer have reported their results. */
static int finish(unsigned slot)
{
    struct pending *p = &pending[slot];
    struct packet out;
    if (!p->used || p->injects || !p->peer_done)
        return 0;
    if (p->remote) {
        out = p->packet;
        out.type = WLAN_RELAY_TX_STATUS;
        out.len = 0;
        out.attempts = 1;
        out.flags &= W_MCS | W_GI | W_PRE;
        out.flags |= W_TX | ((p->packet.flags & W_NOACK) ? W_NOACK : p->acked ? W_ACK : W_FAIL);
        completed[completed_next].used = 1;
        completed[completed_next].packet = out;
        completed_next = (completed_next + 1) % MAX_PENDING;
        /* A lost UDP completion is retried only in response to a repeated frame. */
        (void)send_packet(&out);
    } else if (local_status(p) < 0) {
        return -1;
    }
    p->used = 0;
    return 0;
}

/** Allocate a bounded transaction; expiry prevents a dead peer from filling it. */
static int allocate(void)
{
    unsigned i;
    for (i = 0; i < MAX_PENDING; i++)
        if (!pending[i].used) {
            memset(&pending[i], 0, sizeof(pending[i]));
            pending[i].used = 1;
            pending[i].expires = now_ms() + TX_LIFETIME;
            return (int)i;
        }
    return -1;
}

/** Inject a frame into one radio and retain the receive ACK's transaction. */
static int inject(const struct packet *p, unsigned r, int slot)
{
    struct message m;
    uint32_t freq = p->freq, rate;
    int32_t signal = p->signal ? p->signal : -30;
    unsigned i;
    if (p->len < 24)
        return 0;
    for (i = 0; i < MAX_INJECT; i++)
        if (!injections[i].seq)
            break;
    if (i == MAX_INJECT)
        return 0;
    /* hwsim's receive ABI exposes legacy indexes only, even for an HT MPDU. */
    rate = p->flags & W_MCS ? 4 : (uint32_t)rate_index(p->rate);
    injections[i].seq = begin(&m, family, H_FRAME, NLM_F_ACK);
    injections[i].pending = slot;
    injections[i].addressed = addressed(r, p->frame + 4);
    attr(&m, A_RECEIVER, radios[r].medium, 6);
    attr(&m, A_FRAME, p->frame, p->len);
    attr(&m, A_FREQ, &freq, sizeof(freq));
    attr(&m, A_RX_RATE, &rate, sizeof(rate));
    attr(&m, A_SIGNAL, &signal, sizeof(signal));
    if (submit(&m) < 0) {
        injections[i].seq = 0;
        return -1;
    }
    if (slot >= 0)
        pending[slot].injects++;
    return 0;
}

/** Fan a frame out locally, omitting its originating radio when supplied. */
static int distribute(const struct packet *p, int source, int slot)
{
    unsigned r;
    for (r = 0; r < nradios; r++)
        if ((int)r != source && inject(p, r, slot) < 0)
            return -1;
    return 0;
}

/** Translate a kernel transmit request, retaining everything its status needs. */
static int kernel_frame(const unsigned char **a, const size_t *sizes)
{
    struct pending *p, overflow;
    unsigned flags;
    uint16_t rf = 0;
    uint32_t freq;
    int slot, r, index;
    if (sizes[A_TRANSMITTER] != 6 || sizes[A_FRAME] < 10 ||
        sizes[A_FRAME] > WLAN_RELAY_MAX_FRAME_LEN || sizes[A_FLAGS] != 4 || sizes[A_COOKIE] != 8 ||
        sizes[A_FREQ] != 4 || sizes[A_TX_INFO] != 8 ||
        (sizes[A_RATE_FLAGS] && sizes[A_RATE_FLAGS] != 12))
        return -1;
    r = source_radio(a[A_TRANSMITTER]);
    if (r < 0)
        return -1;
    slot = allocate();
    p = slot < 0 ? &overflow : &pending[slot];
    if (slot < 0)
        memset(p, 0, sizeof(*p));
    p->source = r;
    memcpy(&p->kernel_flags, a[A_FLAGS], 4);
    memcpy(&p->kernel_cookie, a[A_COOKIE], 8);
    memcpy(p->rates, a[A_TX_INFO], 8);
    if (sizes[A_RATE_FLAGS])
        memcpy(p->rate_flags, a[A_RATE_FLAGS], 12);
    if (slot < 0)
        return local_status(p);
    memcpy(&freq, a[A_FREQ], 4);
    if (sizes[A_RATE_FLAGS])
        memcpy(&rf, a[A_RATE_FLAGS] + 1, 2);
    flags = W_TX | ((p->kernel_flags & H_NO_ACK) ? W_NOACK : 0);
    if (a[A_FRAME][4] & 1)
        flags |= W_NOACK;
    if (rf & R_MCS)
        flags |= W_MCS;
    if (rf & R_SHORT_GI)
        flags |= W_GI;
    if (rf & R_SHORT_PREAMBLE)
        flags |= W_PRE;
    index = (signed char)p->rates[0];
    p->packet.type = WLAN_RELAY_FRAME;
    p->packet.radio = radios[r].id;
    p->packet.freq = freq;
    p->packet.flags = flags;
    p->packet.rate = rf & R_MCS ? (unsigned)index : index >= 0 && index < 12 ? legacy[index] : 0;
    p->packet.signal = -30;
    p->packet.cookie = ++next_cookie;
    p->packet.len = (unsigned)sizes[A_FRAME];
    memcpy(p->packet.frame, a[A_FRAME], p->packet.len);
    /* Version 1 carries 2.4 GHz HT20 and legacy rates, without wider modes. */
    if (freq < 2412 || freq > 2484 || index < 0 ||
        (rf & ~(R_MCS | R_SHORT_GI | R_SHORT_PREAMBLE | 3)) ||
        ((rf & R_MCS) ? index > 15 : index >= 12)) {
        p->peer_done = 1;
        return finish((unsigned)slot);
    }
    if (distribute(&p->packet, r, slot) < 0)
        return -1;
    if (send_packet(&p->packet) < 0)
        p->peer_done = 1;
    return finish((unsigned)slot);
}

/** Consume an injection's kernel ACK; only an addressed, on-channel RX ACKs TX. */
static int injection_result(uint32_t seq, int error)
{
    unsigned i;
    int slot;
    for (i = 0; i < MAX_INJECT; i++) {
        if (injections[i].seq != seq)
            continue;
        slot = injections[i].pending;
        injections[i].seq = 0;
        if (slot < 0)
            return 0;
        if (!error && injections[i].addressed)
            pending[slot].acked = 1;
        pending[slot].injects--;
        return finish((unsigned)slot);
    }
    return 0;
}

/** Dispatch a complete kernel datagram, including asynchronous injection ACKs. */
static int netlink_input(uint32_t register_seq, int *registered)
{
    unsigned char b[65536];
    struct nlmsghdr h;
    const unsigned char *a[32];
    size_t sizes[32], off;
    ssize_t n;
    int error;
    n = receive_nl(b, sizeof(b));
    if (n < 0)
        return errno == EINTR ? 0 : -1;
    for (off = 0; off + NLMSG_HDRLEN <= (size_t)n; off += NLMSG_ALIGN(h.nlmsg_len)) {
        memcpy(&h, b + off, NLMSG_HDRLEN);
        if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > (size_t)n - off)
            return -1;
        if (h.nlmsg_type == NLMSG_ERROR) {
            if (h.nlmsg_len < NLMSG_HDRLEN + sizeof(struct nlmsgerr))
                return -1;
            memcpy(&error, b + off + NLMSG_HDRLEN, sizeof(error));
            if (h.nlmsg_seq == register_seq) {
                if (error) {
                    fprintf(stderr, "hwsimrelay: registration: %s\n", strerror(-error));
                    return -1;
                }
                if (!*registered) {
                    *registered = 1;
                    printf("hwsimrelay: ready: %u radios\n", nradios);
                }
            } else if (injection_result(h.nlmsg_seq, error) < 0) {
                return -1;
            }
        } else if (h.nlmsg_type == family && h.nlmsg_len >= NLMSG_HDRLEN + GENL_HDRLEN &&
                   b[off + NLMSG_HDRLEN] == H_FRAME) {
            if (attributes(b + off + NLMSG_HDRLEN + GENL_HDRLEN,
                           h.nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN, a, sizes, 31) < 0 ||
                kernel_frame(a, sizes) < 0)
                return -1;
        } else if (h.nlmsg_type == NLMSG_OVERRUN) {
            return -1;
        }
    }
    return off == (size_t)n ? 0 : -1;
}

/** Accept only correlated completions; distribute peer transmits without echoing. */
static int udp_input(void)
{
    unsigned char b[WLAN_RELAY_HEADER_LEN + WLAN_RELAY_MAX_FRAME_LEN + 1];
    struct packet p;
    struct pending *q;
    ssize_t n;
    unsigned i;
    int slot;
    n = recv(udp, b, sizeof(b), 0);
    if (n < 0)
        return 0;
    if (decode(&p, b, (size_t)n) < 0) {
        fprintf(stderr, "hwsimrelay: refused malformed datagram\n");
        return 0;
    }
    if (p.type == WLAN_RELAY_TX_STATUS) {
        for (i = 0; i < MAX_PENDING; i++) {
            q = &pending[i];
            if (!q->used || q->remote || q->packet.cookie != p.cookie ||
                q->packet.radio != p.radio || q->peer_done)
                continue;
            if (p.freq != q->packet.freq || p.rate != q->packet.rate ||
                (p.flags & (W_MCS | W_GI | W_PRE)) != (q->packet.flags & (W_MCS | W_GI | W_PRE)) ||
                ((p.flags & W_ACK) && (q->packet.flags & W_NOACK)) ||
                ((p.flags & W_NOACK) && !(q->packet.flags & W_NOACK)))
                return 0;
            q->peer_done = 1;
            if (p.flags & W_ACK)
                q->acked = 1;
            return finish(i);
        }
        return 0;
    }
    if (!(p.flags & W_TX)) {
        for (i = 0; i < nradios; i++)
            if (radios[i].id == p.radio)
                return inject(&p, i, -1);
        return 0;
    }
    if ((p.frame[4] & 1) && !(p.flags & W_NOACK))
        return 0;
    for (i = 0; i < MAX_PENDING; i++) {
        q = &pending[i];
        if (q->used && q->remote && q->packet.radio == p.radio && q->packet.cookie == p.cookie)
            return 0;
        if (completed[i].used && completed[i].packet.radio == p.radio &&
            completed[i].packet.cookie == p.cookie) {
            (void)send_packet(&completed[i].packet);
            return 0;
        }
    }
    slot = allocate();
    if (slot < 0) {
        p.type = WLAN_RELAY_TX_STATUS;
        p.len = 0;
        p.flags = (p.flags & (W_MCS | W_GI | W_PRE)) | W_TX | W_FAIL;
        (void)send_packet(&p);
        return 0;
    }
    pending[slot].remote = 1;
    pending[slot].peer_done = 1;
    pending[slot].packet = p;
    if (distribute(&p, -1, slot) < 0)
        return -1;
    return finish((unsigned)slot);
}

/** Fail expired or stopping transactions and release every outstanding RX slot. */
static int expire(int all)
{
    uint64_t now = now_ms();
    unsigned i, j;
    for (i = 0; i < MAX_PENDING; i++) {
        if (!pending[i].used || (!all && pending[i].expires > now))
            continue;
        for (j = 0; j < MAX_INJECT; j++)
            if (injections[j].seq && injections[j].pending == (int)i)
                injections[j].seq = 0;
        pending[i].injects = 0;
        pending[i].peer_done = 1;
        if (finish(i) < 0)
            return -1;
    }
    return 0;
}

/** Parse a nonzero UDP port, returning zero on a missing digit or overflow. */
static unsigned port(const char *s)
{
    char *end;
    unsigned long n;
    errno = 0;
    n = strtoul(s, &end, 10);
    return !errno && *s && !*end && n > 0 && n <= 65535 ? (unsigned)n : 0;
}

/** Run one foreground relay until signalled; close sockets on every exit path. */
int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    struct sockaddr_nl local;
    struct sigaction sa;
    struct pollfd fds[2];
    struct message m;
    unsigned local_port, peer_port;
    uint32_t register_seq;
    int rc = 1, registered = 0, size = 4 * 1024 * 1024, result;
    if (argc != 4 || !(local_port = port(argv[1])) || !(peer_port = port(argv[3]))) {
        fprintf(stderr, "usage: hwsimrelay LOCAL_PORT PEER_IPV4 PEER_PORT\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0)
        goto out;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)local_port);
    if (bind(udp, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto out;
    addr.sin_port = htons((uint16_t)peer_port);
    if (inet_pton(AF_INET, argv[2], &addr.sin_addr) != 1) {
        errno = EINVAL;
        goto out;
    }
    if (connect(udp, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto out;
    nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (nl < 0)
        goto out;
    (void)setsockopt(nl, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    memset(&local, 0, sizeof(local));
    local.nl_family = AF_NETLINK;
    if (bind(nl, (struct sockaddr *)&local, sizeof(local)) < 0)
        goto out;
    family = resolve();
    if (family < 0 || discover() < 0) {
        fprintf(stderr, "hwsimrelay: cannot find hwsim family or radios\n");
        goto close_out;
    }
    next_cookie = (now_ms() << 24) ^ ((uint64_t)getpid() << 8);
    register_seq = begin(&m, family, H_REGISTER, NLM_F_ACK);
    if (submit(&m) < 0)
        goto out;
    fds[0].fd = nl;
    fds[0].events = POLLIN;
    fds[1].fd = udp;
    fds[1].events = POLLIN;
    while (!stopped) {
        result = poll(fds, 2, 100);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            goto out;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "hwsimrelay: netlink transport lost\n");
            goto close_out;
        }
        if (fds[0].revents & POLLIN)
            if (netlink_input(register_seq, &registered) < 0)
                goto out;
        if (registered && (fds[1].revents & POLLIN))
            if (udp_input() < 0)
                goto out;
        /* A connected UDP socket reports ICMP errors without a readable record. */
        if (fds[1].revents & POLLERR) {
            socklen_t len = sizeof(result);
            (void)getsockopt(udp, SOL_SOCKET, SO_ERROR, &result, &len);
        }
        if (expire(0) < 0)
            goto out;
    }
    rc = 0;
    goto close_out;
out:
    perror("hwsimrelay: relay");
close_out:
    if (nl >= 0) {
        if (expire(1) < 0)
            rc = 1;
        close(nl);
    }
    if (udp >= 0)
        close(udp);
    return rc;
}
