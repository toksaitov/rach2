/**
 * Test wifi, utils/wifi.c, against a fake wpa_supplicant on its control
 * socket.
 *
 * host-deps: tests/host/compat/fixdir_host.c
 *
 * The fake binds `wlan0` in the test's directory, in a child of its own,
 * and answers the requests wifi sends as wpa_supplicant does, from a table
 * of networks it keeps, with the events wpa_supplicant sends to an attached
 * monitor: the scan's results, a connection, a refused passphrase, and an
 * enterprise network's server certificate, which it presents at depth 0
 * with its SHA-256 and then refuses, as wpa_supplicant does for
 * `ca_cert="probe://"`, accepts when the network pins that hash and has a
 * password, and refuses as a mismatch when the network pins another. It
 * writes each request it takes, one to a line, to `requests`, so that a row
 * checks what was sent and in what order, and writes `VIOLATION` there when
 * a network is probed with a password set.
 *
 * The rows: the scan, listed once per network at its strongest access
 * point, the strongest first; a personal network joined, written with
 * bgscan and saved, and the other networks enabled again; a refused
 * passphrase, whose network is not kept; an enterprise network probed and
 * pinned on yes, with no password sent before the yes; one refused on no,
 * removed, and given no password; a pinned network whose server presents
 * another certificate, which is refused and kept, and whose reason `wifi
 * status` finds in the log through the EAP failure that follows it, and a
 * later attempt's refused password; the known networks listed with a pin;
 * a network forgotten; and the decoding of a name.
 *
 * The test runs in its directory, so that the sockets' relative paths fit
 * sun_path. Each message stays under 2048 bytes, macOS's largest datagram on
 * such a socket. The test exits with the number of failed checks, at most
 * 120.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fixdir_host.h"

#define WIFI_CTRL_DIR   "."
#define WIFI_CLIENT_DIR "."
#define WIFI_LOG        "services"
#define main            wifi_main
#include "wifi.c"
#undef main

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

/* The server's certificates: the one a network pins, and another. */
#define HASH1 "5a1bc1296205e6fdbe3979728efe3920798885c1c4590b5f90f43222d239ca6a"
#define HASH2 "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

/* A network of the fake's: its SSID in hex, its key management, `ca_cert`
 * and `psk` as they were set, and whether a password was. */
struct fnet {
    int used;
    char ssid[80];
    char key_mgmt[64];
    char ca_cert[128];
    char psk[80];
    int password;
};

/* The fake's state: its networks, the certificate its server presents, the
 * network the station is joined to, and the attached monitor. */
static struct fnet fnets[8];
static const char *fake_cert;
static int fake_current;
static struct sockaddr_un fake_mon;
static socklen_t fake_monlen;
static FILE *fake_log;

/** Send `text` to the attached monitor as an event of level 3. */
static void fake_event(int fd, const char *text)
{
    char msg[2048];

    if (fake_monlen == 0) {
        return;
    }
    snprintf(msg, sizeof(msg), "<3>%s", text);
    (void)sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&fake_mon, fake_monlen);
}

/** Decode the hex `h` into the text `out`. */
static void unhex(const char *h, char *out, size_t size)
{
    size_t n = 0;

    while (h[0] && h[1] && n + 1 < size) {
        out[n++] = (char)(hexval(h[0]) << 4 | hexval(h[1]));
        h += 2;
    }
    out[n] = '\0';
}

/** What the fake does for SELECT_NETWORK of network `id`: probe, connect or
 * refuse, by its key management, `ca_cert` and passphrase. */
