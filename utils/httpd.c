/**
 * Minimal HTTP/1.0 server for Rach2.
 *
 * Serves a status page over the from-scratch TCP/IP stack. The page is a
 * diagnostic readout: it reports the parts of the request the stack actually
 * decoded (the client's `IP:port` from `accept()`, the request line and
 * User-Agent from `recv()`) alongside uptime and per-connection counters, so a
 * reload visibly proves a full SYN, ESTABLISHED, data, FIN, TIME_WAIT cycle
 * round-tripped. Anything reflected from the request is HTML-escaped.
 *
 * It binds port 80 (reachable at the SLIRP address `10.0.2.15:80`) and uses
 * only the blocking socket calls (`socket`, `bind`, `listen`, `accept`, `recv`,
 * `send`, `close`).
 *
 * Test from the host (with QEMU hostfwd tcp::8080-:80):
 *   curl http://127.0.0.1:8080/
 */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

#define LISTEN_PORT 80

/* The stack's static SLIRP address; shown as the server endpoint. */
#define SERVER_ADDR "10.0.2.15:80"

#define PEER_RING    6   /* last-N connections shown on the page */
#define REQLINE_MAX 64   /* stored length of a request line  */
#define METHPATH_MAX 34  /* stored "METHOD /path" for the log */
#define AGENT_MAX   76   /* stored User-Agent length         */
#define HOST_MAX    48   /* stored Host header length        */

/* Thin libc wrappers so the rest of the server reads as it did freestanding. */
static int sys_write(int fd, const void *b, size_t n) { return (int)write(fd, b, n); }
static int sys_close(int fd) { return close(fd); }
static void sys_exit(int s) { exit(s); }
static int sys_gettimeofday(struct timeval *tv) { return gettimeofday(tv, 0); }
static int sys_send(int fd, const void *b, size_t n) { return (int)send(fd, b, n, 0); }
static int sys_recv(int fd, void *b, size_t n) { return (int)recv(fd, b, n, 0); }

static void print(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

static void print_uint(unsigned int n)
{
    char t[12];
    char *p = t + 11;
    *p = '\0';
    if (n == 0) { print("0"); return; }
    while (n > 0) { *--p = (char)('0' + n % 10); n /= 10; }
    print(p);
}

static void print_ip(uint32_t a)
{
    uint8_t *b = (uint8_t *)&a;
    print_uint(b[0]); print("."); print_uint(b[1]); print(".");
    print_uint(b[2]); print("."); print_uint(b[3]);
}

static void print_port(uint16_t p)
{
    uint8_t *b = (uint8_t *)&p;
    print_uint((unsigned int)b[0] * 256 + (unsigned int)b[1]);
}

/**
 * Print `ip:port` (network byte order) to the log.
 */
static void log_addr(uint32_t ip, uint16_t port)
{
    print_ip(ip); print(":"); print_port(port);
}

/* Buffer-append helpers (no libc). */

/**
 * A bounded output cursor over a fixed buffer. Every byte the page is built
 * from is stored through `put_c`, which refuses to write at or past `cap`, so
 * a maximal, attacker-shaped request truncates the page instead of overflowing
 * the buffer. The reflected request fields are individually capped, so a normal
 * page fits with room to spare; the bound only ever clips a crafted worst case.
 */
struct out {
    char *buf;
    int   pos;
    int   cap;
};

static void put_c(struct out *o, char c)
{
    if (o->pos < o->cap - 1) o->buf[o->pos++] = c;
}

static void put_s(struct out *o, const char *s)
{
    while (*s) put_c(o, *s++);
}

static void put_u(struct out *o, unsigned int v)
{
    char t[12];
    int i = 0;
    if (v == 0) { put_c(o, '0'); return; }
    while (v > 0) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) put_c(o, t[--i]);
}

static void put_ip(struct out *o, uint32_t sin_addr)
{
    uint8_t *b = (uint8_t *)&sin_addr;
    put_u(o, b[0]); put_c(o, '.');
    put_u(o, b[1]); put_c(o, '.');
    put_u(o, b[2]); put_c(o, '.');
    put_u(o, b[3]);
}

