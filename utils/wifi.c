/**
 * wifi: scan for Wi-Fi networks and join, list and forget them, through
 * wpa_supplicant's control socket.
 *
 * `wifi scan` lists the networks in range, `wifi list` the known ones,
 * `wifi connect NAME` joins NAME, asking the passphrase or, for an
 * enterprise network, the identity and password, `wifi forget NAME` removes
 * it, and `wifi status` says what the station is joined to, or why it is
 * not. wpa_supplicant does the work and keeps the networks in its
 * configuration, which it writes back on `SAVE_CONFIG`; this program only
 * speaks its control protocol, the requests wpa_cli sends, over two datagram
 * sockets: one for requests and one attached for events.
 *
 * Every network is written with `bgscan="simple:30:-65:300"`, so that
 * wpa_supplicant scans every 30 seconds while the signal is below -65 dBm
 * and every 300 otherwise, roams between the access points of one network,
 * and moves to a stronger known network by itself.
 *
 * An enterprise network's server certificate is pinned on the first
 * connection, as phones do: the network is added with `ca_cert="probe://"`
 * and no password, so that the TLS handshake stops after the server's
 * certificate; wpa_supplicant reports it in `CTRL-EVENT-EAP-PEER-CERT`, and
 * the program shows its subject and SHA-256 fingerprint. On the user's yes
 * it sets `ca_cert="hash://server/sha256/HASH"` and the password and
 * connects; on no it removes the network. A later certificate that does not
 * match is refused by wpa_supplicant, and `wifi status` finds why in the
 * events it logged.
 */

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* The interface, the directory of wpa_supplicant's control sockets, the log
 * its events are printed to (init's service log, init(8)), and where this
 * program binds the sockets wpa_supplicant answers. A test defines them. */
#ifndef WIFI_IFACE
#define WIFI_IFACE "wlan0"
#endif
#ifndef WIFI_CTRL_DIR
#define WIFI_CTRL_DIR "/var/run/wpa_supplicant"
#endif
#ifndef WIFI_LOG
#define WIFI_LOG "/var/log/services"
#endif
#ifndef WIFI_CLIENT_DIR
#define WIFI_CLIENT_DIR "/tmp"
#endif

/* The background scan of every network: wpa_supplicant's `simple` module,
 * 30 seconds between scans below -65 dBm and 300 above. */
#define BGSCAN "simple:30:-65:300"

/* The longest reply or event read, which a certificate's DER in hex fills. */
#define MSG_MAX   16384
/* The longest request sent. */
#define REQ_MAX   2048
/* The most access points and known networks read. */
#define BSS_MAX   128
#define NET_MAX   64
/* An SSID is at most 32 bytes, which printf_encode makes at most four times
 * as long. */
#define SSID_TEXT 132

/* Seconds to wait for a reply, a save, which waits for the disk, a scan, the
 * server's certificate and a connection. */
#define REPLY_WAIT   10
#define SAVE_WAIT    120
#define SCAN_WAIT    20
#define PROBE_WAIT   45
#define CONNECT_WAIT 60

/* The two sockets to wpa_supplicant: requests on `cmd`, and events on
 * `mon` once attached; each bound at the path beside it, or -1 and "". */
struct ctrl {
    int cmd, mon;
    int attached;
    char cmd_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char mon_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

/* An access point of a scan: its SSID as wpa_supplicant prints it, its
 * frequency in MHz, its signal in dBm, and its flags, such as
 * `[WPA2-PSK-CCMP][ESS]`. */
struct bss {
    char ssid[SSID_TEXT];
    char bssid[18];
    int freq, signal;
    char flags[256];
};

/* A known network: its id and SSID as wpa_supplicant prints them, and its
 * flags, such as `[CURRENT]` and `[DISABLED]`. */
struct net {
    int id;
    char ssid[SSID_TEXT];
    char flags[64];
};

/* The kinds of network by their flags. */
enum security { SEC_OPEN, SEC_WEP, SEC_PERSONAL, SEC_ENTERPRISE, SEC_OTHER };

/* A server certificate wpa_supplicant reported at depth 0: its subject, its
 * SHA-256 in hex, and its first alternative name. */
struct cert {
    char subject[512];
    char hash[65];
    char alt[256];
};

/* Set by SIGINT, SIGTERM and SIGHUP, so that a wait ends and the networks
 * are put back as they were. */
static volatile sig_atomic_t interrupted;

/** Note the signal; the wait it ends sees `interrupted`. */
static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

/** Print `wifi: ` and the message to standard error. */
static void complain(const char *fmt, ...)
{
    va_list ap;

    fputs("wifi: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/** Seconds of the monotonic clock, for the deadlines of the waits. */
static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/**
 * Make a datagram socket bound at a new path under `WIFI_CLIENT_DIR` and
 * connected to `server`; the path goes in `path`. Returns the descriptor,
 * or -1 after a diagnostic.
 */
static int ctrl_socket(const char *server, char *path, size_t size, int n)
{
    struct sockaddr_un local, remote;
    int fd;

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        complain("socket: %s", strerror(errno));
        return -1;
    }
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "%s/wifi-%ld-%d", WIFI_CLIENT_DIR,
             (long)getpid(), n);
    (void)unlink(local.sun_path);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        complain("%s: %s", local.sun_path, strerror(errno));
        close(fd);
        return -1;
    }
    snprintf(path, size, "%s", local.sun_path);
    memset(&remote, 0, sizeof(remote));
    remote.sun_family = AF_UNIX;
    snprintf(remote.sun_path, sizeof(remote.sun_path), "%s", server);
    if (connect(fd, (struct sockaddr *)&remote, sizeof(remote)) != 0) {
        complain("cannot reach wpa_supplicant at %s: %s", server, strerror(errno));
        close(fd);
        (void)unlink(path);
        path[0] = '\0';
        return -1;
    }
    return fd;
}

/** Close the event socket of `c`, detached first, and remove its path. */
static void monitor_close(struct ctrl *c)
{
    if (c->mon >= 0) {
        if (c->attached) {
            (void)send(c->mon, "DETACH", 6, 0);
        }
        close(c->mon);
    }
    if (c->mon_path[0] != '\0') {
        (void)unlink(c->mon_path);
    }
    c->mon = -1;
    c->attached = 0;
    c->mon_path[0] = '\0';
}

/** Close the sockets of `c`, detaching the monitor, and remove their
 * paths. */
static void ctrl_close(struct ctrl *c)
{
    monitor_close(c);
    if (c->cmd >= 0) {
        close(c->cmd);
    }
    if (c->cmd_path[0] != '\0') {
        (void)unlink(c->cmd_path);
    }
    c->cmd = -1;
    c->cmd_path[0] = '\0';
}

/**
 * Read one datagram from `fd` into `buf`, NUL-terminated, waiting until the
 * monotonic second `deadline`. Returns its length, 0 when the deadline
 * passes or a signal came, or -1 after a diagnostic.
 */
static int ctrl_read(int fd, char *buf, size_t size, double deadline)
{
    struct pollfd p;
    double left;
    ssize_t n;
    int rc;

    for (;;) {
        if (interrupted) {
            return 0;
        }
        left = deadline - now();
        if (left <= 0) {
            return 0;
        }
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        rc = poll(&p, 1, (int)(left * 1000) + 1);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc < 0) {
            complain("poll: %s", strerror(errno));
            return -1;
        }
        if (rc == 0) {
            continue;
        }
        n = recv(fd, buf, size - 1, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            complain("reading from wpa_supplicant: %s", strerror(errno));
            return -1;
        }
        buf[n] = '\0';
        return (int)n;
    }
}

