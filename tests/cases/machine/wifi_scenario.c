/*
 * wifi_scenario.c: the scenario of the cases of tests/cases/machine.wifi.json
 * and machine.wifi-gnu.json: a Rach2 station joining the access points of
 * the Linux AP guest (tests/system/wifi) through its wsim radio, in each
 * security the guest offers, and keeping its addresses, routes and servers
 * right as the network around it changes.
 *
 * Each case boots the guest, `ap`, and the station, `station`, the installed
 * system or the GNU root on a disk, whose init-metal runs wpa_supplicant,
 * wpa_cli's action and ifplugd. The station's first card is the cable, which
 * the scenario unplugs through QMP at once, and plugs in only for the cable
 * row; its second, eth1 on 10.0.4.0/24, carries the radio's frames, which
 * wsimrelay sends to the guest's relay at the host's UDP port 0 of the case
 * and takes at port 1. The guest's eth0 forwards port 2 to the SSH server of
 * the station on network A, and reaches the host, the stations' endpoint
 * 10.0.2.2, where hostpeer echoes on port 3.
 *
 * Every row is judged by what the machines count and say, never by the
 * clock: the routes and servers each lease gives, the kernel's records on
 * the station's console, wpa_supplicant's events in init's service log, the
 * bytes an echo brings back, the replies a ping counts, and the guest's
 * hostapd. A shell loop
 * that waits for an event sleeps a second between looks; the step's bound
 * ends a wait whose event never comes.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The station's root prompt and the guest's. */
#define ROOT_PROMPT "# "
#define AP_PROMPT   "wifiap# "

/* The networks of the guest (tests/system/wifi/Readme.md). */
#define NET_A    "rach2-net"
#define NET_B    "rach2-other"
#define PSK_A    "rach2-net-psk"
#define PSK_B    "rach2-other-psk"
#define AP1      "02:00:00:00:00:00"
#define AP2      "02:00:00:00:01:00"
#define ADDR_A   "192.168.71.50"
#define ADDR_B   "192.168.72.50"
#define GW_A     "192.168.71.1"
#define GW_B     "192.168.72.1"
#define STATION  "02:77:73:00:00:00"
#define ENDPOINT "endpoint.wifi.test"

/* The enterprise network's accounts and its two certificates. */
#define EAP_USER   "student"
#define EAP_PASS   "rach2-wifi"
#define EAP_ANON   "anonymous@university.test"
#define CERT_SUBJ  "/C=KG/O=Rach2 University/CN=radius.university.test"
#define CERT_SHOWN "D3:DF:40:10:37:DA:9C:3B:34:5F:26:27:56:69:17:2C"
#define CERT_PIN   "d3df401037da9c3b345f26275669172c56284d928ef8055c065336624216f564"

/* The log in which init keeps wpa_supplicant's output, which wifi(1) reads. */
#define LOG "/var/log/services"

/* The bytes of the echo row, a file of the station's. */
#define ECHO_BYTES 262144

/* What one attempt shares among its steps. */
struct wifi {
    struct net_case u;
    struct net_boot ap, sta;
    struct etude_case_service *endpoint;
    unsigned sta_boots; /* the station's boots so far */
    char password[128]; /* root's, the demo password */
    char port[4][16];   /* the case's ports, as text */
};

/** Say whether `text` has `what` in it; NULL has nothing. */
static int has(const char *text, const char *what)
{
    return text != NULL && strstr(text, what) != NULL;
}

/**
 * Run `command` at the shell of `b`, give its exit status, or -1 when the
 * shell did not say, and give in `*text`, when `text` is not NULL, what the
 * console showed from the typed line to the status, which the caller frees.
 */
static long run(struct wifi *w, struct net_boot *b, const char *command, char **text)
{
    char line[480];
    char *said = NULL;
    long status;

    if (text != NULL)
        *text = NULL;
    if (strlen(command) + 40 > sizeof(line)) {
        net_fail(&w->u, "the command is too long: %.40s", command);
        return -1;
    }
    /* A command put in the background ends with its `&`, which takes no `;`. */
    snprintf(line, sizeof(line), "%s%s echo STATUS=$?; echo STATUS\"\"END", command,
             command[0] != '\0' && command[strlen(command) - 1] == '&' ? "" : ";");
    if (net_command(b, line, "STATUSEND", &said) != ETUDE_OK)
        return -1;
    status = net_number_after(said, "STATUS=");
    if (text != NULL)
        *text = said;
    else
        free(said);
    return status;
}

/** Run the command `format` makes at the station's shell, as `run` does. */
static long sta(struct wifi *w, char **text, const char *format, ...)
{
    char command[400];
    va_list args;

    va_start(args, format);
    vsnprintf(command, sizeof(command), format, args);
    va_end(args);
    return run(w, &w->sta, command, text);
}

/** Run the command `format` makes at the guest's shell, as `run` does. */
static long ap(struct wifi *w, char **text, const char *format, ...)
{
    char command[400];
    va_list args;

    va_start(args, format);
    vsnprintf(command, sizeof(command), format, args);
    va_end(args);
    return run(w, &w->ap, command, text);
}

/**
 * Type `command` at the station, answer each prompt of `qa`, pairs of a
 * prompt and its answer ended by NULL, as it comes, and give the command's
 * exit status and in `*text` the console from the typed line to it.
 */
static long dialog(struct wifi *w, const char *command, const char *const qa[], char **text)
{
    char line[400];
    uint64_t from;
    size_t i;

    *text = NULL;
    snprintf(line, sizeof(line), "%s; echo STATUS=$?; echo STATUS\"\"END", command);
    if (net_line(&w->sta, line) != ETUDE_OK)
        return -1;
    from = w->sta.at;
    for (i = 0; qa[i] != NULL && qa[i + 1] != NULL; i += 2) {
        if (net_wait(&w->sta, qa[i]) != ETUDE_OK || net_line(&w->sta, qa[i + 1]) != ETUDE_OK)
            return -1;
    }
    if (net_wait(&w->sta, "STATUSEND") != ETUDE_OK)
        return -1;
    *text = net_text(&w->sta, from, w->sta.at);
    return net_number_after(*text, "STATUS=");
}