static void fake_select(int fd, int id)
{
    struct fnet *n = &fnets[id];
    char ev[512], ssid[40], pin[128];

    unhex(n->ssid, ssid, sizeof(ssid));
    if (strstr(n->key_mgmt, "WPA-EAP")) {
        if (strcmp(n->ca_cert, "\"probe://\"") == 0) {
            if (n->password) {
                fprintf(fake_log, "VIOLATION: a password before the probe\n");
            }
            snprintf(ev, sizeof(ev),
                     "CTRL-EVENT-EAP-PEER-CERT depth=0 subject='/C=KG/CN=radius.uni.example' "
                     "hash=%s",
                     fake_cert);
            fake_event(fd, ev);
            fake_event(fd, "CTRL-EVENT-EAP-PEER-CERT depth=0 subject='/C=KG/CN=radius.uni.example' "
                           "cert=308201");
            fake_event(fd, "CTRL-EVENT-EAP-PEER-ALT depth=0 DNS:radius.uni.example");
            fake_event(fd, "CTRL-EVENT-EAP-TLS-CERT-ERROR reason=8 depth=0 "
                           "subject='/C=KG/CN=radius.uni.example' err='Server certificate chain "
                           "probe'");
            fake_event(fd, "CTRL-EVENT-EAP-FAILURE EAP authentication failed");
            snprintf(ev, sizeof(ev),
                     "CTRL-EVENT-SSID-TEMP-DISABLED id=%d ssid=\"%s\" auth_failures=1 "
                     "duration=10 reason=AUTH_FAILED",
                     id, ssid);
            fake_event(fd, ev);
            return;
        }
        snprintf(pin, sizeof(pin), "\"hash://server/sha256/%s\"", fake_cert);
        if (strcmp(n->ca_cert, pin) == 0 && n->password) {
            snprintf(ev, sizeof(ev),
                     "CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:00:03 completed "
                     "[id=%d id_str=]",
                     id);
            fake_event(fd, ev);
            fake_current = id;
            return;
        }
        snprintf(ev, sizeof(ev), "CTRL-EVENT-EAP-PEER-CERT depth=0 subject='/CN=other' hash=%s",
                 fake_cert);
        fake_event(fd, ev);
        fake_event(fd, "CTRL-EVENT-EAP-TLS-CERT-ERROR reason=0 depth=0 subject='/CN=other' "
                       "err='Server certificate mismatch'");
        snprintf(ev, sizeof(ev),
                 "CTRL-EVENT-SSID-TEMP-DISABLED id=%d ssid=\"%s\" auth_failures=1 duration=10 "
                 "reason=AUTH_FAILED",
                 id, ssid);
        fake_event(fd, ev);
        return;
    }
    if (strstr(n->key_mgmt, "WPA-PSK") && strcmp(n->psk, "\"correct horse\"") != 0) {
        fake_event(fd, "CTRL-EVENT-ASSOC-REJECT bssid=02:00:00:00:00:01 status_code=1");
        snprintf(ev, sizeof(ev),
                 "CTRL-EVENT-SSID-TEMP-DISABLED id=%d ssid=\"%s\" auth_failures=1 duration=10 "
                 "reason=WRONG_KEY",
                 id, ssid);
        fake_event(fd, ev);
        return;
    }
    snprintf(ev, sizeof(ev),
             "CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:00:01 completed [id=%d id_str=]",
             id);
    fake_event(fd, ev);
    fake_current = id;
}