static void put_port(struct out *o, uint16_t sin_port)
{
    uint8_t *b = (uint8_t *)&sin_port;
    put_u(o, (unsigned int)b[0] * 256u + (unsigned int)b[1]);
}

/**
 * Append `s` with the four HTML-significant bytes escaped, copying at most
 * `max` source bytes and dropping anything outside printable ASCII.
 *
 * Every value taken from the request passes through here before it reaches the
 * page.
 */
static void put_esc(struct out *o, const char *s, int max)
{
    int i;
    for (i = 0; i < max && s[i]; i++) {
        char c = s[i];
        if      (c == '<') put_s(o, "&lt;");
        else if (c == '>') put_s(o, "&gt;");
        else if (c == '&') put_s(o, "&amp;");
        else if (c == '"') put_s(o, "&quot;");
        else if (c >= 32 && c < 127) put_c(o, c);
    }
}

/**
 * Append a request string with its leading method token highlighted in red
 * (the `GET` in `GET /path HTTP/1.1`); both parts are HTML-escaped.
 */
static void put_req(struct out *o, const char *s, int max)
{
    int m = 0;
    while (m < max && s[m] && s[m] != ' ') m++;
    put_s(o, "<em>");
    put_esc(o, s, m);
    put_s(o, "</em>");
    put_esc(o, s + m, max - m);
}

/* Request parsing (operates on the raw bytes recv() delivered). */

static int str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/**
 * Case-insensitive test of whether `buf` begins with the lowercase `lower`.
 */