/**
 * Wait until the station's lease of network A, or of B with `b`, is in
 * force: the lease script, which runs after the address is set, has put the
 * default route through the network's router on wlan0 and its server in
 * resolv.conf, and wpa_action has no disconnection pending, whose end may
 * yet stop the client. A rejoin of the same network keeps the lease it had.
 */
static void lease(struct wifi *w, int b)
{
    const char *gw = b ? GW_B : GW_A;

    sta(w, NULL,
        "until route -n | grep -q '^0\\.0\\.0\\.0 *%s .* wlan0$' && "
        "grep -q '^nameserver %s$' /etc/resolv.conf && "
        "! ls /var/run/wpa_action.wlan0.hold.* > /dev/null 2>&1; do sleep 1; done",
        gw, gw);
}

/** Wait until wlan0 has no default route, as once its lease is given up. */
static void unrouted(struct wifi *w)
{
    sta(w, NULL, "while route -n | grep -q '^0\\.0\\.0\\.0 .* wlan0$'; do sleep 1; done");
}

/** Say whether the kernel has recorded on the station's console, since the
 * offset `from`, that wlan0 lost its IPv4 address. */
static int dropped(struct wifi *w, uint64_t from)
{
    char *text = net_text(&w->sta, from, w->sta.at);
    int lost = has(text, "net: wlan0: IPv4 none");

    free(text);
    return lost;
}

/**
 * Wait in the station's shell until LOG has more than `n` lines with
 * `what`, and give how many it has then, or -1.
 */
static long log_after(struct wifi *w, const char *what, long n)
{
    char *text = NULL;
    long count;

    if (sta(w, NULL, "until [ $(grep -c '%s' " LOG ") -gt %ld ]; do sleep 1; done", what, n) != 0)
        return -1;
    if (sta(w, &text, "echo COUNT=$(grep -c '%s' " LOG ")", what) != 0) {
        free(text);
        return -1;
    }
    count = net_number_after(text, "COUNT=");
    free(text);
    return count;
}

/** Give how many lines LOG has, or -1. */
static long log_lines(struct wifi *w)
{
    char *text = NULL;
    long count = -1;

    if (sta(w, &text, "echo COUNT=$(wc -l < " LOG ")") >= 0)
        count = net_number_after(text, "COUNT=");
    free(text);
    return count;
}

/** Give how many lines of LOG have `what`, or -1. */
static long log_count(struct wifi *w, const char *what)
{
    char *text = NULL;
    long count = -1;

    if (sta(w, &text, "echo COUNT=$(grep -c '%s' " LOG ")", what) >= 0)
        count = net_number_after(text, "COUNT=");
    free(text);
    return count;
}

/** Give the replies of a ping of three packets of `host` from the station,
 * or -1 when ping printed no summary. */
static long pinged(struct wifi *w, const char *host)
{
    char *text = NULL;
    long n;

    sta(w, &text, "ping -c 3 %s", host);
    n = has(text, " received") ? net_number_after(text, " packets transmitted, ") : -1;
    free(text);
    return n;
}

/**
 * Send ECHO_BYTES of random bytes from the station to hostpeer's echo at
 * `host`, which reaches the host at port 3 of the case, and say whether
 * the same bytes came back.
 */
static int echoed(struct wifi *w, const char *host)
{
    char *text = NULL;
    int ok;

    sta(w, NULL, "head -c %d /dev/urandom > /tmp/echo.out; rm -f /tmp/echo.in", ECHO_BYTES);
    ok = sta(w, &text,
             "nc -w 5 %s %s < /tmp/echo.out > /tmp/echo.in; cmp /tmp/echo.out /tmp/echo.in && "
             "echo ECHO\"\"SAME",
             host, w->port[3]) == 0 &&
         has(text, "ECHOSAME");
    free(text);
    return ok;
}

/** Give wpa_supplicant's state on the station, or "" when it said none,
 * in `buf` of `size` bytes, with the BSSID in `bssid` when not NULL. */
static const char *state(struct wifi *w, char *buf, size_t size, char bssid[18])
{
    char *text = NULL;
    const char *line;

    buf[0] = '\0';
    if (bssid != NULL)
        bssid[0] = '\0';
    sta(w, &text, "wpa_cli -i wlan0 status");
    if ((line = net_find(text, "wpa_state=*")) != NULL)
        net_copy_line(line + strlen("wpa_state="), buf, size);
    if (bssid != NULL && (line = net_find(text, "bssid=*")) != NULL)
        net_copy_line(line + strlen("bssid="), bssid, 18);
    free(text);
    return buf;
}

/** Plug the station's cable in, `on`, or pull it out, through QMP. */
static void cable(struct wifi *w, int on)
{
    char args[64];
    char *reply = NULL;

    if (!net_ok(&w->u))
        return;
    snprintf(args, sizeof(args), "{\"name\": \"cable\", \"up\": %s}", on ? "true" : "false");
    if (etude_qmp(w->sta.m, "set_link", args, &reply) != ETUDE_OK)
        net_fail(&w->u, "QMP set_link of the cable: %s", reply != NULL ? reply : "no reply");
    free(reply);
}

/**
 * Boot the station as its next boot, pull its cable out, log in as root,
 * start its relay to the guest's, and wait for wpa_supplicant to answer.
 */
static void station(struct wifi *w, const char *step)
{
    char *text = NULL;

    net_step(&w->u, step);
    net_boot(&w->sta, &w->u, "station", ++w->sta_boots, NULL);
    cable(w, 0);
    net_login(&w->sta, "root", w->password, ROOT_PROMPT);
    sta(w, NULL, "ifconfig eth1 10.0.4.15 netmask 255.255.255.0 up");
    sta(w, NULL, "/sbin/wsimrelay -b 10.0.4.15 10.0.4.2 %s > /tmp/wsimrelay.log 2>&1 &",
        w->port[0]);
    if (sta(w, &text, "until grep -q relaying /tmp/wsimrelay.log; do sleep 1; done") != 0)
        net_fail(&w->u, "wsimrelay did not start: %s", text != NULL ? text : "");
    free(text);
    /* init starts wpa_supplicant once the boot's services are up. */
    sta(w, NULL, "until wpa_cli -i wlan0 ping 2>/dev/null | grep -q PONG; do sleep 1; done");
}