/** Answer the request `req` from the client at `from` on `fd`. */
static void fake_answer(int fd, char *req, struct sockaddr_un *from, socklen_t fromlen)
{
    char reply[2048], field[64], value[1024], ssid[40];
    int id, i, n = 0;

    reply[0] = '\0';
    fprintf(fake_log, "%s\n", req);
    fflush(fake_log);
    if (strcmp(req, "ATTACH") == 0) {
        fake_mon = *from;
        fake_monlen = fromlen;
        strcpy(reply, "OK\n");
    } else if (strcmp(req, "DETACH") == 0) {
        fake_monlen = 0;
        strcpy(reply, "OK\n");
    } else if (strcmp(req, "SCAN") == 0) {
        strcpy(reply, "OK\n");
    } else if (strcmp(req, "SCAN_RESULTS") == 0) {
        strcpy(reply, "bssid / frequency / signal level / flags / ssid\n"
                      "02:00:00:00:00:04\t2412\t-80\t[ESS]\tcafe\n"
                      "02:00:00:00:00:02\t2412\t-70\t[WPA2-PSK-CCMP][ESS]\thome\n"
                      "02:00:00:00:00:01\t2437\t-40\t[WPA2-PSK+SAE-CCMP][ESS]\thome\n"
                      "02:00:00:00:00:03\t2462\t-55\t[WPA2-EAP-CCMP][ESS]\tuni\n"
                      "02:00:00:00:00:05\t2412\t-60\t[WEP][ESS]\told\n"
                      "02:00:00:00:00:06\t2412\t-50\t[WPA2-PSK-CCMP][ESS]\t\n");
    } else if (strcmp(req, "LIST_NETWORKS") == 0) {
        n = snprintf(reply, sizeof(reply), "network id / ssid / bssid / flags\n");
        for (i = 0; i < 8; i++) {
            if (fnets[i].used) {
                unhex(fnets[i].ssid, ssid, sizeof(ssid));
                n += snprintf(reply + n, sizeof(reply) - (size_t)n, "%d\t%s\tany\t%s\n", i, ssid,
                              i == fake_current ? "[CURRENT]" : "");
            }
        }
    } else if (strcmp(req, "ADD_NETWORK") == 0) {
        for (i = 0; i < 8 && fnets[i].used; i++) {
        }
        memset(&fnets[i], 0, sizeof(fnets[i]));
        fnets[i].used = 1;
        snprintf(reply, sizeof(reply), "%d\n", i);
    } else if (sscanf(req, "SET_NETWORK %d %63s %1023[^\n]", &id, field, value) == 3 && id >= 0 &&
               id < 8 && fnets[id].used) {
        if (strcmp(field, "ssid") == 0) {
            snprintf(fnets[id].ssid, sizeof(fnets[id].ssid), "%s", value);
        } else if (strcmp(field, "key_mgmt") == 0) {
            snprintf(fnets[id].key_mgmt, sizeof(fnets[id].key_mgmt), "%s", value);
        } else if (strcmp(field, "ca_cert") == 0) {
            snprintf(fnets[id].ca_cert, sizeof(fnets[id].ca_cert), "%s", value);
        } else if (strcmp(field, "psk") == 0) {
            snprintf(fnets[id].psk, sizeof(fnets[id].psk), "%s", value);
        } else if (strcmp(field, "password") == 0) {
            fnets[id].password = 1;
        }
        strcpy(reply, "OK\n");
    } else if (sscanf(req, "GET_NETWORK %d %63s", &id, field) == 2 && id >= 0 && id < 8 &&
               fnets[id].used && strcmp(field, "ca_cert") == 0 && fnets[id].ca_cert[0]) {
        snprintf(reply, sizeof(reply), "%s", fnets[id].ca_cert);
    } else if (sscanf(req, "SELECT_NETWORK %d", &id) == 1 && id >= 0 && id < 8 && fnets[id].used) {
        strcpy(reply, "OK\n");
        (void)sendto(fd, reply, strlen(reply), 0, (struct sockaddr *)from, fromlen);
        fake_select(fd, id);
        return;
    } else if (sscanf(req, "REMOVE_NETWORK %d", &id) == 1 && id >= 0 && id < 8 && fnets[id].used) {
        fnets[id].used = 0;
        strcpy(reply, "OK\n");
    } else if (strncmp(req, "ENABLE_NETWORK ", 15) == 0 ||
               strncmp(req, "DISABLE_NETWORK ", 16) == 0 || strcmp(req, "SAVE_CONFIG") == 0) {
        strcpy(reply, "OK\n");
    } else if (strcmp(req, "STATUS") == 0) {
        if (fake_current >= 0) {
            unhex(fnets[fake_current].ssid, ssid, sizeof(ssid));
            snprintf(reply, sizeof(reply),
                     "bssid=02:00:00:00:00:01\nfreq=2437\nssid=%s\nid=%d\nmode=station\n"
                     "key_mgmt=WPA2-PSK\nwpa_state=COMPLETED\nip_address=10.0.0.5\n",
                     ssid, fake_current);
        } else {
            strcpy(reply, "wpa_state=DISCONNECTED\naddress=02:00:00:00:00:10\n");
        }
    } else if (strcmp(req, "SIGNAL_POLL") == 0) {
        strcpy(reply, "RSSI=-40\nLINKSPEED=54\nNOISE=9999\nFREQUENCY=2437\n");
    } else {
        strcpy(reply, "FAIL\n");
    }
    (void)sendto(fd, reply, strlen(reply), 0, (struct sockaddr *)from, fromlen);
    if (strcmp(req, "SCAN") == 0) {
        fake_event(fd, "CTRL-EVENT-SCAN-STARTED ");
        fake_event(fd, "CTRL-EVENT-SCAN-RESULTS ");
    }
}