/**
 * Send the request `req` and read its reply into `reply`. Returns the
 * reply's length, or -1 after a diagnostic when it cannot be sent or none
 * comes.
 */
static int ctrl_request(struct ctrl *c, const char *req, char *reply, size_t size)
{
    int n;

    if (send(c->cmd, req, strlen(req), 0) < 0) {
        complain("sending to wpa_supplicant: %s", strerror(errno));
        return -1;
    }
    n = ctrl_read(c->cmd, reply, size,
                  now() + (strcmp(req, "SAVE_CONFIG") == 0 ? SAVE_WAIT : REPLY_WAIT));
    if (n == 0 && !interrupted) {
        complain("wpa_supplicant did not answer");
    }
    return n > 0 ? n : -1;
}

/** Send the request formatted from `fmt` and require the reply OK. Returns
 * 0, or -1 after a diagnostic naming the request's first word. */
static int ctrl_ok(struct ctrl *c, const char *fmt, ...)
{
    char req[REQ_MAX], reply[256];
    va_list ap;
    int n;
    size_t word;

    va_start(ap, fmt);
    n = vsnprintf(req, sizeof(req), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(req)) {
        complain("a request is too long");
        return -1;
    }
    n = ctrl_request(c, req, reply, sizeof(reply));
    memset(req, 0, sizeof(req));
    if (n < 0) {
        return -1;
    }
    if (strncmp(reply, "OK", 2) != 0) {
        word = strcspn(fmt, " ");
        complain("wpa_supplicant refused %.*s: %.*s", (int)word, fmt, (int)strcspn(reply, "\n"),
                 reply);
        return -1;
    }
    return 0;
}

/**
 * Open the event socket of `c` and attach it, so that wpa_supplicant sends
 * it each event from now on. Returns 0, or -1 after a diagnostic.
 */
static int monitor_open(struct ctrl *c)
{
    char server[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char reply[64];

    snprintf(server, sizeof(server), "%s/%s", WIFI_CTRL_DIR, WIFI_IFACE);
    c->mon = ctrl_socket(server, c->mon_path, sizeof(c->mon_path), 1);
    if (c->mon < 0) {
        return -1;
    }
    if (send(c->mon, "ATTACH", 6, 0) < 0 ||
        ctrl_read(c->mon, reply, sizeof(reply), now() + REPLY_WAIT) <= 0 ||
        strncmp(reply, "OK", 2) != 0) {
        complain("wpa_supplicant did not take the event monitor");
        monitor_close(c);
        return -1;
    }
    c->attached = 1;
    return 0;
}

/**
 * Open the request socket to wpa_supplicant's socket for `WIFI_IFACE` and,
 * when `events`, the event socket, attached. Returns 0, or -1 after a
 * diagnostic with `c` closed.
 */
static int ctrl_open(struct ctrl *c, int events)
{
    char server[sizeof(((struct sockaddr_un *)0)->sun_path)];

    c->cmd = c->mon = -1;
    c->attached = 0;
    c->cmd_path[0] = c->mon_path[0] = '\0';
    snprintf(server, sizeof(server), "%s/%s", WIFI_CTRL_DIR, WIFI_IFACE);
    c->cmd = ctrl_socket(server, c->cmd_path, sizeof(c->cmd_path), 0);
    if (c->cmd < 0) {
        return -1;
    }
    if (events && monitor_open(c) != 0) {
        ctrl_close(c);
        return -1;
    }
    return 0;
}

/**
 * Read the next event into `buf` without its `<N>` level, until the
 * monotonic second `deadline`. Returns 1 for an event, 0 when the deadline
 * passes or a signal came, -1 after a diagnostic.
 */
static int ctrl_event(struct ctrl *c, char *buf, size_t size, double deadline)
{
    char *body;
    int n;

    for (;;) {
        n = ctrl_read(c->mon, buf, size, deadline);
        if (n <= 0) {
            return n;
        }
        if (buf[0] != '<') {
            continue;
        }
        body = strchr(buf, '>');
        if (!body) {
            continue;
        }
        memmove(buf, body + 1, strlen(body + 1) + 1);
        return 1;
    }
}

/** Write `len` bytes of `src` as lowercase hex into `dst`, NUL-terminated. */
static void hex(char *dst, const unsigned char *src, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++) {
        dst[2 * i] = digits[src[i] >> 4];
        dst[2 * i + 1] = digits[src[i] & 15];
    }
    dst[2 * len] = '\0';
}

/** The value of the hex digit `ch`, or -1. */
static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/**
 * Decode `text`, an SSID as wpa_supplicant prints it (printf_encode: `\\`,
 * `\"`, `\e`, `\n`, `\r`, `\t` and `\xNN`), into its bytes in `out`, at
 * most `size`. Returns their count, or -1 when they do not fit.
 */
static int ssid_decode(const char *text, unsigned char *out, size_t size)
{
    size_t n = 0;
    int hi, lo, ch;

    while (*text != '\0') {
        ch = (unsigned char)*text++;
        if (ch == '\\' && *text != '\0') {
            ch = (unsigned char)*text++;
            switch (ch) {
            case 'e':
                ch = 033;
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'x':
                hi = hexval(text[0]);
                lo = hi < 0 ? -1 : hexval(text[1]);
                if (lo >= 0) {
                    ch = hi << 4 | lo;
                    text += 2;
                }
                break;
            default:
                break;
            }
        }
        if (n == size) {
            return -1;
        }
        out[n++] = (unsigned char)ch;
    }
    return (int)n;
}

/**
 * Set the string field `field` of network `id` to `value`, sent in hex so
 * that no quote or byte of it needs escaping. Returns what `ctrl_ok` does.
 */