/**
 * Start the echo service, boot the guest and wait for its shell, start
 * network A's access points with `profile`, and `rekey` when not NULL, and
 * the guest's relay; then boot the station and start its relay.
 */
static void begin(struct wifi *w, const char *profile, const char *rekey)
{
    char *args[] = {"echo", w->port[3], "0", NULL};
    char *text = NULL;
    struct etude_termination end;
    unsigned i;

    for (i = 0; i < 4; i++)
        snprintf(w->port[i], sizeof(w->port[i]), "%u", etude_port(w->u.c, i));
    net_secret(&w->u, "demo-password", w->password, sizeof(w->password));
    if (net_step(&w->u, "serve") == ETUDE_OK &&
        etude_service(w->u.c, "endpoint", args, &w->endpoint, &end) != ETUDE_OK)
        net_fail(&w->u, "hostpeer's echo did not start");
    net_step(&w->u, "ap");
    net_boot(&w->ap, &w->u, "ap", 1, NULL);
    net_wait(&w->ap, AP_PROMPT);
    text = net_text(&w->ap, 0, w->ap.at);
    if (net_ok(&w->u) && !has(text, "wifiap: ready: wpa2 shell"))
        net_fail(&w->u, "the guest did not become ready");
    free(text);
    text = NULL;
    if (ap(w, &text, "wifiap start %s %s", profile, rekey != NULL ? rekey : "") != 0 ||
        ap(w, &text, "wifiap relay %s", w->port[1]) != 0)
        net_fail(&w->u, "the guest's access points or relay did not start");
    free(text);
    station(w, "station");
}

/** Stop the service, the machines' QEMUs and end the attempt. */
static int finish(struct wifi *w)
{
    struct etude_termination end;
    char *reply = NULL;

    if (w->sta.m != NULL && etude_qmp(w->sta.m, "quit", NULL, &reply) == ETUDE_OK)
        etude_wait_end(w->sta.m, &end);
    free(reply);
    reply = NULL;
    if (w->ap.m != NULL && etude_qmp(w->ap.m, "quit", NULL, &reply) == ETUDE_OK)
        etude_wait_end(w->ap.m, &end);
    free(reply);
    if (w->endpoint != NULL)
        etude_service_stop(w->endpoint, &end);
    return net_end(&w->u);
}

/**
 * Join network `name` with the passphrase `psk` through `wifi connect`,
 * scanning first, since the guest may have changed the network's security
 * since the last scan, and give 0 when wifi(1) said it joined, with its
 * text in `*text`.
 */
static int join_psk(struct wifi *w, const char *name, const char *psk, char **text)
{
    const char *const qa[] = {"Passphrase: ", psk, NULL};
    char command[64], said[64];

    snprintf(command, sizeof(command), "wifi connect %s", name);
    snprintf(said, sizeof(said), "wifi: connected to %s", name);
    sta(w, NULL, "wifi scan > /dev/null");
    return dialog(w, command, qa, text) == 0 && has(*text, said) ? 0 : 1;
}

/* ---- the wpa2 case (16a) ---- */

static void case_wpa2(struct wifi *w)
{
    char *text = NULL, buf[64];
    int ok;

    begin(w, "wpa2", NULL);
    net_step(&w->u, "join");
    sta(w, &text, "wifi scan");
    net_assert(&w->u, "scan", has(text, NET_A) && has(text, NET_B) && has(text, "WPA2"),
               "wifi scan did not list both networks: %s", text != NULL ? text : "");
    free(text);
    text = NULL;
    ok = join_psk(w, NET_A, PSK_A, &text) == 0;
    net_assert(&w->u, "join", ok, "wifi connect said: %s", text != NULL ? text : "");
    free(text);
    text = NULL;
    lease(w, 0);
    sta(w, &text, "route -n; cat /etc/resolv.conf");
    net_assert(&w->u, "lease",
               net_find(text, "0.0.0.0*" GW_A "*0.0.0.0*UG*600*wlan0") != NULL &&
                   has(text, "nameserver " GW_A),
               "no default route through " GW_A " at metric 600 or no server: %s",
               text != NULL ? text : "");
    free(text);
    text = NULL;

    net_step(&w->u, "traffic");
    sta(w, &text, "nslookup " ENDPOINT);
    net_assert(&w->u, "dns", has(text, "Address: 10.0.2.2"), "nslookup said: %s",
               text != NULL ? text : "");
    free(text);
    text = NULL;
    net_assert(&w->u, "ping", pinged(w, "ap.wifi.test") == 3, "ping of the access point failed");
    net_assert(&w->u, "transfer", echoed(w, ENDPOINT),
               "the echo through the access point did not bring the bytes back");

    net_step(&w->u, "tools");
    sta(w, &text, "iw dev");
    net_assert(&w->u, "iw-dev",
               has(text, "Interface wlan0") && has(text, "type managed") &&
                   has(text, "ssid " NET_A),
               "iw dev said: %s", text != NULL ? text : "");
    free(text);
    text = NULL;
    sta(w, &text, "iw phy | grep -E 'Wiphy|2412|2462|CCMP|managed'");
    net_assert(&w->u, "iw-phy",
               has(text, "Wiphy phy0") && has(text, "2412") && has(text, "CCMP") &&
                   has(text, "managed"),
               "iw phy said: %s", text != NULL ? text : "");
    free(text);
    text = NULL;
    sta(w, &text, "iw dev wlan0 scan | grep -E '^BSS|SSID: '");
    net_assert(&w->u, "iw-scan",
               net_count(text, "BSS 02:00:00:00:0") == 3 && has(text, "SSID: " NET_B),
               "iw dev wlan0 scan said: %s", text != NULL ? text : "");
    free(text);
    text = NULL;
    net_assert(&w->u, "iw-link",
               sta(w, &text, "iw dev wlan0 link") == 0 && has(text, "Connected to 02:00:00:00:0") &&
                   has(text, "SSID: " NET_A) && has(text, "signal: -") && has(text, "dtim period"),
               "iw dev wlan0 link said: %s", text != NULL ? text : "");
    free(text);
    text = NULL;
    sta(w, &text, "cat /proc/net/wireless");
    net_assert(&w->u, "wireless", net_find(text, "wlan0: 0000 *-#.") != NULL,
               "/proc/net/wireless said: %s", text != NULL ? text : "");
    free(text);
    net_assert(&w->u, "state", strcmp(state(w, buf, sizeof(buf), NULL), "COMPLETED") == 0,
               "wpa_supplicant ended at %s", buf);
}