/** The fake's loop, in the child, on the bound socket `fd`; it ends when
 * the test kills it. */
static void fake_serve(int fd)
{
    char req[2048];
    struct sockaddr_un from;
    socklen_t fromlen;
    ssize_t n;

    fake_log = fopen("requests", "w");
    if (!fake_log) {
        _exit(3);
    }
    for (;;) {
        fromlen = sizeof(from);
        n = recvfrom(fd, req, sizeof(req) - 1, 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            _exit(4);
        }
        req[n] = '\0';
        fake_answer(fd, req, &from, fromlen);
    }
}

/* A known network a row starts with. */
struct known {
    const char *ssid, *key_mgmt, *ca_cert;
    int password;
};

/* What a row ran: wifi's exit status, its standard output, and the
 * requests the fake took. */
static char out[16384], requests[16384];

/** Read the file `path` into `buf`, empty when it cannot be read. */
static void slurp(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n = 0;

    if (f) {
        n = fread(buf, 1, size - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
}

/**
 * Run wifi with the arguments `args`, `answers` on its standard input,
 * against a fake that starts with the networks `known`, ended by one of no
 * SSID, and whose server presents `cert`. Returns wifi's status, with its
 * output in `out` and the fake's requests in `requests`.
 */
static int run(const char *const *args, const char *answers, const struct known *known,
               const char *cert)
{
    struct sockaddr_un addr;
    char *argv[8];
    int fd, argc, i, rc, saved;
    pid_t child;
    FILE *f;

    memset(fnets, 0, sizeof(fnets));
    fake_cert = cert;
    fake_current = -1;
    fake_monlen = 0;
    for (i = 0; known && known[i].ssid; i++) {
        fnets[i].used = 1;
        hex(fnets[i].ssid, (const unsigned char *)known[i].ssid, strlen(known[i].ssid));
        snprintf(fnets[i].key_mgmt, sizeof(fnets[i].key_mgmt), "%s", known[i].key_mgmt);
        snprintf(fnets[i].ca_cert, sizeof(fnets[i].ca_cert), "%s", known[i].ca_cert);
        fnets[i].password = known[i].password;
    }

    (void)unlink("wlan0");
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "wlan0");
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "wifi_hosttest: cannot bind the fake's socket: %s\n", strerror(errno));
        exit(1);
    }
    fflush(NULL);
    child = fork();
    if (child == 0) {
        fake_serve(fd);
    }
    close(fd);
    if (child < 0) {
        fprintf(stderr, "wifi_hosttest: fork: %s\n", strerror(errno));
        exit(1);
    }

    f = fopen("answers", "w");
    if (!f) {
        exit(1);
    }
    fputs(answers, f);
    fclose(f);
    for (argc = 0; args[argc]; argc++) {
        argv[argc] = (char *)args[argc];
    }
    argv[argc] = 0;
    saved = dup(STDOUT_FILENO);
    if (!freopen("answers", "r", stdin) || !freopen("out", "w", stdout)) {
        exit(1);
    }
    rc = wifi_main(argc, argv);
    fflush(stdout);
    (void)dup2(saved, STDOUT_FILENO);
    close(saved);

    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
    (void)unlink("wlan0");
    slurp("out", out, sizeof(out));
    slurp("requests", requests, sizeof(requests));
    return rc;
}