static int set_hex(struct ctrl *c, int id, const char *field, const unsigned char *value,
                   size_t len)
{
    char h[2 * 512 + 1];
    int rc;

    if (len > 512) {
        complain("%s is too long", field);
        return -1;
    }
    hex(h, value, len);
    rc = ctrl_ok(c, "SET_NETWORK %d %s %s", id, field, h);
    memset(h, 0, sizeof(h));
    return rc;
}

/** Set the string field `field` of network `id` to the text `value`. */
static int set_text(struct ctrl *c, int id, const char *field, const char *value)
{
    return set_hex(c, id, field, (const unsigned char *)value, strlen(value));
}

/** The kind of network the scan flags `flags` describe. */
static enum security security_of(const char *flags)
{
    if (strstr(flags, "EAP")) {
        return SEC_ENTERPRISE;
    }
    if (strstr(flags, "PSK") || strstr(flags, "SAE")) {
        return SEC_PERSONAL;
    }
    if (strstr(flags, "WEP")) {
        return SEC_WEP;
    }
    if (strstr(flags, "WPA") || strstr(flags, "RSN") || strstr(flags, "OWE")) {
        return SEC_OTHER;
    }
    return SEC_OPEN;
}

/** A short name of the protection the scan flags `flags` describe. */
static const char *security_name(const char *flags)
{
    int sae = strstr(flags, "SAE") != 0;
    int psk2 = strstr(flags, "WPA2-PSK") != 0 || strstr(flags, "RSN-PSK") != 0;

    switch (security_of(flags)) {
    case SEC_ENTERPRISE:
        return "enterprise";
    case SEC_PERSONAL:
        if (sae && (psk2 || strstr(flags, "PSK+SAE"))) {
            return "WPA2/WPA3";
        }
        if (sae) {
            return "WPA3";
        }
        return psk2 ? "WPA2" : "WPA";
    case SEC_WEP:
        return "WEP";
    case SEC_OTHER:
        return "other";
    case SEC_OPEN:
    default:
        return "open";
    }
}

/** The 802.11 channel of the frequency `freq` in MHz, or 0. */
static int channel_of(int freq)
{
    if (freq == 2484) {
        return 14;
    }
    if (freq >= 2412 && freq <= 2472) {
        return (freq - 2407) / 5;
    }
    if (freq >= 5000 && freq <= 5900) {
        return (freq - 5000) / 5;
    }
    return 0;
}

/**
 * Split the next line of `*text` into at most `max` fields at tabs, into
 * `field`, and move `*text` past it. Returns the count of fields, or -1 at
 * the end of the text. The text is changed in place.
 */
static int split_line(char **text, char **field, int max)
{
    char *line = *text, *end, *tab;
    int n = 0;

    if (*line == '\0') {
        return -1;
    }
    end = strchr(line, '\n');
    if (end) {
        *end = '\0';
        *text = end + 1;
    } else {
        *text = line + strlen(line);
    }
    while (n < max) {
        field[n++] = line;
        tab = strchr(line, '\t');
        if (!tab) {
            break;
        }
        *tab = '\0';
        line = tab + 1;
    }
    return n;
}

/** Copy `src` into `dst` of `size` bytes, cut short if it must be. */
static void copy(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);

    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/**
 * Read the last scan's results into `bss`, at most `BSS_MAX`, the strongest
 * first. Returns their count, or -1 after a diagnostic.
 */
static int read_scan(struct ctrl *c, struct bss *bss)
{
    static char reply[MSG_MAX];
    char *text = reply, *f[5];
    struct bss t;
    int n = 0, k, i;

    if (ctrl_request(c, "SCAN_RESULTS", reply, sizeof(reply)) < 0) {
        return -1;
    }
    (void)split_line(&text, f, 5); /* the heading */
    while ((k = split_line(&text, f, 5)) >= 0) {
        if (k < 4 || n == BSS_MAX) {
            continue;
        }
        copy(bss[n].bssid, sizeof(bss[n].bssid), f[0]);
        bss[n].freq = atoi(f[1]);
        bss[n].signal = atoi(f[2]);
        copy(bss[n].flags, sizeof(bss[n].flags), f[3]);
        copy(bss[n].ssid, sizeof(bss[n].ssid), k == 5 ? f[4] : "");
        for (i = n; i > 0 && bss[i - 1].signal < bss[i].signal; i--) {
            t = bss[i - 1];
            bss[i - 1] = bss[i];
            bss[i] = t;
        }
        n++;
    }
    return n;
}

/** Read the known networks into `nets`, at most `NET_MAX`. Returns their
 * count, or -1 after a diagnostic. */
static int read_networks(struct ctrl *c, struct net *nets)
{
    static char reply[MSG_MAX];
    char *text = reply, *f[4];
    int n = 0, k;

    if (ctrl_request(c, "LIST_NETWORKS", reply, sizeof(reply)) < 0) {
        return -1;
    }
    (void)split_line(&text, f, 4);
    while ((k = split_line(&text, f, 4)) >= 0) {
        if (k < 2 || n == NET_MAX) {
            continue;
        }
        nets[n].id = atoi(f[0]);
        copy(nets[n].ssid, sizeof(nets[n].ssid), f[1]);
        copy(nets[n].flags, sizeof(nets[n].flags), k == 4 ? f[3] : "");
        n++;
    }
    return n;
}

/** The id of the known network named `name`, or -1; -2 after a
 * diagnostic. */
static int known_id(struct ctrl *c, const char *name)
{
    struct net nets[NET_MAX];
    int n, i;

    n = read_networks(c, nets);
    if (n < 0) {
        return -2;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(nets[i].ssid, name) == 0) {
            return nets[i].id;
        }
    }
    return -1;
}

/**
 * Scan and wait for the results. A scan already running, which wpa_supplicant
 * answers FAIL-BUSY, is waited for too. Returns 0, or -1 after a
 * diagnostic.
 */
static int scan(struct ctrl *c)
{
    char reply[64], ev[MSG_MAX];
    double deadline = now() + SCAN_WAIT;
    int rc;

    if (ctrl_request(c, "SCAN", reply, sizeof(reply)) < 0) {
        return -1;
    }
    if (strncmp(reply, "OK", 2) != 0 && strncmp(reply, "FAIL-BUSY", 9) != 0) {
        complain("wpa_supplicant refused SCAN: %.*s", (int)strcspn(reply, "\n"), reply);
        return -1;
    }
    while ((rc = ctrl_event(c, ev, sizeof(ev), deadline)) > 0) {
        if (strncmp(ev, "CTRL-EVENT-SCAN-RESULTS", 23) == 0) {
            return 0;
        }
        if (strncmp(ev, "CTRL-EVENT-SCAN-FAILED", 22) == 0) {
            complain("the scan failed: %s", ev);
            return -1;
        }
    }
    if (rc == 0 && !interrupted) {
        complain("no scan results came");
    }
    return -1;
}