/* ---- the security case (16b) ---- */

/**
 * Join network A, now with `profile`'s security, after forgetting it, with
 * `psk`, or none for an open network; wait for the lease, and check
 * wpa_supplicant's `key_mgmt`, and `extra`, another line of its status,
 * when not NULL, and a ping of the access point: assertion `id`.
 */
static void profile_row(struct wifi *w, const char *id, const char *profile, const char *psk,
                        const char *mgmt, const char *extra)
{
    char *text = NULL, want[64];
    long joined;

    net_step(&w->u, id);
    sta(w, NULL, "wifi forget " NET_A " > /dev/null 2>&1; true");
    if (profile != NULL && ap(w, NULL, "wifiap start %s", profile) != 0)
        net_fail(&w->u, "the guest did not start %s", profile);
    if (psk != NULL) {
        joined = join_psk(w, NET_A, psk, &text);
    } else {
        sta(w, NULL, "wifi scan > /dev/null");
        joined =
            sta(w, &text, "wifi connect " NET_A) == 0 && has(text, "connected to " NET_A) ? 0 : 1;
    }
    free(text);
    text = NULL;
    if (joined == 0)
        lease(w, 0);
    sta(w, &text, "wpa_cli -i wlan0 status");
    snprintf(want, sizeof(want), "key_mgmt=%s", mgmt);
    net_assert(&w->u, id,
               joined == 0 && has(text, "wpa_state=COMPLETED") && has(text, want) &&
                   (extra == NULL || has(text, extra)) && pinged(w, GW_A) == 3,
               "the join under %s failed: %s", profile != NULL ? profile : "sae",
               text != NULL ? text : "");
    free(text);
}

static void case_security(struct wifi *w)
{
    char *text = NULL, buf[64], before[18], after[18];
    long drops, gone;

    begin(w, "sae", NULL);
    profile_row(w, "sae", NULL, PSK_A, "SAE", "pmf=2");

    net_step(&w->u, "deauth");
    state(w, buf, sizeof(buf), before);
    gone = log_count(w, "CTRL-EVENT-DISCONNECTED");
    drops = log_count(w, "Unprotected Deauthentication frame dropped");
    sta(w, NULL, "wpa_cli -i wlan0 log_level debug > /dev/null");
    ap(w, &text, "wifiap forge-deauth " STATION);
    net_assert(&w->u, "forged", has(text, "wifiap: forge-deauth: sent "),
               "the guest sent no deauthentication: %s", text != NULL ? text : "");
    free(text);
    text = NULL;
    drops = drops < 0 ? -1 : log_after(w, "Unprotected Deauthentication frame dropped", drops);
    sta(w, NULL, "wpa_cli -i wlan0 log_level info > /dev/null");
    state(w, buf, sizeof(buf), after);
    net_assert(&w->u, "deauth-ignored",
               drops > 0 && strcmp(buf, "COMPLETED") == 0 && strcmp(before, after) == 0 &&
                   log_count(w, "CTRL-EVENT-DISCONNECTED") == gone && pinged(w, GW_A) == 3,
               "the station left or lost its access point: %s, %s before and %s after", buf, before,
               after);

    profile_row(w, "transition", "transition", PSK_A, "SAE", "pmf=1");
    profile_row(w, "tkip", "tkip", PSK_A, "WPA-PSK", "pairwise_cipher=TKIP");
    profile_row(w, "open", "open", NULL, "NONE", NULL);
}

/* ---- the enterprise case (16c) ---- */

/**
 * Run `wifi connect` with `option`, for the enterprise network, answering
 * the identity, password, eduroam's anonymous identity and no domain, up to
 * the certificate's question; then, before answering it with `answer`, look
 * whether any password could have left: assertion `quiet`, when not NULL.
 * Give the command's exit status and its text in `*text`.
 */
static long enterprise(struct wifi *w, const char *option, const char *answer, const char *quiet,
                       char **text)
{
    char line[160];
    char *log = NULL, *sta_list = NULL;
    uint64_t from;
    long before, status;

    *text = NULL;
    before = log_count(w, "CTRL-EVENT-EAP-SUCCESS");
    sta(w, NULL, "wifi scan > /dev/null");
    snprintf(line, sizeof(line), "wifi connect %s" NET_A "; echo STATUS=$?; echo STATUS\"\"END",
             option);
    if (net_line(&w->sta, line) != ETUDE_OK)
        return -1;
    from = w->sta.at;
    if (net_wait(&w->sta, "Identity: ") != ETUDE_OK || net_line(&w->sta, EAP_USER) != ETUDE_OK ||
        net_wait(&w->sta, "Password: ") != ETUDE_OK || net_line(&w->sta, EAP_PASS) != ETUDE_OK ||
        net_wait(&w->sta, "Anonymous identity") != ETUDE_OK ||
        net_line(&w->sta, EAP_ANON) != ETUDE_OK || net_wait(&w->sta, "Server domain") != ETUDE_OK ||
        net_line(&w->sta, "") != ETUDE_OK || net_wait(&w->sta, "[y/N] ") != ETUDE_OK)
        return -1;
    *text = net_text(&w->sta, from, w->sta.at);
    if (quiet != NULL) {
        /* The question is asked with wifi's own prompt in the foreground,
         * so the look is through the guest: no station of network A's
         * access points, and no success in the station's log, which wifi
         * itself reads once it is answered. */
        ap(w, &sta_list,
           "hostapd_cli -p /run/hostapd -i wlan0 all_sta; "
           "hostapd_cli -p /run/hostapd -i wlan1 all_sta");
        net_assert(&w->u, quiet,
                   has(*text, "subject: " CERT_SUBJ) && has(*text, CERT_SHOWN) &&
                       !has(sta_list, STATION),
                   "the probe showed no certificate, or a station was left on the AP: %s %s", *text,
                   sta_list != NULL ? sta_list : "");
        free(sta_list);
    }
    free(*text);
    *text = NULL;
    if (net_line(&w->sta, answer) != ETUDE_OK || net_wait(&w->sta, "STATUSEND") != ETUDE_OK)
        return -1;
    *text = net_text(&w->sta, from, w->sta.at);
    status = net_number_after(*text, "STATUS=");
    if (quiet != NULL && before >= 0) {
        sta(w, &log, "grep 'certificate chain probe' " LOG);
        net_assert(&w->u, "no-password",
                   has(log, "err='Server certificate chain probe'") &&
                       log_count(w, "CTRL-EVENT-EAP-SUCCESS") ==
                           before + (strcmp(answer, "y") == 0 ? 1 : 0),
                   "the probe was not stopped at the certificate: %s", log != NULL ? log : "");
        free(log);
    }
    return status;
}