/** The place of `what` in the requests, or -1. */
static long at(const char *what)
{
    const char *p = strstr(requests, what);

    return p ? (long)(p - requests) : -1;
}

/** `wifi scan`: one line per network, the strongest first, at the
 * strongest access point, with no hidden network and no second `home`. */
static void row_scan(void)
{
    static const char *const args[] = {"wifi", "scan", 0};
    static const struct known known[] = {{"cafe", "NONE", "", 0}, {0, 0, 0, 0}};
    const char *home, *uni, *old, *cafe;

    CHECK(run(args, "", known, HASH1) == 0, "scan: exits 0");
    home = strstr(out, "home ");
    uni = strstr(out, "uni ");
    old = strstr(out, "old ");
    cafe = strstr(out, "cafe ");
    CHECK(home && uni && old && cafe && home < uni && uni < old && old < cafe,
          "scan: the strongest network first");
    CHECK(home && !strstr(home + 1, "home "), "scan: a network listed once");
    CHECK(strstr(out, "-40 dBm       6  WPA2/WPA3\n") != 0,
          "scan: home at its strongest access point, on channel 6, WPA2/WPA3");
    CHECK(strstr(out, "-55 dBm      11  enterprise\n") != 0, "scan: uni is enterprise");
    CHECK(strstr(out, "-60 dBm       1  WEP\n") != 0, "scan: old is WEP");
    CHECK(strstr(out, "-80 dBm       1  open, known\n") != 0, "scan: cafe is open and known");
    CHECK(!strstr(out, "-50 dBm"), "scan: a hidden network is not listed");
    CHECK(at("ATTACH") >= 0 && at("ATTACH") < at("SCAN\n") && at("SCAN\n") < at("SCAN_RESULTS"),
          "scan: attaches, scans, and reads the results after");
}

/** `wifi connect home` with the right passphrase and a wrong one. */
static void row_personal(void)
{
    static const char *const args[] = {"wifi", "connect", "home", 0};
    static const struct known known[] = {{"cafe", "NONE", "", 0}, {0, 0, 0, 0}};

    CHECK(run(args, "correct horse\n", known, HASH1) == 0, "personal: exits 0");
    CHECK(strstr(out, "wifi: connected to home\n") != 0, "personal: says it is connected");
    CHECK(at("SET_NETWORK 1 ssid 686f6d65\n") >= 0, "personal: the SSID in hex");
    CHECK(at("SET_NETWORK 1 bgscan \"simple:30:-65:300\"\n") >= 0, "personal: bgscan");
    CHECK(at("SET_NETWORK 1 key_mgmt WPA-PSK WPA-PSK-SHA256 SAE\n") >= 0,
          "personal: WPA2's and WPA3's key management");
    CHECK(at("SET_NETWORK 1 ieee80211w 1\n") >= 0, "personal: optional frame protection");
    CHECK(at("SET_NETWORK 1 psk \"correct horse\"\n") >= 0, "personal: the passphrase quoted");
    CHECK(at("SELECT_NETWORK 1\n") >= 0 &&
              at("SELECT_NETWORK 1\n") < at("ENABLE_NETWORK 0 no-connect"),
          "personal: the other network enabled again after the select");
    CHECK(at("ENABLE_NETWORK 0 no-connect") < at("SAVE_CONFIG"), "personal: saved once joined");
    CHECK(at("REMOVE_NETWORK") < 0, "personal: nothing removed");

    CHECK(run(args, "wrong password\n", known, HASH1) == 1, "wrong passphrase: exits 1");
    CHECK(at("REMOVE_NETWORK 1\n") >= 0, "wrong passphrase: the network is not kept");
    CHECK(at("SAVE_CONFIG") < 0, "wrong passphrase: nothing saved");
    CHECK(at("ENABLE_NETWORK 0 no-connect") >= 0, "wrong passphrase: the other network enabled");

    CHECK(run(args, "short\n", known, HASH1) == 1, "short passphrase: exits 1");
    CHECK(at("SELECT_NETWORK") < 0 && at("REMOVE_NETWORK 1\n") >= 0,
          "short passphrase: refused before any select");
}