/** `wifi scan`: scan, and print each network in range once, at its
 * strongest access point, the strongest first. */
static int cmd_scan(struct ctrl *c)
{
    static struct bss bss[BSS_MAX];
    struct net nets[NET_MAX];
    int n, k, i, j, seen, known;

    if (scan(c) != 0 || (n = read_scan(c, bss)) < 0 || (k = read_networks(c, nets)) < 0) {
        return 1;
    }
    printf("%-32s %7s %7s  %s\n", "NAME", "SIGNAL", "CHANNEL", "SECURITY");
    for (i = 0; i < n; i++) {
        if (bss[i].ssid[0] == '\0') {
            continue;
        }
        for (seen = 0, j = 0; j < i && !seen; j++) {
            seen = strcmp(bss[j].ssid, bss[i].ssid) == 0;
        }
        if (seen) {
            continue;
        }
        for (known = 0, j = 0; j < k && !known; j++) {
            known = strcmp(nets[j].ssid, bss[i].ssid) == 0;
        }
        printf("%-32s %3d dBm %7d  %s%s\n", bss[i].ssid, bss[i].signal, channel_of(bss[i].freq),
               security_name(bss[i].flags), known ? ", known" : "");
    }
    return 0;
}

/**
 * Read the field `field` of network `id` into `out`, without the quotes
 * wpa_supplicant puts around a string. Returns 0, or -1 when the network has
 * no such field set.
 */
static int get_field(struct ctrl *c, int id, const char *field, char *out, size_t size)
{
    char req[128], reply[1024];
    size_t len;

    snprintf(req, sizeof(req), "GET_NETWORK %d %s", id, field);
    if (ctrl_request(c, req, reply, sizeof(reply)) < 0 || strncmp(reply, "FAIL", 4) == 0) {
        return -1;
    }
    len = strcspn(reply, "\n");
    reply[len] = '\0';
    if (len >= 2 && reply[0] == '"' && reply[len - 1] == '"') {
        reply[len - 1] = '\0';
        copy(out, size, reply + 1);
    } else {
        copy(out, size, reply);
    }
    return 0;
}

/** Print a certificate's SHA-256 `hash`, in hex, as colon-separated pairs
 * in two lines of 16 bytes, each after `lead`. */
static void print_fingerprint(const char *lead, const char *hash)
{
    int i, len = (int)strlen(hash);

    for (i = 0; i + 1 < len; i += 2) {
        if (i % 32 == 0) {
            printf("%s%s", i ? "\n" : "", i ? "           " : lead);
        }
        printf("%c%c%s", toupper((unsigned char)hash[i]), toupper((unsigned char)hash[i + 1]),
               i + 2 < len && (i + 2) % 32 != 0 ? ":" : "");
    }
    printf("\n");
}

/** `wifi list`: print the known networks, which one is in use, which are
 * disabled, and an enterprise network's pinned certificate. */
static int cmd_list(struct ctrl *c)
{
    struct net nets[NET_MAX];
    char ca[256];
    int n, i;
    const char *pin = "hash://server/sha256/";

    n = read_networks(c, nets);
    if (n < 0) {
        return 1;
    }
    for (i = 0; i < n; i++) {
        printf("%s%s%s%s\n", nets[i].ssid, strstr(nets[i].flags, "[CURRENT]") ? ", in use" : "",
               strstr(nets[i].flags, "[DISABLED]") ? ", disabled" : "",
               strstr(nets[i].flags, "[TEMP-DISABLED]") ? ", failing" : "");
        if (get_field(c, nets[i].id, "ca_cert", ca, sizeof(ca)) == 0 &&
            strncmp(ca, pin, strlen(pin)) == 0) {
            print_fingerprint("  SHA-256: ", ca + strlen(pin));
        }
    }
    return 0;
}

/** `wifi forget NAME`: remove every known network named `name` and save. */
static int cmd_forget(struct ctrl *c, const char *name)
{
    int id, removed = 0;

    while ((id = known_id(c, name)) >= 0) {
        if (ctrl_ok(c, "REMOVE_NETWORK %d", id) != 0) {
            return 1;
        }
        removed++;
    }
    if (id == -2) {
        return 1;
    }
    if (!removed) {
        complain("%s: no such known network", name);
        return 1;
    }
    if (ctrl_ok(c, "SAVE_CONFIG") != 0) {
        return 1;
    }
    printf("wifi: forgot %s\n", name);
    return 0;
}

/**
 * Find the value of `key=` in the reply `text` of lines KEY=VALUE, into
 * `out`. Returns 0, or -1 when the key is not there.
 */
static int status_field(const char *text, const char *key, char *out, size_t size)
{
    size_t klen = strlen(key);
    const char *line = text;

    while (*line != '\0') {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            snprintf(out, size, "%.*s", (int)strcspn(line + klen + 1, "\n"), line + klen + 1);
            return 0;
        }
        line += strcspn(line, "\n");
        if (*line == '\n') {
            line++;
        }
    }
    return -1;
}

/**
 * Take the quoted value after `key='` in `text` into `out`, to the last
 * `'` before `end` or before the end of `text`. Returns 0, or -1 when the
 * key is not there.
 */
static int quoted_field(const char *text, const char *key, const char *end, char *out, size_t size)
{
    const char *start = strstr(text, key), *stop;

    if (!start) {
        return -1;
    }
    start += strlen(key);
    stop = end ? strstr(start, end) : 0;
    if (!stop) {
        stop = strrchr(start, '\'');
    }
    if (!stop || stop < start) {
        return -1;
    }
    snprintf(out, size, "%.*s", (int)(stop - start), start);
    return 0;
}

/** Take the value of the word `key=VALUE` in `text` into `out`. Returns 0,
 * or -1 when it is not there. */
static int word_field(const char *text, const char *key, char *out, size_t size)
{
    const char *start = strstr(text, key);

    if (!start) {
        return -1;
    }
    start += strlen(key);
    snprintf(out, size, "%.*s", (int)strcspn(start, " \n"), start);
    return 0;
}

/**
 * Explain the event `ev`, a failure of a network, into `why`: a refused
 * passphrase, identity or certificate, or an access point that refused the
 * station. Returns 1 when `ev` is such a failure, else 0.
 */
