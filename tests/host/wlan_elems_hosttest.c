/* Check Wi-Fi element framing, duplicate rejection and station builders.
 * host-deps: rach2/sys/network/wireless/elems.c
 */
#include "../../rach2/sys/network/wireless/wireless.h"
#include "../../rach2/sys/network/wireless/wcrypto.h"
#include "../../rach2/sys/network/socket/genetlink.h"
#include "../../rach2/sys/lib/kerrno.h"

extern int printf(const char *format, ...);

static unsigned failures;
#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            printf("wlan_elems_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

/** Compare n bytes without libc's integer typedefs. */
static int equal(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/** Require no borrowed pointer to remain after a failed parse. */
static void cleared(const struct ieee80211_elems *e)
{
    unsigned i;

    for (i = 0; i < 256; ++i)
        CHECK(!e->data[i]);
    CHECK(!e->wmm_info && !e->wmm_param && !e->wpa);
}

/** Exercise framing independently of element-specific length checks. */
static void framing(void)
{
    static const uint8_t good[] = {0, 0, 1, 2, 0x82, 0x84, 3, 1, 6};
    static const uint8_t repeat[] = {0, 0, 0, 1, 'a'};
    static const uint8_t vendor[] = {221, 4, 0, 1, 2, 3, 221, 4, 0, 1, 2, 4};
    static const uint8_t ext[] = {255, 1, 1, 255, 1, 2};
    static const uint8_t ext_repeat[] = {255, 1, 1, 255, 2, 1, 0};
    static const uint8_t unknown[] = {200, 1, 9};
    struct ieee80211_elems e;
    uint8_t long_ie[257];
    unsigned i;

    CHECK(sizeof(struct ieee80211_hdr) == 24);
    CHECK(sizeof(struct ieee80211_hdr_4addr) == 30);
    CHECK(sizeof(struct ieee80211_qos_hdr) == 26);
    CHECK(sizeof(struct ieee80211_ctl_short) == 10);
    CHECK(sizeof(struct ieee80211_ctl_rts) == 16);
    CHECK(sizeof(struct ieee80211_beacon) == 12);
    CHECK(sizeof(struct ieee80211_auth) == 6);
    CHECK(sizeof(struct ieee80211_assoc_req) == 4);
    CHECK(sizeof(struct ieee80211_reassoc_req) == 10);
    CHECK(sizeof(struct ieee80211_assoc_resp) == 6);
    CHECK(sizeof(struct ieee80211_mmie) == 18);
    CHECK(sizeof(struct genlmsghdr) == GENL_HDRLEN);
    CHECK(sizeof(struct nlattr) == NLA_HDRLEN);
    CHECK(WLAN_STATUS_ANTI_CLOG_REQUIRED == 76);
    CHECK(WLAN_STATUS_SAE_HASH_TO_ELEMENT == 126);
    CHECK(ieee80211_parse_elems(good, sizeof(good), &e) == 0);
    CHECK(e.data[0] == good && e.data[1] == good + 2 && e.data[3] == good + 6);
    CHECK(ieee80211_parse_elems(good, sizeof(good) - 1, &e) == -EINVAL);
    cleared(&e);
    CHECK(ieee80211_parse_elems(repeat, sizeof(repeat), &e) == -EINVAL);
    cleared(&e);
    CHECK(ieee80211_parse_elems(vendor, sizeof(vendor), &e) == 0);
    CHECK(e.data[221] == vendor);
    CHECK(ieee80211_parse_elems(ext, sizeof(ext), &e) == 0);
    CHECK(ieee80211_parse_elems(ext_repeat, sizeof(ext_repeat), &e) == -EINVAL);
    CHECK(ieee80211_parse_elems(unknown, sizeof(unknown), &e) == 0);
    CHECK(e.data[200] == unknown);
    CHECK(ieee80211_parse_elems(0, 0, &e) == 0);
    cleared(&e);
    CHECK(ieee80211_parse_elems(0, 1, &e) == -EINVAL);
    cleared(&e);
    CHECK(ieee80211_parse_elems(good, sizeof(good), 0) == -EINVAL);
    long_ie[0] = 200;
    long_ie[1] = 255;
    for (i = 2; i < sizeof(long_ie); ++i)
        long_ie[i] = (uint8_t)i;
    for (i = 1; i < sizeof(long_ie); ++i) {
        CHECK(ieee80211_parse_elems(long_ie, i, &e) == -EINVAL);
        cleared(&e);
    }
    CHECK(ieee80211_parse_elems(long_ie, sizeof(long_ie), &e) == 0);
    CHECK(e.data[200] == long_ie);
}

/** Check exact and minimum lengths independently of their payload contents. */
static void lengths(void)
{
    static const unsigned exact[][2] = {{3, 1}, {42, 1}, {45, 26}, {61, 22}, {56, 5}, {76, 16}};
    static const unsigned minimum[][2] = {{5, 4},   {7, 3},   {48, 2}, {50, 1},
                                          {127, 1}, {244, 1}, {255, 1}};
    uint8_t buf[260];
    struct ieee80211_elems e;
    unsigned i, j, len;

    for (i = 0; i < sizeof(buf); ++i)
        buf[i] = 0;
    for (i = 0; i < sizeof(exact) / sizeof(exact[0]); ++i) {
        buf[0] = (uint8_t)exact[i][0];
        for (j = 0; j < 3; ++j) {
            len = exact[i][1] + j - 1;
            buf[1] = (uint8_t)len;
            CHECK((ieee80211_parse_elems(buf, len + 2, &e) == 0) == (j == 1));
        }
    }
    for (i = 0; i < sizeof(minimum) / sizeof(minimum[0]); ++i) {
        buf[0] = (uint8_t)minimum[i][0];
        buf[1] = (uint8_t)(minimum[i][1] - 1);
        CHECK(ieee80211_parse_elems(buf, buf[1] + 2u, &e) == -EINVAL);
        ++buf[1];
        CHECK(ieee80211_parse_elems(buf, buf[1] + 2u, &e) == 0);
    }
    buf[0] = WLAN_EID_SSID;
    for (i = 0; i <= 33; ++i) {
        buf[1] = (uint8_t)i;
        CHECK((ieee80211_parse_elems(buf, i + 2, &e) == 0) == (i <= 32));
    }
    buf[0] = WLAN_EID_SUPP_RATES;
    for (i = 0; i <= 9; ++i) {
        buf[1] = (uint8_t)i;
        CHECK((ieee80211_parse_elems(buf, i + 2, &e) == 0) == (i >= 1 && i <= 8));
    }
}

/** Keep WPA, WMM information and WMM parameters separate, rejecting repeats. */
static void vendors(void)
{
    uint8_t buf[96];
    static const uint8_t wpa[] = {0, 0x50, 0xf2, 1, 1, 0};
    static const uint8_t info[] = {0, 0x50, 0xf2, 2, 0, 1, 0};
    static const uint8_t param[24] = {0, 0x50, 0xf2, 2, 1, 1};
    struct ieee80211_elems e;
    unsigned n, saved, i;
    const uint8_t *payloads[3];
    unsigned sizes[3];

    payloads[0] = wpa;
    payloads[1] = info;
    payloads[2] = param;
    sizes[0] = sizeof(wpa);
    sizes[1] = sizeof(info);
    sizes[2] = sizeof(param);
    n = 0;
    for (i = 0; i < 3; ++i)
        CHECK(!ieee80211_put_elem(buf, sizeof(buf), &n, 221, payloads[i], sizes[i]));
    CHECK(!ieee80211_parse_elems(buf, n, &e));
    CHECK(e.wpa == buf && e.wmm_info == buf + 8 && e.wmm_param == buf + 17);
    saved = n;
    for (i = 0; i < 3; ++i) {
        n = saved;
        CHECK(!ieee80211_put_elem(buf, sizeof(buf), &n, 221, payloads[i], sizes[i]));
        CHECK(ieee80211_parse_elems(buf, n, &e) == -EINVAL);
        cleared(&e);
    }
    buf[1] = 5;
    CHECK(ieee80211_parse_elems(buf, 7, &e) == -EINVAL);
}

/** Compare builders with literal wire bytes and check atomic failure. */
static void builders(void)
{
    static const uint8_t rates[32] = {0x82, 0x84, 0x8b, 0x96, 12, 18, 24, 36, 48, 72, 96, 108};
    static const uint8_t golden[] = {0,  3,  'a', 'p', '1', 1, 8,  0x82, 0x84, 0x8b, 0x96,
                                     12, 18, 24,  36,  50,  4, 48, 72,   96,   108};
    static const uint8_t ssid[32] = {'a', 'p', '1'};
    static const uint8_t ht[26] = {0};
    static const uint8_t rsn[] = {1, 0};
    static const uint8_t rsnx[] = {0x20};
    static const uint8_t extcap[] = {0, 0, 0, 0};
    struct ieee80211_sta_elems p = {0};
    struct ieee80211_elems e;
    uint8_t buf[512], saved[512], large[256];
    unsigned n, expected, cap, i;

    p.ssid = ssid;
    p.ssid_len = 3;
    p.rates = rates;
    p.rates_len = 12;
    n = 99;
    CHECK(!ieee80211_build_sta_elems(buf, sizeof(buf), &n, &p));
    CHECK(n == sizeof(golden) && equal(buf, golden, n));
    p.ht_cap = ht;
    p.rsn = rsn;
    p.rsn_len = sizeof(rsn);
    p.rsnx = rsnx;
    p.rsnx_len = sizeof(rsnx);
    p.ext_cap = extcap;
    p.ext_cap_len = sizeof(extcap);
    p.wmm = 1;
    CHECK(!ieee80211_build_sta_elems(buf, sizeof(buf), &n, &p));
    expected = n;
    CHECK(!ieee80211_parse_elems(buf, n, &e));
    CHECK(e.data[45] && equal(e.data[45] + 2, ht, sizeof(ht)));
    CHECK(e.data[48] && equal(e.data[48] + 2, rsn, sizeof(rsn)));
    CHECK(e.data[244] && e.data[127] && e.wmm_info);
    for (i = 0; i < sizeof(buf); ++i)
        saved[i] = buf[i] = 0xa5;
    for (cap = 0; cap < expected; ++cap) {
        n = 999;
        CHECK(ieee80211_build_sta_elems(buf, cap, &n, &p) == -ENOSPC);
        CHECK(n == 999 && equal(buf, saved, sizeof(buf)));
    }
    CHECK(!ieee80211_build_sta_elems(buf, expected, &n, &p));
    CHECK(n == expected && buf[n] == 0xa5);
    for (i = 1; i <= 32; ++i) {
        p.rates_len = i;
        p.ssid_len = i;
        CHECK(!ieee80211_build_sta_elems(buf, sizeof(buf), &n, &p));
        CHECK(!ieee80211_parse_elems(buf, n, &e));
        CHECK(e.data[0][1] == i);
        CHECK(e.data[1][1] == (i > 8 ? 8 : i));
        CHECK((e.data[50] != 0) == (i > 8));
        if (i > 8)
            CHECK(e.data[50][1] == i - 8);
    }
    p.ssid = 0;
    p.ssid_len = 0;
    CHECK(!ieee80211_build_sta_elems(buf, sizeof(buf), &n, &p));
    CHECK(!ieee80211_parse_elems(buf, n, &e) && e.data[0][1] == 0);
    n = 77;
    p.ssid_len = 1;
    CHECK(ieee80211_build_sta_elems(buf, sizeof(buf), &n, &p) == -EINVAL && n == 77);
    p.ssid_len = 0;
    p.rates_len = 33;
    CHECK(ieee80211_build_sta_elems(buf, sizeof(buf), &n, &p) == -EINVAL);
    p.rates_len = 1;
    p.rsn_len = 1;
    CHECK(ieee80211_build_sta_elems(buf, sizeof(buf), &n, &p) == -EINVAL);
    CHECK(ieee80211_build_sta_elems(buf, sizeof(buf), &n, 0) == -EINVAL);
    for (i = 0; i < sizeof(large); ++i)
        large[i] = (uint8_t)i;
    n = 0;
    CHECK(!ieee80211_put_elem(buf, sizeof(buf), &n, 200, large, 255));
    CHECK(n == 257 && buf[0] == 200 && buf[1] == 255 && equal(buf + 2, large, 255));
    CHECK(ieee80211_put_elem(buf, sizeof(buf), &n, 200, large, 256) == -EINVAL);
    CHECK(ieee80211_put_elem(buf, sizeof(buf), &n, 256, large, 1) == -EINVAL);
    CHECK(ieee80211_put_elem(buf, sizeof(buf), &n, 0, 0, 1) == -EINVAL);
    CHECK(ieee80211_put_elem(buf, sizeof(buf), &n, 0, 0, 0) == 0);
    n = ~0u;
    CHECK(ieee80211_put_elem(buf, sizeof(buf), &n, 0, 0, 0) == -ENOSPC);
    CHECK(n == ~0u);
}

/** Run bounded element and wire-layout checks without files or processes. */
int main(void)
{
    framing();
    lengths();
    vendors();
    builders();
    if (failures)
        return 1;
    printf("wlan_elems_hosttest: result: passed\n");
    return 0;
}