static void case_enterprise(struct wifi *w)
{
    char *text = NULL, *conf = NULL, *who = NULL, buf[64];
    long status;

    begin(w, "enterprise", NULL);

    net_step(&w->u, "probe");
    status = enterprise(w, "", "N", "probe", &text);
    sta(w, &conf, "grep -c 'network=' /etc/wpa_supplicant/wlan0.conf; wifi list");
    net_assert(&w->u, "refused",
               status != 0 && has(text, "is not trusted, and is not kept") && !has(conf, NET_A),
               "the refused network was kept: %s %s", text != NULL ? text : "",
               conf != NULL ? conf : "");
    free(text);
    free(conf);
    text = conf = NULL;

    net_step(&w->u, "peap");
    status = enterprise(w, "", "y", NULL, &text);
    if (status == 0)
        lease(w, 0);
    sta(w, &conf,
        "wpa_cli -i wlan0 status; grep -E 'ca_cert|anonymous' "
        "/etc/wpa_supplicant/wlan0.conf");
    net_assert(&w->u, "peap",
               status == 0 && has(conf, "selectedMethod=25 (EAP-PEAP)") &&
                   has(conf, "Phase2 method=MSCHAPV2") && pinged(w, GW_A) == 3,
               "PEAP did not join: %s %s", text != NULL ? text : "", conf != NULL ? conf : "");
    net_assert(&w->u, "pinned",
               has(conf, "ca_cert=\"hash://server/sha256/" CERT_PIN "\"") &&
                   has(conf, "anonymous_identity=\"" EAP_ANON "\""),
               "the certificate or the anonymous identity was not kept: %s",
               conf != NULL ? conf : "");
    /* hostapd records each outer EAP Response-Identity with its length:
     * 5 bytes of header and the identity's 25, never the inner one's 7,
     * which travels inside the tunnel. */
    ap(w, &who, "grep -h 'EAP Response-Identity' /run/wifiap/wlan0.log /run/wifiap/wlan1.log");
    net_assert(&w->u, "anonymous",
               net_count(who, "Response-Identity (1)") > 0 &&
                   net_count(who, "Response-Identity (1)") == net_count(who, "len=30) from STA"),
               "the access point saw an outer identity of another length: %s",
               who != NULL ? who : "");
    free(text);
    free(conf);
    free(who);
    text = conf = who = NULL;

    /* The disk keeps what wpa_supplicant wrote once it is synced. */
    net_step(&w->u, "poweroff");
    if (net_line(&w->sta, "sync; poweroff -f") == ETUDE_OK) {
        struct etude_termination end;
        if (etude_wait_end(w->sta.m, &end) == ETUDE_LOST)
            net_fail(&w->u, "the station was lost as it powered off");
    }
    station(w, "reboot");
    lease(w, 0);
    sta(w, &text, "wpa_cli -i wlan0 status");
    net_assert(&w->u, "pin-kept",
               has(text, "wpa_state=COMPLETED") && has(text, "selectedMethod=25 (EAP-PEAP)") &&
                   pinged(w, GW_A) == 3,
               "the station did not join again by itself: %s", text != NULL ? text : "");
    free(text);
    text = NULL;

    net_step(&w->u, "changed");
    status = log_count(w, "Server certificate mismatch");
    ap(w, NULL, "wifiap start enterprise2");
    status = status < 0 ? -1 : log_after(w, "Server certificate mismatch", status);
    sta(w, &text, "wifi status");
    net_assert(&w->u, "changed",
               status > 0 && has(text, "not connected") && has(text, "not the one trusted"),
               "the changed certificate was not refused with its reason: %s",
               text != NULL ? text : "");
    free(text);
    text = NULL;

    net_step(&w->u, "ttls");
    ap(w, NULL, "wifiap start enterprise");
    sta(w, NULL, "wifi forget " NET_A " > /dev/null 2>&1; true");
    status = enterprise(w, "-e ttls-pap ", "y", NULL, &text);
    if (status == 0)
        lease(w, 0);
    free(text);
    text = NULL;
    sta(w, &text, "wpa_cli -i wlan0 status");
    net_assert(&w->u, "ttls",
               status == 0 && has(text, "selectedMethod=21 (EAP-TTLS)") &&
                   has(text, "Phase2 method=PAP") && pinged(w, GW_A) == 3,
               "TTLS with PAP did not join: %s", text != NULL ? text : "");
    free(text);
    net_assert(&w->u, "state", strcmp(state(w, buf, sizeof(buf), NULL), "COMPLETED") == 0,
               "wpa_supplicant ended at %s", buf);
}

/* ---- the robust case (16d) ---- */

/** Say whether the station's default routes and servers are network A's,
 * or B's with `b`, alone, with the address, and give the text. */
static int routed(struct wifi *w, int b, char **text)
{
    const char *gw = b ? GW_B : GW_A;
    char want[96];

    sta(w, text, "route -n; cat /etc/resolv.conf");
    snprintf(want, sizeof(want), "0.0.0.0*%s*0.0.0.0*UG*600*wlan0", gw);
    return net_find(*text, want) != NULL && net_count(*text, " UG ") == 1 &&
           has(*text, b ? "nameserver " GW_B : "nameserver " GW_A) &&
           !has(*text, b ? "nameserver " GW_A : "nameserver " GW_B) &&
           !has(*text, "nameserver 10.0.2.3");
}