/** The answers of a row that joins `uni`: identity, password, anonymous
 * identity, server domain, and `trust`. */
#define EAP_ANSWERS(trust) "alice\nsecret pw\nanonymous@uni.example\nuni.example\n" trust "\n"

/** `wifi connect uni`: the probe, then yes; then the probe and no. */
static void row_enterprise(void)
{
    static const char *const args[] = {"wifi", "connect", "uni", 0};
    long probe, password;

    CHECK(run(args, EAP_ANSWERS("y"), 0, HASH1) == 0, "yes: exits 0");
    probe = at("SET_NETWORK 0 ca_cert \"probe://\"\n");
    password = at("SET_NETWORK 0 password ");
    CHECK(probe >= 0 && probe < at("SELECT_NETWORK 0\n"), "yes: probed before the first select");
    CHECK(password > at("SELECT_NETWORK 0\n") && password > at("DISABLE_NETWORK 0\n"),
          "yes: the password set only after the probe");
    CHECK(at("SET_NETWORK 0 password 736563726574207077\n") >= 0, "yes: the password in hex");
    CHECK(at("VIOLATION") < 0, "yes: no password was set at the probe");
    CHECK(at("SET_NETWORK 0 ca_cert \"hash://server/sha256/" HASH1 "\"\n") > probe,
          "yes: the certificate pinned by its hash");
    CHECK(at("SET_NETWORK 0 key_mgmt WPA-EAP WPA-EAP-SHA256\n") >= 0, "yes: WPA-EAP");
    CHECK(at("SET_NETWORK 0 eap PEAP TTLS\n") >= 0, "yes: PEAP and TTLS");
    CHECK(at("SET_NETWORK 0 phase2 \"auth=MSCHAPV2\"\n") >= 0, "yes: MSCHAPv2 inside");
    CHECK(at("SET_NETWORK 0 identity 616c696365\n") >= 0, "yes: the identity");
    CHECK(at("SET_NETWORK 0 anonymous_identity 616e6f6e796d6f75734075") >= 0,
          "yes: the anonymous identity");
    CHECK(at("SET_NETWORK 0 domain_suffix_match 756e692e6578616d706c65\n") >= 0,
          "yes: the server domain");
    CHECK(at("SAVE_CONFIG") > password, "yes: saved once joined");
    CHECK(strstr(out, "subject: /C=KG/CN=radius.uni.example\n") != 0, "yes: the subject shown");
    CHECK(strstr(out, "name:    DNS:radius.uni.example\n") != 0, "yes: the name shown");
    CHECK(strstr(out, "SHA-256: 5A:1B:C1:29:62:05:E6:FD:BE:39:79:72:8E:FE:39:20\n"
                      "           79:88:85:C1:C4:59:0B:5F:90:F4:32:22:D2:39:CA:6A\n") != 0,
          "yes: the fingerprint shown in two lines");
    CHECK(strstr(out, "wifi: connected to uni\n") != 0, "yes: says it is connected");

    CHECK(run(args, EAP_ANSWERS("n"), 0, HASH1) == 1, "no: exits 1");
    CHECK(at("SET_NETWORK 0 password") < 0, "no: no password was ever set");
    CHECK(at("REMOVE_NETWORK 0\n") >= 0, "no: the network removed");
    CHECK(at("SAVE_CONFIG") < 0, "no: nothing saved");
    CHECK(strstr(out, "uni is not trusted") != 0, "no: says so");

    CHECK(run(args, "alice\n", 0, HASH1) == 1, "input ended: exits 1");
    CHECK(at("ADD_NETWORK") < 0, "input ended: nothing added");

    {
        static const char *const pap[] = {"wifi", "connect", "-e", "ttls-pap", "uni", 0};

        CHECK(run(pap, EAP_ANSWERS("yes"), 0, HASH1) == 0, "ttls-pap: exits 0");
        CHECK(at("SET_NETWORK 0 eap TTLS\n") >= 0 && at("SET_NETWORK 0 phase2 \"auth=PAP\"\n") >= 0,
              "ttls-pap: TTLS with PAP");
    }
}