static int failure_of(const char *ev, char *why, size_t size)
{
    char reason[64], err[128];

    if (strncmp(ev, "CTRL-EVENT-EAP-TLS-CERT-ERROR", 29) == 0) {
        if (quoted_field(ev, "err='", 0, err, sizeof(err)) != 0) {
            copy(err, sizeof(err), "unknown");
        }
        if (strstr(err, "mismatch")) {
            copy(why, size, "the server's certificate is not the one trusted for this network");
        } else {
            snprintf(why, size, "the server's certificate was refused: %s", err);
        }
        return 1;
    }
    if (strncmp(ev, "CTRL-EVENT-SSID-TEMP-DISABLED", 29) == 0) {
        if (word_field(ev, "reason=", reason, sizeof(reason)) != 0) {
            copy(reason, sizeof(reason), "");
        }
        if (strcmp(reason, "WRONG_KEY") == 0) {
            copy(why, size, "the network refused the passphrase");
        } else if (strcmp(reason, "AUTH_FAILED") == 0) {
            copy(why, size, "the network refused the identity or password");
        } else {
            snprintf(why, size, "the connection failed (%s)", reason[0] ? reason : "no reason");
        }
        return 1;
    }
    if (strncmp(ev, "CTRL-EVENT-EAP-FAILURE", 22) == 0) {
        copy(why, size, "the network refused the identity or password");
        return 1;
    }
    if (strncmp(ev, "CTRL-EVENT-ASSOC-REJECT", 23) == 0) {
        if (word_field(ev, "status_code=", reason, sizeof(reason)) != 0) {
            copy(reason, sizeof(reason), "?");
        }
        snprintf(why, size, "the access point refused the station (status %s)", reason);
        return 1;
    }
    return 0;
}

/**
 * Find in the log `WIFI_LOG` why the station is not connected: the last
 * failure wpa_supplicant logged for `WIFI_IFACE` since it started or last
 * connected, into `why`, and the certificate the server last presented into
 * `cert`. Returns 1 when one is found, else 0.
 *
 * A refused certificate is the reason for the rest of its attempt, which
 * begins at an authentication, an association or the start of EAP:
 * wpa_supplicant follows it with `CTRL-EVENT-EAP-FAILURE` and an
 * `AUTH_FAILED` that would otherwise read as a refused password.
 */
static int logged_failure(char *why, size_t size, struct cert *cert)
{
    char line[MSG_MAX], prefix[32], auth[48], assoc[48], *ev;
    FILE *f = fopen(WIFI_LOG, "r");
    int found = 0, refused = 0;

    if (!f) {
        return 0;
    }
    snprintf(prefix, sizeof(prefix), "%s: CTRL-EVENT-", WIFI_IFACE);
    snprintf(auth, sizeof(auth), "%s: SME: Trying to authenticate with ", WIFI_IFACE);
    snprintf(assoc, sizeof(assoc), "%s: Trying to associate with ", WIFI_IFACE);
    cert->hash[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strstr(line, "Successfully initialized wpa_supplicant")) {
            found = refused = 0;
            cert->hash[0] = '\0';
            continue;
        }
        if (strstr(line, auth) || strstr(line, assoc)) {
            refused = 0;
            continue;
        }
        ev = strstr(line, prefix);
        if (!ev) {
            continue;
        }
        ev += strlen(WIFI_IFACE) + 2;
        if (strncmp(ev, "CTRL-EVENT-CONNECTED", 20) == 0) {
            found = refused = 0;
            cert->hash[0] = '\0';
        } else if (strncmp(ev, "CTRL-EVENT-EAP-STARTED", 22) == 0) {
            refused = 0;
        } else if (strncmp(ev, "CTRL-EVENT-EAP-PEER-CERT depth=0 ", 33) == 0 &&
                   word_field(ev, " hash=", cert->hash, sizeof(cert->hash)) == 0) {
            if (quoted_field(ev, "subject='", "' hash=", cert->subject, sizeof(cert->subject)) !=
                0) {
                cert->subject[0] = '\0';
            }
        } else if (!refused && failure_of(ev, why, size)) {
            found = 1;
            refused = strncmp(ev, "CTRL-EVENT-EAP-TLS-CERT-ERROR", 29) == 0;
        }
    }
    fclose(f);
    return found;
}

/** `wifi status`: print the network the station is joined to, its access
 * point, channel, signal and address, or the state and why. */
static int cmd_status(struct ctrl *c)
{
    static char reply[MSG_MAX];
    char state[32], ssid[SSID_TEXT], bssid[32], freq[16], mgmt[64], ip[64], rssi[16];
    char poll[256], why[256];
    struct cert cert;

    if (ctrl_request(c, "STATUS", reply, sizeof(reply)) < 0) {
        return 1;
    }
    if (status_field(reply, "wpa_state", state, sizeof(state)) != 0) {
        copy(state, sizeof(state), "UNKNOWN");
    }
    if (strcmp(state, "COMPLETED") == 0 && status_field(reply, "ssid", ssid, sizeof(ssid)) == 0) {
        if (status_field(reply, "bssid", bssid, sizeof(bssid)) != 0) {
            copy(bssid, sizeof(bssid), "?");
        }
        if (status_field(reply, "freq", freq, sizeof(freq)) != 0) {
            copy(freq, sizeof(freq), "0");
        }
        if (status_field(reply, "key_mgmt", mgmt, sizeof(mgmt)) != 0) {
            copy(mgmt, sizeof(mgmt), "NONE");
        }
        printf("connected to %s\n", ssid);
        printf("  access point %s, channel %d\n", bssid, channel_of(atoi(freq)));
        if (ctrl_request(c, "SIGNAL_POLL", poll, sizeof(poll)) > 0 &&
            status_field(poll, "RSSI", rssi, sizeof(rssi)) == 0) {
            printf("  signal %s dBm\n", rssi);
        }
        printf("  security %s\n", mgmt);
        if (status_field(reply, "ip_address", ip, sizeof(ip)) == 0) {
            printf("  address %s\n", ip);
        }
        return 0;
    }
    printf("not connected (%s)\n", state);
    if (logged_failure(why, sizeof(why), &cert)) {
        printf("  %s\n", why);
        if (strstr(why, "not the one trusted") && cert.hash[0] != '\0') {
            printf("  it presented %s\n", cert.subject[0] ? cert.subject : "a certificate");
            print_fingerprint("  SHA-256: ", cert.hash);
            printf("  if the network's administrators changed it, `wifi forget` and\n"
                   "  `wifi connect` the network to trust the new one\n");
        }
    }
    return 0;
}

/**
 * Read a line from standard input into `buf` after printing `prompt`,
 * without echo when `secret` and the input is a terminal. Returns 0, or -1
 * at the end of the input or on a signal.
 */