/**
 * Wait for the station's next CTRL-EVENT-CONNECTED after `connected` of
 * them, and for its lease of network A, or B with `b`, and say whether the
 * routes, servers and an echo to the endpoint are right then: assertion
 * `id`, `why` saying what was done.
 */
static void rejoined(struct wifi *w, const char *id, long connected, int b, const char *why)
{
    char *text = NULL;
    long now;

    now = connected < 0 ? -1 : log_after(w, "CTRL-EVENT-CONNECTED", connected);
    lease(w, b);
    net_assert(&w->u, id, now > connected && routed(w, b, &text) && echoed(w, ENDPOINT),
               "after %s the station did not come back with its routes, servers and an echo: %s",
               why, text != NULL ? text : "");
    free(text);
}

/** The robust case's first steps: the join of network A, and a group key
 * the access point makes anew, which reaches the station. */
static void robust_join(struct wifi *w)
{
    char *text = NULL;
    long rekeys;
    int ok;

    begin(w, "wpa2", "5");
    net_step(&w->u, "join");
    ok = join_psk(w, NET_A, PSK_A, &text) == 0;
    free(text);
    text = NULL;
    lease(w, 0);
    net_assert(&w->u, "join", ok && routed(w, 0, &text) && echoed(w, ENDPOINT),
               "the first join failed: %s", text != NULL ? text : "");
    free(text);
    text = NULL;

    /* A group key the access point makes anew reaches the station: the
     * guest's ARP request for the station, broadcast under it, is
     * answered. */
    net_step(&w->u, "rekey");
    rekeys = log_count(w, "Group rekeying completed");
    rekeys = rekeys < 0 ? -1 : log_after(w, "Group rekeying completed", rekeys);
    ap(w, &text, "ip neigh flush dev br0; ping -c 3 " ADDR_A);
    net_assert(&w->u, "rekey",
               rekeys > 0 && net_number_after(text, " packets transmitted, ") == 3 &&
                   echoed(w, ENDPOINT),
               "after %ld rekeys the guest's broadcast or the echo failed: %s", rekeys,
               text != NULL ? text : "");
    free(text);
}

/** The robust case's rejoins: a restart of the access points, a
 * deauthentication and a rejoin inside a renewal each leave the station's
 * lease in place, and a join seen twice at once starts one DHCP client. */
static void robust_rejoins(struct wifi *w)
{
    char *text = NULL;
    long n;
    uint64_t mark;

    /* A rejoin of the same network keeps the lease: the kernel records no
     * loss of the address. */
    net_step(&w->u, "restart");
    n = log_count(w, "CTRL-EVENT-CONNECTED");
    mark = w->sta.at;
    ap(w, NULL, "wifiap start wpa2 5");
    rejoined(w, "restart", n, 0, "the access points restarted");
    net_assert(&w->u, "restart-kept", !dropped(w, mark),
               "the station gave its lease up as the access points restarted");

    net_step(&w->u, "deauth");
    n = log_count(w, "CTRL-EVENT-CONNECTED");
    mark = w->sta.at;
    ap(w, NULL,
       "hostapd_cli -p /run/hostapd -i wlan0 deauthenticate " STATION "; "
       "hostapd_cli -p /run/hostapd -i wlan1 deauthenticate " STATION);
    rejoined(w, "deauth", n, 0, "a deauthentication");
    text = net_text(&w->sta, mark, w->sta.at);
    net_assert(&w->u, "deauth-kept",
               !has(text, "net: wlan0: IPv4 none") &&
                   net_find(text, "wlan: wlan0: left: 02:00:00:00:0*: deauthenticated by the AP: "
                                  "reason #") != NULL,
               "the station gave its lease up, or logged no deauthentication: %s",
               text != NULL ? text : "");
    free(text);
    text = NULL;

    /* Two rejoins while the server cannot answer the first's renewal:
     * udhcpc gives its address up at a second SIGUSR1 inside the first's
     * wait, so the second must send none. */
    net_step(&w->u, "pending");
    mark = w->sta.at;
    ap(w, NULL, "pkill -STOP dnsmasq");
    sta(w, NULL,
        "WPA_ID=$(cat /var/run/udhcpc.wlan0.net); export WPA_ID; "
        "for i in 1 2; do /etc/wpa_supplicant/wpa_action wlan0 DISCONNECTED; "
        "/etc/wpa_supplicant/wpa_action wlan0 CONNECTED; done");
    ap(w, NULL, "pkill -CONT dnsmasq");
    sta(w, NULL, "while [ -e /var/run/udhcpc.wlan0.renew ]; do sleep 1; done");
    lease(w, 0);
    net_assert(&w->u, "pending-kept", !dropped(w, mark),
               "the station gave its lease up at a rejoin inside a renewal");

    /* With no client running, a CONNECTED and a wait that has seen the
     * join act at the same moment, a second after the disconnection, and
     * must start one client between them. */
    net_step(&w->u, "one-client");
    sta(w, &text,
        "WPA_ID=$(cat /var/run/udhcpc.wlan0.net); export WPA_ID; "
        "for i in 1 2 3; do p=$(cat /var/run/udhcpc.wlan0.pid); kill $p; "
        "while kill -0 $p 2> /dev/null; do usleep 100000; done; "
        "/etc/wpa_supplicant/wpa_action wlan0 DISCONNECTED; sleep 1; "
        "/etc/wpa_supplicant/wpa_action wlan0 CONNECTED; sleep 2; "
        "echo CLIENTS=$(ps | grep -c '[u]dhcpc -f -R'); done");
    lease(w, 0);
    n = net_count(text, "CLIENTS=1");
    net_assert(&w->u, "one-client", n == 3, "a join started two clients: %s",
               text != NULL ? text : "");
    free(text);
}

/** The robust case's channel switch of both access points of network A,
 * which the station follows with its lease and traffic. */