/** A pinned network whose server presents another certificate: refused,
 * kept, and explained by `wifi status` from the log. */
static void row_changed(void)
{
    static const char *const connect[] = {"wifi", "connect", "uni", 0};
    static const char *const status[] = {"wifi", "status", 0};
    static const struct known known[] = {
        {"uni", "WPA-EAP WPA-EAP-SHA256", "\"hash://server/sha256/" HASH1 "\"", 1}, {0, 0, 0, 0}};
    FILE *f;

    CHECK(run(connect, "", known, HASH2) == 1, "changed: exits 1");
    CHECK(at("SELECT_NETWORK 0\n") >= 0, "changed: the known network selected");
    CHECK(at("REMOVE_NETWORK") < 0, "changed: the network kept");
    CHECK(at("SET_NETWORK") < 0, "changed: its pin left as it was");

    f = fopen("services", "w");
    if (!f) {
        exit(1);
    }
    fputs("ntpd: setting time\n"
          "wlan0: CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:00:03 completed [id=0 id_str=]\n"
          "wlan0: CTRL-EVENT-DISCONNECTED bssid=02:00:00:00:00:03 reason=3 locally_generated=1\n"
          "wlan0: SME: Trying to authenticate with 02:00:00:00:00:03 (SSID='uni' freq=2412 MHz)\n"
          "wlan0: Trying to associate with 02:00:00:00:00:03 (SSID='uni' freq=2412 MHz)\n"
          "wlan0: Associated with 02:00:00:00:00:03\n"
          "wlan0: CTRL-EVENT-EAP-STARTED EAP authentication started\n"
          "wlan0: CTRL-EVENT-EAP-PROPOSED-METHOD vendor=0 method=25\n"
          "wlan0: CTRL-EVENT-EAP-PEER-CERT depth=0 subject='/CN=other' hash=" HASH2 "\n"
          "udhcpc: started, v1.36.1\n"
          "wlan0: CTRL-EVENT-EAP-TLS-CERT-ERROR reason=0 depth=0 subject='/CN=other' "
          "err='Server certificate mismatch'\n"
          "wlan0: CTRL-EVENT-EAP-FAILURE EAP authentication failed\n"
          "wlan0: CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"uni\" auth_failures=1 duration=10 "
          "reason=AUTH_FAILED\n"
          "wlan0: CTRL-EVENT-DISCONNECTED bssid=02:00:00:00:00:03 reason=23\n",
          f);
    fclose(f);
    CHECK(run(status, "", known, HASH2) == 0, "status: exits 0");
    CHECK(strstr(out, "not connected (DISCONNECTED)\n") != 0, "status: not connected");
    CHECK(strstr(out, "not the one trusted for this network\n") != 0,
          "status: the certificate, not the EAP failure after it");
    CHECK(!strstr(out, "identity or password"), "status: no password error");
    CHECK(strstr(out, "it presented /CN=other\n") != 0, "status: the certificate presented");
    CHECK(strstr(out, "SHA-256: 00:11:22:33") != 0, "status: its fingerprint");

    f = fopen("services", "a");
    if (!f) {
        exit(1);
    }
    fputs("wlan0: SME: Trying to authenticate with 02:00:00:00:00:03 (SSID='uni' freq=2412 MHz)\n"
          "wlan0: Trying to associate with 02:00:00:00:00:03 (SSID='uni' freq=2412 MHz)\n"
          "wlan0: CTRL-EVENT-EAP-STARTED EAP authentication started\n"
          "wlan0: CTRL-EVENT-EAP-PEER-CERT depth=0 subject='/CN=uni' hash=" HASH1 "\n"
          "wlan0: CTRL-EVENT-EAP-FAILURE EAP authentication failed\n"
          "wlan0: CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid=\"uni\" auth_failures=2 duration=20 "
          "reason=AUTH_FAILED\n",
          f);
    fclose(f);
    CHECK(run(status, "", known, HASH1) == 0 &&
              strstr(out, "refused the identity or password\n") != 0 && !strstr(out, "trusted"),
          "status: a later attempt's password error");

    f = fopen("services", "a");
    if (!f) {
        exit(1);
    }
    fputs(
        "wlan0: CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:00:03 completed [id=0 id_str=]\n",
        f);
    fclose(f);
    CHECK(run(status, "", known, HASH2) == 0 && !strstr(out, "trusted"),
          "status: no reason after a later connection");
    (void)unlink("services");
}