static int ask(const char *prompt, char *buf, size_t size, int secret)
{
    struct termios old, quiet;
    int hide = secret && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    char *got;

    /* Echo goes off before the prompt shows, since TCSAFLUSH discards what
     * was typed before it: an answer typed once the prompt is seen is kept. */
    if (hide) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    fputs(prompt, stdout);
    fflush(stdout);
    got = fgets(buf, (int)size, stdin);
    if (hide) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        fputc('\n', stdout);
    }
    if (!got || interrupted) {
        buf[0] = '\0';
        return -1;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

/** Whether `s` is a valid WPA passphrase, 8 to 63 printable characters. */
static int passphrase_ok(const char *s)
{
    size_t i, n = strlen(s);

    for (i = 0; i < n; i++) {
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) {
            return 0;
        }
    }
    return n >= 8 && n <= 63;
}

/** Whether `s` is a raw PSK, 64 hex digits. */
static int psk_hex(const char *s)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        if (hexval((unsigned char)s[i]) < 0) {
            return 0;
        }
    }
    return i == 64;
}

/* The networks enabled before a SELECT_NETWORK disabled them, to enable
 * again after. */
struct enabled {
    int id[NET_MAX];
    int n;
};

/** Note in `e` the known networks but `id` that are enabled. Returns 0, or
 * -1 after a diagnostic. */
static int note_enabled(struct ctrl *c, int id, struct enabled *e)
{
    struct net nets[NET_MAX];
    int n, i;

    e->n = 0;
    n = read_networks(c, nets);
    if (n < 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (nets[i].id != id && !strstr(nets[i].flags, "[DISABLED]")) {
            e->id[e->n++] = nets[i].id;
        }
    }
    return 0;
}

/** Enable again the networks of `e`, without connecting to them now. */
static void restore_enabled(struct ctrl *c, const struct enabled *e)
{
    int i;

    for (i = 0; i < e->n; i++) {
        (void)ctrl_ok(c, "ENABLE_NETWORK %d no-connect", e->id[i]);
    }
}

/** The network id of an event's `[id=N` or `id=N`, or -1. */
static int event_id(const char *ev)
{
    const char *at = strstr(ev, "id=");

    while (at && at > ev && at[-1] != '[' && at[-1] != ' ') {
        at = strstr(at + 3, "id=");
    }
    return at ? atoi(at + 3) : -1;
}

/**
 * Select network `id` and wait for what the select brings. With `probe`, the
 * network's `ca_cert` is `probe://`, and the wait ends at the server's
 * certificate, which goes in `cert`, and the network is disabled after it;
 * otherwise the wait ends at the connection or at a failure. The other
 * networks are disabled while it lasts, as SELECT_NETWORK does, and enabled
 * again at its end. Returns 0 when it got what it waited for, or -1 with the
 * reason in `why`.
 */
static int select_wait(struct ctrl *c, int id, int probe, struct cert *cert, char *why, size_t size)
{
    static char ev[MSG_MAX];
    struct enabled e;
    double deadline = now() + (probe ? PROBE_WAIT : CONNECT_WAIT);
    int rc, got = -1;

    copy(why, size, probe ? "the network sent no certificate" : "the network did not answer");
    if (note_enabled(c, id, &e) != 0 || ctrl_ok(c, "SELECT_NETWORK %d", id) != 0) {
        copy(why, size, "wpa_supplicant refused the network");
        return -1;
    }
    while ((rc = ctrl_event(c, ev, sizeof(ev), deadline)) > 0) {
        if (got == 0 && strncmp(ev, "CTRL-EVENT-EAP-PEER-", 20) != 0) {
            break; /* the certificate's events are over */
        }
        if (probe && strncmp(ev, "CTRL-EVENT-EAP-PEER-ALT depth=0 ", 32) == 0) {
            if (cert->alt[0] == '\0') {
                copy(cert->alt, sizeof(cert->alt), ev + 32);
            }
        } else if (probe && got < 0 && strncmp(ev, "CTRL-EVENT-EAP-PEER-CERT depth=0 ", 33) == 0 &&
                   word_field(ev, " hash=", cert->hash, sizeof(cert->hash)) == 0) {
            if (quoted_field(ev, "subject='", "' hash=", cert->subject, sizeof(cert->subject)) !=
                0) {
                cert->subject[0] = '\0';
            }
            if (strlen(cert->hash) != 64) {
                copy(why, size, "the network's certificate came without its SHA-256");
                break;
            }
            /* its names come in the events after it, at once */
            got = 0;
            deadline = now() + 1;
        } else if (!probe && strncmp(ev, "CTRL-EVENT-CONNECTED", 20) == 0 && event_id(ev) == id) {
            got = 0;
            break;
        } else if (!probe && failure_of(ev, why, size)) {
            /* a refused association is tried again, at another access
             * point or later; the others end the wait */
            if (strncmp(ev, "CTRL-EVENT-ASSOC-REJECT", 23) == 0) {
                continue;
            }
            if (strncmp(ev, "CTRL-EVENT-SSID-TEMP-DISABLED", 29) != 0 || event_id(ev) == id) {
                break;
            }
        }
    }
    if (probe) {
        /* stop wpa_supplicant's own retries of the probe */
        (void)ctrl_ok(c, "DISABLE_NETWORK %d", id);
    }
    restore_enabled(c, &e);
    if (interrupted) {
        copy(why, size, "interrupted");
    }
    return got;
}

/** Print the certificate `cert` of the network `name`, and ask whether to
 * trust it. Returns 1 for yes, 0 for no or no answer. */
static int trust(const char *name, const struct cert *cert)
{
    char answer[16];

    printf("%s presents this server certificate:\n", name);
    printf("  subject: %s\n", cert->subject[0] ? cert->subject : "(none)");
    if (cert->alt[0] != '\0') {
        printf("  name:    %s\n", cert->alt);
    }
    print_fingerprint("  SHA-256: ", cert->hash);
    printf("Compare the fingerprint with the one the network's administrators publish.\n");
    if (ask("Trust it for this network? [y/N] ", answer, sizeof(answer), 0) != 0) {
        return 0;
    }
    return strcmp(answer, "y") == 0 || strcmp(answer, "Y") == 0 || strcmp(answer, "yes") == 0 ||
           strcmp(answer, "Yes") == 0;
}

/* What `wifi connect` asks for an enterprise network, and the EAP methods. */
struct eap {
    char identity[256], password[256], anonymous[256], domain[256];
    const char *methods, *phase2;
};

/** Ask the identity, password, anonymous identity and server domain of an
 * enterprise network into `a`. Returns 0, or -1 when the input ends. */