static int ci_prefix(const char *buf, const char *lower)
{
    int i;
    for (i = 0; lower[i]; i++) {
        char c = buf[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return 0;
    }
    return 1;
}

/**
 * Locate a header value by lowercase `"name:"` and copy it into `dst`, NUL
 * terminated and capped at `cap`. Returns 1 if found, 0 otherwise.
 */
static int header_value(const char *buf, int len, const char *name,
                        char *dst, int cap)
{
    int nlen = str_len(name);
    int i = 0;

    while (i < len) {
        int eol = i;
        while (eol < len && buf[eol] != '\r' && buf[eol] != '\n') eol++;

        if (eol - i >= nlen && ci_prefix(buf + i, name)) {
            int v = i + nlen, n = 0;
            while (v < eol && (buf[v] == ' ' || buf[v] == '\t')) v++;
            while (v < eol && n < cap - 1) dst[n++] = buf[v++];
            dst[n] = '\0';
            return 1;
        }

        i = eol;
        while (i < len && (buf[i] == '\r' || buf[i] == '\n')) i++;
    }
    return 0;
}

/**
 * Copy the request line into `dst`, or just `METHOD path` when `methpath` is
 * set; printable ASCII only, capped at `cap`.
 */
static void copy_request(const char *buf, int len, char *dst, int cap,
                         int methpath)
{
    int i = 0, n = 0, spaces = 0;
    while (i < len && n < cap - 1 && buf[i] != '\r' && buf[i] != '\n') {
        char ch = buf[i++];
        if (ch == ' ' && methpath && ++spaces == 2) break;
        if (ch >= 32 && ch < 127) dst[n++] = ch;
    }
    dst[n] = '\0';
}

/* Live state. */

struct peer_rec {
    uint32_t ip;        /* network byte order */
    uint16_t port;      /* network byte order */
    uint32_t req_num;
    uint32_t ts_sec;
    unsigned int bytes;
    char     methpath[METHPATH_MAX];
};

static unsigned int served = 0;
static unsigned int total_bytes = 0;
static int32_t      epoch_sec_start = 0;
static struct peer_rec peers[PEER_RING];
static int          peer_head = 0;
static int          peer_count = 0;

/* The request currently being served (filled in main, read by build_body). */
static uint32_t cur_client_ip   = 0;
static uint16_t cur_client_port = 0;
static unsigned int cur_bytes_in = 0;
static char     cur_reqline[REQLINE_MAX];
static char     cur_methpath[METHPATH_MAX];
static char     cur_agent[AGENT_MAX];
static char     cur_host[HOST_MAX];

static char body[4096];
static char resp[6144];
static char reqbuf[2048];

static int32_t now_sec(void)
{
    struct timeval tv;
    sys_gettimeofday(&tv);
    return tv.tv_sec;
}

/**
 * Push a connection onto the `peers` ring, overwriting the oldest slot once it
 * is full. The `methpath` is copied from the current request.
 */
static void record_peer(uint32_t ip, uint16_t port,
                        unsigned int req_num, unsigned int bytes)
{
    int i;
    peers[peer_head].ip      = ip;
    peers[peer_head].port    = port;
    peers[peer_head].req_num = req_num;
    peers[peer_head].ts_sec  = (uint32_t)now_sec();
    peers[peer_head].bytes   = bytes;
    for (i = 0; i < METHPATH_MAX; i++) peers[peer_head].methpath[i] = cur_methpath[i];
    peer_head = (peer_head + 1) % PEER_RING;
    if (peer_count < PEER_RING) peer_count++;
}

/**
 * Append uptime as `Hh MMm SSs`.
 */
static void put_uptime(struct out *o, uint32_t s)
{
    uint32_t h = s / 3600u, m = (s / 60u) % 60u, sec = s % 60u;
    put_u(o, h);   put_c(o, 'h'); put_c(o, ' ');
    if (m < 10) put_c(o, '0');
    put_u(o, m);   put_c(o, 'm'); put_c(o, ' ');
    if (sec < 10) put_c(o, '0');
    put_u(o, sec); put_c(o, 's');
}

/**
 * Append a byte count as `N B` or `N.N KB`.
 */
static void put_bytes(struct out *o, unsigned int n)
{
    if (n < 1024) {
        put_u(o, n);
        put_s(o, " B");
    } else {
        put_u(o, n / 1024u);
        put_c(o, '.');
        put_u(o, ((n % 1024u) * 10u) / 1024u);
        put_s(o, " KB");
    }
}

/**
 * Build the live status page into `body[]`; returns its length.
 */
static int build_body(uint32_t uptime_s)
{
    struct out o = { body, 0, (int)sizeof(body) };
    int i, idx;
    uint32_t now;

    put_s(&o,
        "<!DOCTYPE html><html lang=en><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>rach2</title><style>"
        ":root{--bg:#ffffff;--ink:#1a1a1a;--dim:#5e5e5e;--faint:#8c8c8c;"
        "--line:#e7e7e7;--red:#871710}"
        "@media(prefers-color-scheme:dark){:root{--bg:#0a0a0a;--ink:#e8e8e6;"
        "--dim:#8a8a88;--faint:#585856;--line:#1d1d1d;--red:#cf4031}}"
        "*{box-sizing:border-box}"
        "body{margin:0;background:var(--bg);color:var(--ink);"
        "font:14px/1.7 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;"
        "padding:9vh 22px 14vh;display:flex;justify-content:center}"
        "main{width:100%;max-width:600px}"
        "a{color:inherit}"
        ".top{display:flex;justify-content:space-between;align-items:flex-end;"
        "gap:18px;border-bottom:1px solid var(--line);padding-bottom:15px}"
        ".top h1{margin:0;font-size:23px;font-weight:600;letter-spacing:.01em;"
        "color:var(--red)}"
        ".stat{text-align:right;color:var(--faint);font-size:12px;"
        "line-height:1.55;white-space:nowrap}"
        "em{font-style:normal;color:var(--red)}"
        ".sec{color:var(--faint);font-size:11px;letter-spacing:.17em;"
        "text-transform:uppercase;margin:30px 0 11px}"
        ".r{display:grid;grid-template-columns:78px 1fr;gap:14px;padding:2px 0}"
        ".r i{color:var(--dim);font-style:normal}"
        ".r b{color:var(--ink);font-weight:400;word-break:break-all}"
        "table{width:100%;border-collapse:collapse;font-size:13px}"
        "td{padding:6px 0;border-top:1px solid var(--line);color:var(--dim);"
        "white-space:nowrap;vertical-align:top}"
        "td.n{color:var(--faint);padding-right:16px}"
        "td.p{color:var(--ink);padding-right:18px}"
        "td.m{color:var(--dim);width:99%;white-space:normal;word-break:break-all;"
        "padding-right:14px}"
        "td.a{text-align:right;color:var(--faint);padding-left:14px}"
        "</style></head><body><main>");

    put_s(&o, "<div class=top><h1>rach2</h1><div class=stat>"
                    SERVER_ADDR "<br>up ");
    put_uptime(&o, uptime_s);
    put_s(&o, "</div></div>");

    put_s(&o, "<div class=sec>this request</div>");

    put_s(&o, "<div class=r><i>client</i><b>");
    put_ip(&o, cur_client_ip);
    put_c(&o, ':');
    put_port(&o, cur_client_port);
    put_s(&o, "</b></div>");

    if (cur_host[0]) {
        put_s(&o, "<div class=r><i>host</i><b>");
        put_esc(&o, cur_host, HOST_MAX);
        put_s(&o, "</b></div>");
    }

    put_s(&o, "<div class=r><i>request</i><b>");
    put_req(&o, cur_reqline, REQLINE_MAX);
    put_s(&o, "</b></div>");

    if (cur_agent[0]) {
        put_s(&o, "<div class=r><i>agent</i><b>");
        put_esc(&o, cur_agent, AGENT_MAX);
        put_s(&o, "</b></div>");
    }

    put_s(&o, "<div class=r><i>bytes in</i><b>");
    put_u(&o, cur_bytes_in);
    put_s(&o, "</b></div>");

    put_s(&o, "<div class=sec>traffic</div>");

    put_s(&o, "<div class=r><i>served</i><b>");
    put_u(&o, served);
    put_s(&o, served == 1 ? " request" : " requests");
    put_s(&o, "</b></div>");

    put_s(&o, "<div class=r><i>sent</i><b>");
    put_bytes(&o, total_bytes);
    put_s(&o, "</b></div>");

    /* Recent connections: source ports vary per connection, so this doubles as
     * proof the stack opens, serves and reclaims many distinct TCBs. */
    put_s(&o, "<div class=sec>recent connections</div><table>");
    now = (uint32_t)now_sec();
    for (i = 0; i < peer_count; i++) {
        idx = (peer_head - peer_count + i + PEER_RING) % PEER_RING;
        put_s(&o, "<tr><td class=n>");
        put_u(&o, peers[idx].req_num);
        put_s(&o, "</td><td class=p>");
        put_ip(&o, peers[idx].ip);
        put_c(&o, ':');
        put_port(&o, peers[idx].port);
        put_s(&o, "</td><td class=m>");
        put_req(&o, peers[idx].methpath, METHPATH_MAX);
        put_s(&o, "</td><td class=a>");
        if (i == peer_count - 1) {
            /* The newest row is the request being served right now; its size is
             * not known until the reply finishes (it is this very page). */
            put_s(&o, "<em>live</em>");
        } else {
            put_bytes(&o, peers[idx].bytes);
        }
        put_s(&o, "</td><td class=a>");
        put_u(&o, now - peers[idx].ts_sec);
        put_s(&o, "s</td></tr>");
    }
    if (peer_count == 0) {
        put_s(&o, "<tr><td colspan=5 style=color:var(--faint)>"
                        "awaiting the first connection</td></tr>");
    }
    put_s(&o, "</table>");

    put_s(&o, "</main></body></html>");
    return o.pos;
}

/**
 * Build the full HTTP/1.0 response (headers plus body) into `resp[]`; returns
 * its length.
 */
static int build_response(uint32_t uptime_s)
{
    int blen = build_body(uptime_s);
    struct out o = { resp, 0, (int)sizeof(resp) };
    int i;

    put_s(&o, "HTTP/1.0 200 OK\r\n");
    put_s(&o, "Content-Type: text/html; charset=utf-8\r\n");
    put_s(&o, "Connection: close\r\n");
    put_s(&o, "Cache-Control: no-store\r\n");
    put_s(&o, "Content-Length: ");
    put_u(&o, (unsigned int)blen);
    put_s(&o, "\r\n\r\n");
    for (i = 0; i < blen; i++) put_c(&o, body[i]);
    return o.pos;
}

/**
 * Bind port 80, then accept and serve one connection at a time: read the
 * request, decode it into the `cur_*` readout, render and send the status
 * page, then close and access-log the connection.
 */
int main(void)
{
    int fd, conn;
    struct sockaddr_in addr, peer;
    socklen_t peerlen;
    int i;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { print("httpd: error: socket() failed\n"); sys_exit(1); }

    addr.sin_family = AF_INET;
    addr.sin_port   = htons(LISTEN_PORT);
    addr.sin_addr.s_addr = 0;
    for (i = 0; i < 8; i++) addr.sin_zero[i] = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        print("httpd: error: cannot bind port 80 (already in use?)\n");
        sys_exit(1);
    }

    if (listen(fd, 4) < 0) {
        print("httpd: error: listen() failed\n"); sys_exit(1);
    }

    epoch_sec_start = now_sec();
    print("httpd: listening on " SERVER_ADDR "\n");

    for (;;) {
        int rlen, total, sent, n;
        uint32_t uptime_s;

        peerlen = sizeof peer;
        conn = accept(fd, (struct sockaddr *)&peer, &peerlen);
        if (conn < 0) { print("httpd: warn: accept() failed\n"); continue; }

        rlen = sys_recv(conn, reqbuf, sizeof(reqbuf));
        if (rlen <= 0) {
            print("httpd: warn: ");
            log_addr(peer.sin_addr.s_addr, peer.sin_port);
            print(" closed before sending a request\n");
            sys_close(conn);
            continue;
        }

        cur_client_ip   = peer.sin_addr.s_addr;
        cur_client_port = peer.sin_port;
        cur_bytes_in    = (unsigned int)rlen;
        copy_request(reqbuf, rlen, cur_reqline, REQLINE_MAX, 0);
        copy_request(reqbuf, rlen, cur_methpath, METHPATH_MAX, 1);
        if (!header_value(reqbuf, rlen, "user-agent:", cur_agent, AGENT_MAX)) {
            cur_agent[0] = '\0';
        }
        if (!header_value(reqbuf, rlen, "host:", cur_host, HOST_MAX)) {
            cur_host[0] = '\0';
        }

        served++;
        uptime_s = (uint32_t)(now_sec() - epoch_sec_start);

        /* Log the connection before rendering so it appears in its own page.
         * The response size is not known mid-render (it is the size of the very
         * reply being built), so it is recorded as 0 now and patched in once
         * the reply has been sent; the in-flight row shows a blank byte
         * count. */
        record_peer(peer.sin_addr.s_addr, peer.sin_port, served, 0);

        total = build_response(uptime_s);

        sent = 0;
        while (sent < total) {
            n = sys_send(conn, resp + sent, (size_t)(total - sent));
            if (n <= 0) break;
            sent += n;
        }
        sys_close(conn);

        total_bytes += (unsigned int)sent;
        peers[(peer_head + PEER_RING - 1) % PEER_RING].bytes = (unsigned int)sent;

        /*
         * Access log, Common Log Format style:
         *   <client> "<request line>" <status> <bytes sent>
         * A truncated reply (the client went away mid-send) is flagged at warn
         * level and shows how far it got.
         */
        print("httpd: ");
        if (sent < total) print("warn: ");
        log_addr(peer.sin_addr.s_addr, peer.sin_port);
        print(" \"");
        print(cur_reqline);
        print("\" 200 ");
        print_uint((unsigned int)sent);
        if (sent < total) {
            print("/");
            print_uint((unsigned int)total);
            print(" (short write, client closed)");
        }
        print("\n");
    }
}