/** `wifi list` and `wifi forget`. */
static void row_known(void)
{
    static const char *const list[] = {"wifi", "list", 0};
    static const char *const forget[] = {"wifi", "forget", "home", 0};
    static const char *const nothing[] = {"wifi", "forget", "nowhere", 0};
    static const struct known known[] = {
        {"home", "WPA-PSK", "", 0},
        {"uni", "WPA-EAP WPA-EAP-SHA256", "\"hash://server/sha256/" HASH1 "\"", 1},
        {0, 0, 0, 0}};

    CHECK(run(list, "", known, HASH1) == 0, "list: exits 0");
    CHECK(strstr(out, "home\nuni\n  SHA-256: 5A:1B:") != 0, "list: the networks, and uni's pin");

    CHECK(run(forget, "", known, HASH1) == 0, "forget: exits 0");
    CHECK(at("REMOVE_NETWORK 0\n") >= 0 && at("REMOVE_NETWORK 0\n") < at("SAVE_CONFIG"),
          "forget: removed and saved");
    CHECK(at("REMOVE_NETWORK 1") < 0, "forget: the other network kept");

    CHECK(run(nothing, "", known, HASH1) == 1, "forget an unknown network: exits 1");
    CHECK(at("SAVE_CONFIG") < 0, "forget an unknown network: nothing saved");
}

/** The usage, and the decoding of a name as wpa_supplicant prints it. */
static void row_parse(void)
{
    static const char *const bare[] = {"wifi", 0};
    static const char *const two[] = {"wifi", "connect", "a", "b", 0};
    static const char *const method[] = {"wifi", "connect", "-e", "md5", "uni", 0};
    unsigned char got[8];

    CHECK(run(bare, "", 0, HASH1) == 2, "usage: no command");
    CHECK(run(two, "", 0, HASH1) == 2, "usage: two names");
    CHECK(run(method, "", 0, HASH1) == 2, "usage: an unknown method");
    CHECK(ssid_decode("a\\x41\\\\b\\\"", got, sizeof(got)) == 5 && memcmp(got, "aA\\b\"", 5) == 0,
          "decode: escapes");
    CHECK(ssid_decode("abcdefghi", got, sizeof(got)) == -1, "decode: too long");
    CHECK(security_of("[WPA-PSK-TKIP][ESS]") == SEC_PERSONAL &&
              strcmp(security_name("[WPA-PSK-TKIP][ESS]"), "WPA") == 0,
          "security: WPA with TKIP");
    CHECK(strcmp(security_name("[WPA2-SAE-CCMP][ESS]"), "WPA3") == 0, "security: WPA3");
    CHECK(channel_of(2484) == 14 && channel_of(2412) == 1 && channel_of(5180) == 36,
          "channel: 14, 1 and 36");
}

int main(void)
{
    if (chdir(fix_dir()) != 0) {
        fprintf(stderr, "wifi_hosttest: cannot enter %s\n", fix_dir());
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    row_scan();
    row_personal();
    row_enterprise();
    row_changed();
    row_known();
    row_parse();
    (void)unlink("answers");
    (void)unlink("out");
    (void)unlink("requests");
    if (fails == 0) {
        fix_passed();
    }
    fprintf(stderr, "wifi_hosttest: %d failed\n", fails);
    return fails > 120 ? 120 : fails;
}