static int ask_eap(struct eap *a)
{
    if (ask("Identity: ", a->identity, sizeof(a->identity), 0) != 0 ||
        ask("Password: ", a->password, sizeof(a->password), 1) != 0 ||
        ask("Anonymous identity (empty for none; eduroam's is anonymous@REALM): ", a->anonymous,
            sizeof(a->anonymous), 0) != 0 ||
        ask("Server domain to check (empty for none): ", a->domain, sizeof(a->domain), 0) != 0) {
        return -1;
    }
    if (a->identity[0] == '\0') {
        complain("an enterprise network needs an identity");
        return -1;
    }
    return 0;
}

/**
 * Write the fields of a new enterprise network `id` but its password and
 * server domain, with `ca_cert="probe://"`, so that the TLS handshake stops
 * at the server's certificate. Returns 0, or -1 after a diagnostic.
 */
static int set_eap(struct ctrl *c, int id, const struct eap *a)
{
    if (ctrl_ok(c, "SET_NETWORK %d key_mgmt WPA-EAP WPA-EAP-SHA256", id) != 0 ||
        ctrl_ok(c, "SET_NETWORK %d ieee80211w 1", id) != 0 ||
        ctrl_ok(c, "SET_NETWORK %d eap %s", id, a->methods) != 0 ||
        ctrl_ok(c, "SET_NETWORK %d phase2 \"%s\"", id, a->phase2) != 0 ||
        set_text(c, id, "identity", a->identity) != 0 ||
        (a->anonymous[0] != '\0' && set_text(c, id, "anonymous_identity", a->anonymous) != 0)) {
        return -1;
    }
    return ctrl_ok(c, "SET_NETWORK %d ca_cert \"probe://\"", id);
}

/**
 * Write the passphrase or key of a new personal network `id`: WPA and WPA2's
 * PSK and WPA3's SAE from a passphrase, with management-frame protection
 * where the access point offers it, or PSK alone from 64 hex digits.
 * Returns 0, or -1 after a diagnostic.
 */
static int set_personal(struct ctrl *c, int id, const char *pass)
{
    char req[REQ_MAX];
    int n, rc;

    if (psk_hex(pass)) {
        return ctrl_ok(c, "SET_NETWORK %d key_mgmt WPA-PSK WPA-PSK-SHA256", id) != 0 ||
                       ctrl_ok(c, "SET_NETWORK %d ieee80211w 1", id) != 0 ||
                       ctrl_ok(c, "SET_NETWORK %d psk %s", id, pass) != 0
                   ? -1
                   : 0;
    }
    if (!passphrase_ok(pass)) {
        complain("a passphrase is 8 to 63 printable characters, or 64 hex digits");
        return -1;
    }
    if (ctrl_ok(c, "SET_NETWORK %d key_mgmt WPA-PSK WPA-PSK-SHA256 SAE", id) != 0 ||
        ctrl_ok(c, "SET_NETWORK %d ieee80211w 1", id) != 0) {
        return -1;
    }
    /* psk takes a passphrase only quoted: the text to its last quote */
    n = snprintf(req, sizeof(req), "SET_NETWORK %d psk \"%s\"", id, pass);
    rc = n > 0 && (size_t)n < sizeof(req) ? ctrl_ok(c, "%s", req) : -1;
    memset(req, 0, sizeof(req));
    return rc;
}

/** Remove network `id`, which this run added and does not keep. */
static void drop(struct ctrl *c, int id)
{
    (void)ctrl_ok(c, "REMOVE_NETWORK %d", id);
}

/** `wifi connect` of the known network `name`, whose id is `id`: nothing
 * when it is the one joined, else select it and wait for the join. Returns
 * the command's exit status. */
static int connect_known(struct ctrl *c, const char *name, int id)
{
    char state[64], why[256], reply[MSG_MAX], idtext[16];
    struct cert cert;

    memset(&cert, 0, sizeof(cert));
    if (ctrl_request(c, "STATUS", reply, sizeof(reply)) > 0 &&
        status_field(reply, "wpa_state", state, sizeof(state)) == 0 &&
        strcmp(state, "COMPLETED") == 0 && status_field(reply, "id", idtext, sizeof(idtext)) == 0 &&
        atoi(idtext) == id) {
        printf("wifi: already connected to %s\n", name);
        return 0;
    }
    if (select_wait(c, id, 0, &cert, why, sizeof(why)) != 0) {
        complain("%s: %s", name, why);
        if (strstr(why, "not the one trusted")) {
            complain("`wifi status` shows the certificate it presented");
        }
        return 1;
    }
    printf("wifi: connected to %s\n", name);
    return 0;
}

/** The index in bss of the network `name` in the last scan, or in a new
 * one when the last has none. Returns -1, having said why, when neither has
 * it or a scan fails. */
static int find_bss(struct ctrl *c, struct bss *bss, const char *name)
{
    int n, i;

    n = read_scan(c, bss);
    if (n < 0) {
        return -1;
    }
    for (i = 0; i < n && strcmp(bss[i].ssid, name) != 0; i++) {
    }
    if (i < n) {
        return i;
    }
    if (scan(c) != 0 || (n = read_scan(c, bss)) < 0) {
        return -1;
    }
    for (i = 0; i < n && strcmp(bss[i].ssid, name) != 0; i++) {
    }
    if (i == n) {
        complain("%s: no such network in range; `wifi scan` lists them", name);
        return -1;
    }
    return i;
}

/**
 * Probe the enterprise network `name`, added as `id` with no password yet,
 * for its server's certificate, which goes in cert, and on the user's yes
 * pin it and set the password and domain of `a`. Returns 0, or -1 having
 * said why and dropped the network. Wipes `a` either way.
 */
static int pin_enterprise(struct ctrl *c, int id, const char *name, struct eap *a,
                          struct cert *cert)
{
    char why[256];

    /* No password is set yet, and the handshake stops at the server's
     * certificate, so nothing secret leaves the machine before the
     * user's yes. */
    if (select_wait(c, id, 1, cert, why, sizeof(why)) != 0) {
        complain("%s: %s", name, why);
        memset(a, 0, sizeof(*a));
        drop(c, id);
        return -1;
    }
    /* The events of the probe, and those that would fill the socket's
     * queue while the user reads, are dropped with the socket; a new
     * one sees only what comes after the answer. */
    monitor_close(c);
    if (!trust(name, cert)) {
        printf("wifi: %s is not trusted, and is not kept\n", name);
        memset(a, 0, sizeof(*a));
        drop(c, id);
        return -1;
    }
    if (ctrl_ok(c, "SET_NETWORK %d ca_cert \"hash://server/sha256/%s\"", id, cert->hash) != 0 ||
        set_text(c, id, "password", a->password) != 0 ||
        (a->domain[0] != '\0' && set_text(c, id, "domain_suffix_match", a->domain) != 0) ||
        ctrl_ok(c, "ENABLE_NETWORK %d no-connect", id) != 0 || monitor_open(c) != 0) {
        memset(a, 0, sizeof(*a));
        drop(c, id);
        return -1;
    }
    memset(a, 0, sizeof(*a));
    return 0;
}