static void robust_switch(struct wifi *w)
{
    char *text = NULL, *console = NULL, *routes = NULL;
    long n, left;
    uint64_t mark;

    /* Both access points of network A move, to channels 3 and 8, each
     * announcing it five beacons ahead with transmit held, while a ping
     * runs: the station follows its own without leaving it, and its lease
     * and traffic go on. */
    net_step(&w->u, "switch");
    n = log_count(w, "CTRL-EVENT-CHANNEL-SWITCH");
    left = log_count(w, "CTRL-EVENT-DISCONNECTED");
    mark = w->sta.at;
    sta(w, NULL, "ping -c 10 " GW_A " > /tmp/switch.ping 2>&1 &");
    sta(w, NULL, "echo $! > /tmp/switch.pid");
    ap(w, NULL,
       "hostapd_cli -p /run/hostapd -i wlan0 chan_switch 5 2422 ht blocktx; "
       "hostapd_cli -p /run/hostapd -i wlan1 chan_switch 5 2447 ht blocktx");
    n = n < 0 ? -1 : log_after(w, "CTRL-EVENT-CHANNEL-SWITCH", n);
    /* The shell's wait would wait for wsimrelay too. BusyBox's ping ends with
     * no totals on a failed send, and the file then says why. */
    sta(w, &text,
        "until grep -q 'packets transmitted' /tmp/switch.ping || "
        "[ ! -d /proc/$(cat /tmp/switch.pid) ]; do sleep 1; done; "
        "cat /tmp/switch.ping; grep CTRL-EVENT-CHANNEL-SWITCH " LOG " | tail -1");
    lease(w, 0);
    console = net_text(&w->sta, mark, w->sta.at);
    net_assert(&w->u, "switch",
               n > 0 && log_count(w, "CTRL-EVENT-DISCONNECTED") == left &&
                   (has(text, "freq=2422") || has(text, "freq=2447")) &&
                   net_number_after(text, " packets transmitted, ") > 0 &&
                   net_find(console, "wlan: wlan0: switched: 02:00:00:00:0*: to channel #") !=
                       NULL &&
                   !has(console, "wlan: wlan0: left:") && !has(console, "net: wlan0: IPv4 none") &&
                   routed(w, 0, &routes) && echoed(w, ENDPOINT),
               "the station did not follow its access point's channel switch with its lease and "
               "traffic: %s %s",
               text != NULL ? text : "", console != NULL ? console : "");
    free(text);
    free(console);
    free(routes);
}

/** The robust case's loss of network A, its return, and a roam between
 * its access points under an SSH session. */
static void robust_roam(struct wifi *w)
{
    char *text = NULL, buf[64], from[18], to[18];
    long n;
    int ok;

    net_step(&w->u, "gone");
    n = log_count(w, "CTRL-EVENT-DISCONNECTED");
    ap(w, NULL, "wifiap stop a");
    n = n < 0 ? -1 : log_after(w, "CTRL-EVENT-DISCONNECTED", n);
    unrouted(w);
    sta(w, &text, "route -n");
    net_assert(&w->u, "gone", n > 0 && !has(text, "wlan0"),
               "the station kept a route on network A once it was gone: %s",
               text != NULL ? text : "");
    free(text);
    text = NULL;
    n = log_count(w, "CTRL-EVENT-CONNECTED");
    ap(w, NULL, "wifiap start wpa2 5");
    rejoined(w, "back", n, 0, "network A came back");

    /* An SSH session from the station to its own server, both ends over
     * Wi-Fi through the guest's forward of port 2, carries a line before
     * and after a roam to the other access point of network A. */
    net_step(&w->u, "roam");
    state(w, buf, sizeof(buf), from);
    sta(w, NULL, "rm -f /root/.ssh/wifi_key /tmp/ssh.in /tmp/ssh.out; mkfifo /tmp/ssh.in");
    sta(w, NULL,
        "dropbearmulti dropbearkey -t ed25519 -f /root/.ssh/wifi_key | "
        "grep ^ssh- >> /root/.ssh/authorized_keys");
    sta(w, NULL,
        "dropbearmulti dbclient -y -i /root/.ssh/wifi_key -p %s root@10.0.2.2 cat "
        "< /tmp/ssh.in > /tmp/ssh.out 2> /tmp/ssh.err &",
        w->port[2]);
    sta(w, NULL, "exec 3> /tmp/ssh.in; echo before >&3");
    ok = sta(w, NULL, "until grep -q before /tmp/ssh.out; do sleep 1; done") == 0;
    n = log_count(w, "CTRL-EVENT-CONNECTED");
    sta(w, NULL, "wpa_cli -i wlan0 roam %s", strcmp(from, AP1) == 0 ? AP2 : AP1);
    n = n < 0 ? -1 : log_after(w, "CTRL-EVENT-CONNECTED", n);
    state(w, buf, sizeof(buf), to);
    sta(w, NULL, "echo after >&3");
    ok = ok && sta(w, &text,
                   "until grep -q after /tmp/ssh.out; do sleep 1; done; "
                   "cat /tmp/ssh.out; exec 3>&-") == 0;
    net_assert(&w->u, "roam",
               ok && n > 0 && strcmp(buf, "COMPLETED") == 0 && strcmp(from, to) != 0 &&
                   has(text, "before") && has(text, "after"),
               "the roam from %s to %s lost the session or the lease: %s", from, to,
               text != NULL ? text : "");
    free(text);
}

/** The robust case's move to network B and back, and a cable plugged in
 * and out beside Wi-Fi. */