/**
 * Add a network for the SSID of ssid_len bytes at ssid, protected as sec
 * says, by the passphrase pass or the enterprise settings a, and return its
 * id; or -1, having said why and dropped what it added.
 */
static int add_network(struct ctrl *c, const unsigned char *ssid, int ssid_len, enum security sec,
                       const char *pass, const struct eap *a)
{
    char reply[MSG_MAX];
    int id;

    if (ctrl_request(c, "ADD_NETWORK", reply, sizeof(reply)) < 0 ||
        !isdigit((unsigned char)reply[0])) {
        complain("wpa_supplicant refused ADD_NETWORK");
        return -1;
    }
    id = atoi(reply);
    if (set_hex(c, id, "ssid", ssid, (size_t)ssid_len) != 0 ||
        ctrl_ok(c, "SET_NETWORK %d bgscan \"%s\"", id, BGSCAN) != 0 ||
        (sec == SEC_OPEN && ctrl_ok(c, "SET_NETWORK %d key_mgmt NONE", id) != 0) ||
        (sec == SEC_PERSONAL && set_personal(c, id, pass) != 0) ||
        (sec == SEC_ENTERPRISE && set_eap(c, id, a) != 0)) {
        drop(c, id);
        return -1;
    }
    return id;
}

/**
 * `wifi connect NAME`: join the known network `name`, or add it from the
 * scan, asking its passphrase or its identity and password, probe an
 * enterprise network's certificate and pin it on the user's yes, and save
 * the networks once it is joined. `methods` and `phase2` are the EAP methods
 * of a new enterprise network.
 */
static int cmd_connect(struct ctrl *c, const char *name, const char *methods, const char *phase2)
{
    static struct bss bss[BSS_MAX];
    static struct eap a;
    char why[256], pass[256];
    unsigned char ssid[64];
    struct cert cert;
    int i, id, ssid_len;
    enum security sec;

    id = known_id(c, name);
    if (id == -2) {
        return 1;
    }
    if (id >= 0) {
        return connect_known(c, name, id);
    }

    /* A new network: find it in the last scan, or in a new one. */
    i = find_bss(c, bss, name);
    if (i < 0) {
        return 1;
    }
    sec = security_of(bss[i].flags);
    if (sec == SEC_WEP || sec == SEC_OTHER) {
        complain("%s: its protection, %s, is not supported", name, bss[i].flags);
        return 1;
    }
    ssid_len = ssid_decode(name, ssid, sizeof(ssid));
    if (ssid_len <= 0 || ssid_len > 32) {
        complain("%s: not a network name", name);
        return 1;
    }

    memset(&a, 0, sizeof(a));
    memset(&cert, 0, sizeof(cert));
    pass[0] = '\0';
    if (sec == SEC_PERSONAL && ask("Passphrase: ", pass, sizeof(pass), 1) != 0) {
        return 1;
    }
    if (sec == SEC_ENTERPRISE) {
        a.methods = methods;
        a.phase2 = phase2;
        if (ask_eap(&a) != 0) {
            memset(&a, 0, sizeof(a));
            return 1;
        }
    }

    id = add_network(c, ssid, ssid_len, sec, pass, &a);
    memset(pass, 0, sizeof(pass));
    if (id < 0) {
        memset(&a, 0, sizeof(a));
        return 1;
    }
    if (sec == SEC_ENTERPRISE && pin_enterprise(c, id, name, &a, &cert) != 0) {
        return 1;
    }

    if (select_wait(c, id, 0, &cert, why, sizeof(why)) != 0) {
        complain("%s: %s; the network is not kept", name, why);
        drop(c, id);
        return 1;
    }
    if (ctrl_ok(c, "SAVE_CONFIG") != 0) {
        complain("%s: connected, but the network could not be saved", name);
        return 1;
    }
    printf("wifi: connected to %s\n", name);
    return 0;
}

/** Print the usage to standard error and return 2. */
static int usage(void)
{
    fprintf(stderr, "usage: wifi scan\n"
                    "       wifi list\n"
                    "       wifi connect [-e peap|ttls|ttls-pap] NAME\n"
                    "       wifi forget NAME\n"
                    "       wifi status\n");
    return 2;
}

/** Run `wifi` with `argc` and `argv`; the status to exit with. */
int main(int argc, char **argv)
{
    struct ctrl c;
    struct sigaction sa;
    const char *methods = "PEAP TTLS", *phase2 = "auth=MSCHAPV2", *cmd;
    int rc, arg = 2;

    interrupted = 0;
    if (argc < 2) {
        return usage();
    }
    cmd = argv[1];
    if (strcmp(cmd, "connect") == 0 && argc > 3 && strcmp(argv[2], "-e") == 0) {
        arg = 4;
        if (strcmp(argv[3], "peap") == 0) {
            methods = "PEAP";
        } else if (strcmp(argv[3], "ttls") == 0) {
            methods = "TTLS";
        } else if (strcmp(argv[3], "ttls-pap") == 0) {
            methods = "TTLS";
            phase2 = "auth=PAP";
        } else {
            return usage();
        }
    }
    if ((strcmp(cmd, "connect") == 0 || strcmp(cmd, "forget") == 0) ? argc != arg + 1 : argc != 2) {
        return usage();
    }
    if (strcmp(cmd, "scan") != 0 && strcmp(cmd, "list") != 0 && strcmp(cmd, "connect") != 0 &&
        strcmp(cmd, "forget") != 0 && strcmp(cmd, "status") != 0) {
        return usage();
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGHUP, &sa, NULL);

    if (ctrl_open(&c, strcmp(cmd, "scan") == 0 || strcmp(cmd, "connect") == 0) != 0) {
        return 1;
    }
    if (strcmp(cmd, "scan") == 0) {
        rc = cmd_scan(&c);
    } else if (strcmp(cmd, "list") == 0) {
        rc = cmd_list(&c);
    } else if (strcmp(cmd, "connect") == 0) {
        rc = cmd_connect(&c, argv[arg], methods, phase2);
    } else if (strcmp(cmd, "forget") == 0) {
        rc = cmd_forget(&c, argv[arg]);
    } else {
        rc = cmd_status(&c);
    }
    ctrl_close(&c);
    fflush(stdout);
    return interrupted ? 130 : rc;
}