static void robust_move(struct wifi *w)
{
    char *text = NULL;
    long n;
    int ok;

    /* Network B is known once joined; with A's access points gone, the
     * station moves back to it by itself, and to A when B goes. */
    net_step(&w->u, "move");
    ok = join_psk(w, NET_B, PSK_B, &text) == 0;
    free(text);
    text = NULL;
    lease(w, 1);
    net_assert(&w->u, "joined-b", ok && routed(w, 1, &text) && echoed(w, ENDPOINT),
               "the join of network B failed: %s", text != NULL ? text : "");
    free(text);
    text = NULL;
    n = log_count(w, "CTRL-EVENT-CONNECTED");
    ap(w, NULL, "wifiap stop b");
    rejoined(w, "moved", n, 0, "network B went, with A known");

    net_step(&w->u, "cable");
    cable(w, 1);
    /* The lease script sets the route, then resolv.script the servers. */
    ok = sta(w, &text,
             "until route -n | grep -q '^0.0.0.0 .*UG .*100 .*eth0' && "
             "grep -q '^nameserver 10.0.2.3$' /etc/resolv.conf; do sleep 1; done; "
             "route -n; cat /etc/resolv.conf") == 0;
    net_assert(&w->u, "cable-in",
               ok && net_find(text, "0.0.0.0*" GW_A "*UG*600*wlan0") != NULL &&
                   has(text, "nameserver 10.0.2.3") && echoed(w, "10.0.2.2"),
               "the cable did not take the default route and the first server: %s",
               text != NULL ? text : "");
    free(text);
    text = NULL;
    cable(w, 0);
    ok = sta(w, &text,
             "while route -n | grep -q eth0 || grep -q '^nameserver 10.0.2.3$' /etc/resolv.conf; "
             "do sleep 1; done") == 0;
    free(text);
    text = NULL;
    net_assert(&w->u, "cable-out", ok && routed(w, 0, &text) && echoed(w, ENDPOINT),
               "with the cable out the routes and servers were not Wi-Fi's alone: %s",
               text != NULL ? text : "");
    free(text);
}

/** The robust case: one session through rekeys, restarts, a channel switch,
 * a roam, a move between networks and a cable. */
static void case_robust(struct wifi *w)
{
    robust_join(w);
    robust_rejoins(w);
    robust_switch(w);
    robust_roam(w);
    robust_move(w);
}

/* ---- the rfkill case (16e) ---- */

/* The most lines the block may add to LOG: wpa_supplicant's reports of the
 * block, the disconnection and the disabled interface, and wpa_action's and
 * udhcpc's as the lease is given up, 11 in all on wsim. A radio left
 * scanning adds a failed scan a second. */
#define BLOCK_LINES 20

/**
 * Block the radio, unblock it, and see the station join again by itself.
 * The block closes wlan0, as Linux's does, and wpa_supplicant, seeing the
 * interface go down, says it is disabled and stops scanning; at the
 * release it brings the interface up and joins.
 */
static void case_rfkill(struct wifi *w)
{
    char *text = NULL, buf[64];
    long n, lines, scans, added;
    int ok;

    begin(w, "wpa2", NULL);
    net_step(&w->u, "join");
    ok = join_psk(w, NET_A, PSK_A, &text) == 0;
    free(text);
    text = NULL;
    lease(w, 0);
    net_assert(&w->u, "join", ok && pinged(w, GW_A) == 3, "the join failed");

    net_step(&w->u, "block");
    n = log_count(w, "CTRL-EVENT-DISCONNECTED");
    scans = log_count(w, "CTRL-EVENT-SCAN-FAILED");
    lines = log_lines(w);
    sta(w, NULL, "rfkill block wifi");
    n = n < 0 ? -1 : log_after(w, "CTRL-EVENT-DISCONNECTED", n);
    unrouted(w);
    sta(w, NULL,
        "until wpa_cli -i wlan0 status | grep -q '^wpa_state=INTERFACE_DISABLED$'; do sleep 1; "
        "done");
    sta(w, &text, "rfkill list; iw dev wlan0 link; route -n; cat /sys/class/net/wlan0/operstate");
    net_assert(&w->u, "blocked",
               n > 0 && has(text, "Soft blocked: yes") && has(text, "Not connected") &&
                   net_count(text, "0.0.0.0*wlan0") == 0 && has(text, "\ndown"),
               "the block left the radio on: %s", text != NULL ? text : "");
    free(text);
    text = NULL;
    added = lines < 0 ? -1 : log_lines(w) - lines;
    net_assert(&w->u, "quiet",
               added >= 0 && added <= BLOCK_LINES && scans >= 0 &&
                   log_count(w, "CTRL-EVENT-SCAN-FAILED") == scans &&
                   strcmp(state(w, buf, sizeof(buf), NULL), "INTERFACE_DISABLED") == 0,
               "the block added %ld lines to the log, or a failed scan", added);

    net_step(&w->u, "unblock");
    n = log_count(w, "CTRL-EVENT-CONNECTED");
    sta(w, NULL, "rfkill unblock wifi");
    rejoined(w, "unblocked", n, 0, "rfkill unblock");
}

/* ---- the GNU root's case (16f) ---- */

static void case_gnu(struct wifi *w)
{
    begin(w, "wpa2", NULL);
    profile_row(w, "wpa2", NULL, PSK_A, "WPA2-PSK", "pairwise_cipher=CCMP");
    profile_row(w, "sae", "sae", PSK_A, "SAE", "pmf=2");
    {
        char *text = NULL;
        long status;

        net_step(&w->u, "peap");
        ap(w, NULL, "wifiap start enterprise");
        sta(w, NULL, "wifi forget " NET_A " > /dev/null 2>&1; true");
        status = enterprise(w, "", "y", NULL, &text);
        if (status == 0)
            lease(w, 0);
        free(text);
        text = NULL;
        sta(w, &text, "wpa_cli -i wlan0 status");
        net_assert(&w->u, "peap",
                   status == 0 && has(text, "selectedMethod=25 (EAP-PEAP)") && pinged(w, GW_A) == 3,
                   "PEAP did not join: %s", text != NULL ? text : "");
        free(text);
    }
}

/**
 * Run the case the attempt names: one of the five of machine.wifi.json or
 * the one of machine.wifi-gnu.json.
 */
int etude_scenario(struct etude_case *c)
{
    static struct wifi w;
    const char *id = etude_case_id(c);

    memset(&w, 0, sizeof(w));
    net_case(&w.u, c);
    if (strcmp(id, "machine.wifi.wpa2") == 0)
        case_wpa2(&w);
    else if (strcmp(id, "machine.wifi.security") == 0)
        case_security(&w);
    else if (strcmp(id, "machine.wifi.enterprise") == 0)
        case_enterprise(&w);
    else if (strcmp(id, "machine.wifi.robust") == 0)
        case_robust(&w);
    else if (strcmp(id, "machine.wifi.rfkill") == 0)
        case_rfkill(&w);
    else if (strcmp(id, "machine.wifi-gnu.joins") == 0)
        case_gnu(&w);
    else
        net_fail(&w.u, "no such case: %s", id);
    return finish(&w);
}
