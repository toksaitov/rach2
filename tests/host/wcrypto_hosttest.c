/* Check the Wi-Fi ciphers on the vectors of IEEE 802.11-2020 Annex J, of
 * P802.11i/D7.0 and of NIST, and their refusals.
 * host-deps: rach2/sys/network/wireless/crypto/aes.c
 * host-deps: rach2/sys/network/wireless/crypto/ccmp.c
 * host-deps: rach2/sys/network/wireless/crypto/bip.c
 * host-deps: rach2/sys/network/wireless/crypto/tkip.c
 */
#include "../../rach2/sys/network/wireless/crypto/wcrypto_int.h"
#include "../../rach2/sys/lib/kerrno.h"

extern int printf(const char *format, ...);

static unsigned failures;
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            printf("wcrypto_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                        \
        }                                                                      \
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

/** Copy n bytes. */
static void copy(uint8_t *dst, const uint8_t *src, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i)
        dst[i] = src[i];
}

/** Turn an ASCII string of hex digits into bytes; return the byte count. */
static unsigned hex(uint8_t *out, const char *s)
{
    unsigned n, v;

    for (n = 0; s[0] && s[1]; s += 2, ++n) {
        v = (unsigned)(s[0] <= '9' ? s[0] - '0' : s[0] - 'a' + 10) << 4;
        v |= (unsigned)(s[1] <= '9' ? s[1] - '0' : s[1] - 'a' + 10);
        out[n] = (uint8_t)v;
    }
    return n;
}

/*
 * The CCMP vectors. ccmp1 is Annex J's (M.6.4 in 802.11-2012); ccmp2, ccmp6
 * and ccmp7 are test MPDUs 2, 6 and 7 of P802.11i/D7.0 I.7.4, whose headers
 * 802.11-2020 masks as 802.11i did. Its MPDUs 3, 4, 5 and 8 set Order in a
 * QoS data frame, which now means an HT Control field they do not carry,
 * so they are left out. Bytes from FreeBSD's
 * tools/regression/net80211/ccmp/test_ccmp.c. Each plaintext header has
 * Protected set already, as the 802.11i text gives it.
 */
static const uint8_t ccmp1_tk[16] = {
    0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85, 0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd5, 0x2f,
};
static const uint8_t ccmp1_plain[44] = {
    0x08, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84, 0x44,
    0x08, 0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33, 0xf8, 0xba, 0x1a, 0x55, 0xd0, 0x2f,
    0x85, 0xae, 0x96, 0x7b, 0xb6, 0x2f, 0xb6, 0xcd, 0xa8, 0xeb, 0x7e, 0x78, 0xa0, 0x50,
};
static const uint8_t ccmp1_enc[60] = {
    0x08, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84, 0x44,
    0x08, 0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33, 0x0c, 0xe7, 0x00, 0x20, 0x76, 0x97,
    0x03, 0xb5, 0xf3, 0xd0, 0xa2, 0xfe, 0x9a, 0x3d, 0xbf, 0x23, 0x42, 0xa6, 0x43, 0xe4, 0x32,
    0x46, 0xe8, 0x0c, 0x3c, 0x04, 0xd0, 0x19, 0x78, 0x45, 0xce, 0x0b, 0x16, 0xf9, 0x76, 0x23,
};
static const uint8_t ccmp2_tk[16] = {
    0x8f, 0x7a, 0x05, 0x3f, 0xa5, 0x77, 0xa5, 0x59, 0x75, 0x29, 0x27, 0x20, 0x97, 0xa6, 0x03, 0xd5,
};
static const uint8_t ccmp2_plain[44] = {
    0x38, 0xc0, 0x6a, 0x51, 0xea, 0x10, 0x0c, 0x84, 0x68, 0x50, 0xee, 0xc1, 0x76, 0x2c, 0x88,
    0xde, 0xaf, 0x2e, 0xe9, 0xf4, 0x6a, 0x07, 0xe0, 0xcc, 0x83, 0xa0, 0x63, 0x4b, 0x5e, 0xd7,
    0x62, 0x7e, 0xb9, 0xdf, 0x22, 0x5e, 0x05, 0x74, 0x03, 0x42, 0xde, 0x19, 0x41, 0x17,
};
static const uint8_t ccmp2_enc[60] = {
    0x38, 0xc0, 0x6a, 0x51, 0xea, 0x10, 0x0c, 0x84, 0x68, 0x50, 0xee, 0xc1, 0x76, 0x2c, 0x88,
    0xde, 0xaf, 0x2e, 0xe9, 0xf4, 0x6a, 0x07, 0xe0, 0xcc, 0xea, 0x97, 0x00, 0xa0, 0xba, 0xcb,
    0xf3, 0x31, 0x81, 0x4b, 0x69, 0x65, 0xd0, 0x5b, 0xf2, 0xb2, 0xed, 0x38, 0xd4, 0xbe, 0xb0,
    0x69, 0xfe, 0x82, 0x71, 0x4a, 0x61, 0x0b, 0x54, 0x2f, 0xbf, 0x8d, 0xa0, 0x6a, 0xa4, 0xae,
};
static const uint8_t ccmp6_tk[16] = {
    0xf7, 0x1e, 0xea, 0x4e, 0x1f, 0x58, 0x80, 0x4b, 0x97, 0x17, 0x23, 0x0a, 0xd0, 0x61, 0x46, 0x41,
};
static const uint8_t ccmp6_plain[46] = {
    0x88, 0x52, 0xe1, 0x1f, 0x5a, 0xf2, 0x84, 0x30, 0xfd, 0xab, 0xbf, 0xf9, 0x43, 0xb9, 0xf9, 0xa6,
    0xab, 0x1d, 0x98, 0xc7, 0xfe, 0x73, 0x50, 0x71, 0x3d, 0x6a, 0xab, 0xfd, 0xa2, 0x2d, 0x3a, 0x0b,
    0xfc, 0x9c, 0xc1, 0xfc, 0x07, 0x93, 0x63, 0xc2, 0xfc, 0xa1, 0x43, 0xe6, 0xeb, 0x1d,
};
static const uint8_t ccmp6_enc[62] = {
    0x88, 0x52, 0xe1, 0x1f, 0x5a, 0xf2, 0x84, 0x30, 0xfd, 0xab, 0xbf, 0xf9, 0x43, 0xb9, 0xf9, 0xa6,
    0xab, 0x1d, 0x98, 0xc7, 0xfe, 0x73, 0x50, 0x71, 0x3d, 0x6a, 0x89, 0x89, 0x00, 0x60, 0xa4, 0xec,
    0x81, 0x6b, 0x9a, 0x70, 0x9b, 0x60, 0xa3, 0x9d, 0x40, 0xb1, 0xdf, 0xb6, 0x12, 0xe1, 0x8b, 0x5f,
    0x11, 0x4b, 0xad, 0xb6, 0xcc, 0x86, 0x30, 0x9a, 0x8d, 0x5c, 0x46, 0x6b, 0xbb, 0x71,
};
static const uint8_t ccmp7_tk[16] = {
    0x1b, 0xdb, 0x34, 0x98, 0x0e, 0x03, 0x81, 0x24, 0xa1, 0xdb, 0x1a, 0x89, 0x2b, 0xec, 0x36, 0x6a,
};
static const uint8_t ccmp7_plain[44] = {
    0x18, 0x79, 0x81, 0x46, 0x9b, 0x50, 0xf4, 0xfd, 0x56, 0xf6, 0xef, 0xec, 0x95, 0x20, 0x16,
    0x91, 0x83, 0x57, 0x0c, 0x4c, 0xcd, 0xee, 0x20, 0xa0, 0x98, 0xbe, 0xca, 0x86, 0xf4, 0xb3,
    0x8d, 0xa2, 0x0c, 0xfd, 0xf2, 0x47, 0x24, 0xc5, 0x8e, 0xb8, 0x35, 0x66, 0x53, 0x39,
};
static const uint8_t ccmp7_enc[60] = {
    0x18, 0x79, 0x81, 0x46, 0x9b, 0x50, 0xf4, 0xfd, 0x56, 0xf6, 0xef, 0xec, 0x95, 0x20, 0x16,
    0x91, 0x83, 0x57, 0x0c, 0x4c, 0xcd, 0xee, 0x20, 0xa0, 0x23, 0xe7, 0x00, 0xe0, 0x73, 0x40,
    0xec, 0x5e, 0x12, 0xc5, 0x37, 0xeb, 0xf3, 0xab, 0x58, 0x4e, 0xf1, 0xfe, 0xf9, 0xa1, 0xf3,
    0x54, 0x7a, 0x8c, 0x13, 0xb3, 0x22, 0x5a, 0x2d, 0x09, 0x57, 0xec, 0xfa, 0xbe, 0x95, 0xb9,
};
static const uint8_t tkip_plain[124] = {
    0x08, 0x42, 0x2c, 0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xd0, 0x02, 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00,
    0x45, 0x00, 0x00, 0x54, 0x00, 0x00, 0x40, 0x00, 0x40, 0x01, 0xa5, 0x55, 0xc0, 0xa8, 0x0a, 0x02,
    0xc0, 0xa8, 0x0a, 0x01, 0x08, 0x00, 0x3a, 0xb0, 0x00, 0x00, 0x00, 0x00, 0xcd, 0x4c, 0x05, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
    0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,
    0x34, 0x35, 0x36, 0x37, 0x68, 0x81, 0xa3, 0xf3, 0xd6, 0x48, 0xd0, 0x3c,
};
static const uint8_t tkip_enc[136] = {
    0x08, 0x42, 0x2c, 0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xd0, 0x02, 0x00, 0x20, 0x01, 0x20, 0x00, 0x00, 0x00, 0x00,
    0xc0, 0x0e, 0x14, 0xfc, 0xe7, 0xcf, 0xab, 0xc7, 0x75, 0x47, 0xe6, 0x66, 0xe5, 0x7c, 0x0d, 0xac,
    0x70, 0x4a, 0x1e, 0x35, 0x8a, 0x88, 0xc1, 0x1c, 0x8e, 0x2e, 0x28, 0x2e, 0x38, 0x01, 0x02, 0x7a,
    0x46, 0x56, 0x05, 0x5e, 0xe9, 0x3e, 0x9c, 0x25, 0x47, 0x02, 0xe9, 0x73, 0x58, 0x05, 0xdd, 0xb5,
    0x76, 0x9b, 0xa7, 0x3f, 0x1e, 0xbb, 0x56, 0xe8, 0x44, 0xef, 0x91, 0x22, 0x85, 0xd3, 0xdd, 0x6e,
    0x54, 0x1e, 0x82, 0x38, 0x73, 0x55, 0x8a, 0xdb, 0xa0, 0x79, 0x06, 0x8a, 0xbd, 0x7f, 0x7f, 0x50,
    0x95, 0x96, 0x75, 0xac, 0xc4, 0xb4, 0xde, 0x9a, 0xa9, 0x9c, 0x05, 0xf2, 0x89, 0xa7, 0xc5, 0x2f,
    0xee, 0x5b, 0xfc, 0x14, 0xf6, 0xf8, 0xe5, 0xf8,
};

/** The TKIP vector of Annex J (M.6.3 in 802.11-2012): its phase 1 words
 * and RC4 key, from FreeBSD's tools/regression/net80211/tkip/test_tkip.c. */
static const uint16_t tkip_ttak[5] = {0x58bb, 0x1f07, 0x939e, 0x38b4, 0x4b25};
static const uint8_t tkip_rc4key[16] = {0x00, 0x20, 0x01, 0x4c, 0xfe, 0x67, 0xbe, 0xd2,
                                        0x7c, 0x86, 0x7b, 0x1b, 0xf8, 0x02, 0x8b, 0x1c};

/** Check AES-128 on FIPS-197 C.1 and B and on SP 800-38A F.1.1 (ECB). */
static void aes(void)
{
    static const char *const ecb[4][2] = {
        {"6bc1bee22e409f96e93d7e117393172a", "3ad77bb40d7a3660a89ecaf32466ef97"},
        {"ae2d8a571e03ac9c9eb76fac45af8e51", "f5d3d58503b9699de785895a96fdbaaf"},
        {"30c81c46a35ce411e5fbc1191a0a52ef", "43b1cd7f598ece23881b00e3ed030688"},
        {"f69f2445df4f9b17ad2b417be66c3710", "7b0c785e27e8ad3f8223207104725dd4"},
    };
    struct wlan_aes128 c;
    uint8_t k[16], p[16], want[16], out[16], a[16], b[16];
    unsigned i;

    hex(k, "000102030405060708090a0b0c0d0e0f");
    hex(p, "00112233445566778899aabbccddeeff");
    hex(want, "69c4e0d86a7b0430d8cdb78070b4c55a");
    wlan_aes128_setkey(&c, k);
    wlan_aes128_encrypt(&c, p, out);
    CHECK(equal(out, want, 16));
    wlan_aes128_encrypt(&c, p, p);
    CHECK(equal(p, want, 16));

    hex(k, "2b7e151628aed2a6abf7158809cf4f3c");
    hex(p, "3243f6a8885a308d313198a2e0370734");
    hex(want, "3925841d02dc09fbdc118597196a0b32");
    wlan_aes128_setkey(&c, k);
    wlan_aes128_encrypt(&c, p, out);
    CHECK(equal(out, want, 16));
    for (i = 0; i < 4; ++i) {
        hex(p, ecb[i][0]);
        hex(want, ecb[i][1]);
        wlan_aes128_encrypt(&c, p, out);
        CHECK(equal(out, want, 16));
    }
    hex(a, ecb[0][0]);
    hex(b, ecb[3][0]);
    wcrypto_aes2(&c, a, b);
    hex(want, ecb[0][1]);
    CHECK(equal(a, want, 16));
    hex(want, ecb[3][1]);
    CHECK(equal(b, want, 16));
}

/** Check AES-CMAC on SP 800-38B D.1, whole and in uneven pieces. */
static void cmac(void)
{
    static const unsigned lens[4] = {0, 16, 40, 64};
    static const char *const tags[4] = {
        "bb1d6929e95937287fa37d129b756746",
        "070a16b46b4d4144f79bdd9dd04a287c",
        "dfa66747de9ae63030ca32611497c827",
        "51f0bebf7e3b9d92fc49741779363cfe",
    };
    struct wlan_aes128 c;
    struct wcrypto_cmac st;
    uint8_t k[16], m[64], want[16], out[16];
    unsigned i;

    hex(k, "2b7e151628aed2a6abf7158809cf4f3c");
    hex(m, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
           "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    wlan_aes128_setkey(&c, k);
    for (i = 0; i < 4; ++i) {
        hex(want, tags[i]);
        wlan_aes128_cmac(&c, i ? m : 0, lens[i], out);
        CHECK(equal(out, want, 16));
    }
    wcrypto_cmac_init(&st, &c);
    wcrypto_cmac_update(&st, m, 7);
    wcrypto_cmac_update(&st, 0, 0);
    wcrypto_cmac_update(&st, m + 7, 9);
    wcrypto_cmac_update(&st, m + 16, 24);
    wcrypto_cmac_final(&st, out);
    hex(want, tags[2]);
    CHECK(equal(out, want, 16));
}

/** Encrypt plain (Protected set in its header) with pn and key_id, expect
 * enc, and decrypt it back. */
static void ccmp_vector(const uint8_t *tk, const uint8_t *plain, unsigned plain_len,
                        const uint8_t *enc, unsigned enc_len, uint64_t pn, unsigned key_id)
{
    struct wlan_aes128 c;
    uint8_t f[128], clear[128];
    unsigned len, kid;
    uint64_t rx_pn;

    wlan_aes128_setkey(&c, tk);
    copy(clear, plain, plain_len);
    clear[1] &= (uint8_t)~0x40;
    copy(f, clear, plain_len);
    len = plain_len;
    CHECK(wlan_ccmp_encrypt(&c, f, &len, sizeof(f), pn, key_id) == 0);
    CHECK(len == enc_len && equal(f, enc, enc_len));

    copy(f, enc, enc_len);
    len = enc_len;
    rx_pn = 0;
    kid = 9;
    CHECK(wlan_ccmp_decrypt(&c, f, &len, &rx_pn, &kid) == 0);
    CHECK(len == plain_len && equal(f, clear, plain_len));
    CHECK(rx_pn == pn && kid == key_id);
}

/** Check CCMP on its vectors, on a management frame, and its refusals,
 * each of which leaves the frame and the outputs as they were. */
static void ccmp(void)
{
    static const uint8_t mgmt_tk[16] = {0x66, 0xed, 0x21, 0x04, 0x2f, 0x9f, 0x26, 0xd7,
                                        0x11, 0x57, 0x06, 0xe4, 0x04, 0x14, 0xcf, 0x2e};
    struct wlan_aes128 c;
    uint8_t f[128], g[128], mgmt[26], mgmt_enc[42];
    unsigned len, kid, i;
    uint64_t pn;

    ccmp_vector(ccmp1_tk, ccmp1_plain, sizeof(ccmp1_plain), ccmp1_enc, sizeof(ccmp1_enc),
                0xB5039776E70CULL, 0);
    ccmp_vector(ccmp2_tk, ccmp2_plain, sizeof(ccmp2_plain), ccmp2_enc, sizeof(ccmp2_enc),
                0x31F3CBBA97EAULL, 2);
    ccmp_vector(ccmp6_tk, ccmp6_plain, sizeof(ccmp6_plain), ccmp6_enc, sizeof(ccmp6_enc),
                118205765159305ULL, 1);
    ccmp_vector(ccmp7_tk, ccmp7_plain, sizeof(ccmp7_plain), ccmp7_enc, sizeof(ccmp7_enc),
                0x5EEC4073E723ULL, 3);

    /* Annex J's unicast Deauthentication (M.9.2 in 802.11-2012) gives no
     * output here; this one is OpenSSL's AES-CCM over the same nonce and
     * AAD, which sets the nonce's Management bit. */
    hex(mgmt, "c00000000200000001000200000000000200000000006000"
              "0200");
    hex(mgmt_enc, "c040000002000000010002000000000002000000000060000100002000000000"
                  "1d07cafd0409bb8bafef");
    ccmp_vector(mgmt_tk, mgmt, sizeof(mgmt), mgmt_enc, sizeof(mgmt_enc), 1, 0);

    wlan_aes128_setkey(&c, ccmp1_tk);
    for (i = 0; i < sizeof(ccmp1_enc); ++i) {
        copy(f, ccmp1_enc, sizeof(ccmp1_enc));
        f[i] ^= 0x01;
        copy(g, f, sizeof(ccmp1_enc));
        len = sizeof(ccmp1_enc);
        pn = 7;
        kid = 7;
        /* Duration, the sequence number and the CCMP header's reserved
         * bits are outside the MIC; a version of 1 is refused. */
        if (i == 2 || i == 3 || i == 23 || i == 26 || i == 27)
            continue;
        CHECK(wlan_ccmp_decrypt(&c, f, &len, &pn, &kid) == (i == 0 ? -EINVAL : -EBADMSG));
        CHECK(equal(f, g, sizeof(ccmp1_enc)) && len == sizeof(ccmp1_enc));
        CHECK(pn == 7 && kid == 7);
    }
    copy(f, ccmp1_enc, sizeof(ccmp1_enc));
    f[1] ^= 0x08 | 0x10 | 0x20; /* Retry, Power Management, More Data */
    f[22] ^= 0xf0;
    f[23] ^= 0xff;
    len = sizeof(ccmp1_enc);
    CHECK(wlan_ccmp_decrypt(&c, f, &len, &pn, &kid) == 0);

    copy(f, ccmp1_enc, sizeof(ccmp1_enc));
    f[27] &= (uint8_t)~0x20;
    len = sizeof(ccmp1_enc);
    CHECK(wlan_ccmp_decrypt(&c, f, &len, &pn, &kid) == -EINVAL);
    copy(f, ccmp1_enc, sizeof(ccmp1_enc));
    len = 24 + 8 + 7;
    CHECK(wlan_ccmp_decrypt(&c, f, &len, &pn, &kid) == -EINVAL && len == 39);
    copy(f, ccmp1_enc, sizeof(ccmp1_enc));
    f[1] &= (uint8_t)~0x40;
    len = sizeof(ccmp1_enc);
    CHECK(wlan_ccmp_decrypt(&c, f, &len, &pn, &kid) == -EINVAL);

    copy(f, ccmp1_plain, sizeof(ccmp1_plain));
    f[1] &= (uint8_t)~0x40;
    copy(g, f, sizeof(ccmp1_plain));
    len = sizeof(ccmp1_plain);
    CHECK(wlan_ccmp_encrypt(&c, f, &len, len + 15, 1, 0) == -ENOSPC);
    CHECK(wlan_ccmp_encrypt(&c, f, &len, sizeof(f), 0, 0) == -EINVAL);
    CHECK(wlan_ccmp_encrypt(&c, f, &len, sizeof(f), 0x1000000000000ULL, 0) == -EINVAL);
    CHECK(wlan_ccmp_encrypt(&c, f, &len, sizeof(f), 1, 4) == -EINVAL);
    CHECK(wlan_ccmp_encrypt(&c, f, &len, len - 1, 1, 0) == -EINVAL);
    f[1] |= 0x40;
    CHECK(wlan_ccmp_encrypt(&c, f, &len, sizeof(f), 1, 0) == -EINVAL);
    f[1] &= (uint8_t)~0x40;
    f[0] = 0xd4; /* an ACK, a control frame */
    CHECK(wlan_ccmp_encrypt(&c, f, &len, sizeof(f), 1, 0) == -EINVAL);
    f[0] = g[0];
    CHECK(len == sizeof(ccmp1_plain) && equal(f, g, sizeof(ccmp1_plain)));
    len = 23;
    CHECK(wlan_ccmp_encrypt(&c, f, &len, sizeof(f), 1, 0) == -EINVAL && len == 23);

    /* A QoS null frame: a header and no body, still authenticated. */
    hex(f, "c8010000020000000001020000000002020000000003100005"
           "00");
    len = 26;
    CHECK(wlan_ccmp_encrypt(&c, f, &len, sizeof(f), 5, 1) == 0 && len == 42);
    CHECK(wlan_ccmp_decrypt(&c, f, &len, &pn, &kid) == 0 && len == 26 && pn == 5 && kid == 1);
    CHECK(f[1] == 0x01 && f[24] == 0x05);
}

/** Check BIP-CMAC-128 on Annex J's broadcast Deauthentication (M.9.1 in
 * 802.11-2012; MIC also in Broadcom's wlc_security.c), and its refusals. */
static void bip(void)
{
    static const uint8_t igtk[16] = {0x4e, 0xa9, 0x54, 0x3e, 0x09, 0xcf, 0x2b, 0x1e,
                                     0xca, 0x66, 0xff, 0xc5, 0x8b, 0xde, 0xcb, 0xcf};
    struct wlan_aes128 c;
    uint8_t want[44], f[64], g[64];
    unsigned len, kid, i;
    uint64_t ipn;

    hex(want, "c0000000ffffffffffff020000000000020000000000090002004c10040004000000"
              "000048dfbfa7b8278872");
    wlan_aes128_setkey(&c, igtk);
    copy(f, want, 26);
    len = 26;
    CHECK(wlan_bip_sign(&c, f, &len, sizeof(f), 4, 4) == 0);
    CHECK(len == 44 && equal(f, want, 44));
    ipn = 0;
    kid = 0;
    CHECK(wlan_bip_check(&c, f, &len, &ipn, &kid) == 0);
    CHECK(len == 26 && ipn == 4 && kid == 4 && equal(f, want, 26));

    copy(f, want, 44);
    f[1] |= 0x08 | 0x10 | 0x20;
    len = 44;
    CHECK(wlan_bip_check(&c, f, &len, &ipn, &kid) == 0 && len == 26);
    for (i = 0; i < 44; ++i) {
        /* Duration and the sequence control are outside the MIC. */
        if (i == 2 || i == 3 || i == 22 || i == 23)
            continue;
        copy(f, want, 44);
        f[i] ^= 0x02;
        copy(g, f, 44);
        len = 44;
        ipn = 9;
        kid = 9;
        if (i == 0 || (i >= 26 && i < 30))
            CHECK(wlan_bip_check(&c, f, &len, &ipn, &kid) == -EINVAL);
        else
            CHECK(wlan_bip_check(&c, f, &len, &ipn, &kid) == -EBADMSG);
        CHECK(len == 44 && ipn == 9 && kid == 9 && equal(f, g, 44));
    }

    copy(f, want, 26);
    len = 26;
    CHECK(wlan_bip_sign(&c, f, &len, 43, 4, 4) == -ENOSPC);
    CHECK(wlan_bip_sign(&c, f, &len, sizeof(f), 4, 3) == -EINVAL);
    CHECK(wlan_bip_sign(&c, f, &len, sizeof(f), 0, 4) == -EINVAL);
    CHECK(wlan_bip_sign(&c, f, &len, sizeof(f), 0x1000000000000ULL, 5) == -EINVAL);
    f[4] = 0x02;
    CHECK(wlan_bip_sign(&c, f, &len, sizeof(f), 4, 4) == -EINVAL);
    copy(g, want, 44);
    g[4] = 0x02;
    len = 44;
    CHECK(wlan_bip_check(&c, g, &len, &ipn, &kid) == -EINVAL && len == 44);
    len = 26;
    f[4] = 0xff;
    f[1] = 0x40;
    CHECK(wlan_bip_sign(&c, f, &len, sizeof(f), 4, 4) == -EINVAL);
    f[1] = 0;
    f[0] = 0x08;
    CHECK(wlan_bip_sign(&c, f, &len, sizeof(f), 4, 4) == -EINVAL);
    f[0] = 0xc0;
    CHECK(len == 26 && equal(f, want, 26));
    CHECK(wlan_bip_sign(&c, f, &len, sizeof(f), 0xffffffffffffULL, 5) == 0 && len == 44);
    CHECK(wlan_bip_check(&c, f, &len, &ipn, &kid) == 0);
    CHECK(ipn == 0xffffffffffffULL && kid == 5);
}

/** Compute the reflected CRC-32 of IEEE 802.3, which TKIP's ICV is. */
static uint32_t crc32(const uint8_t *p, unsigned n)
{
    uint32_t c;
    unsigned k;

    c = 0xffffffffu;
    while (n-- > 0) {
        c ^= *p++;
        for (k = 0; k < 8; ++k)
            c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

/** Check Michael on Annex J's vectors (Linux's michael_mic_tv_template),
 * RC4 on RFC 6229, and TKIP's mixing and whole MPDU on Annex J's. */
static void tkip(void)
{
    static const char *const mich[6][3] = {
        {"0000000000000000", "", "82925c1ca1d130b8"},
        {"82925c1ca1d130b8", "M", "434721ca40639b3f"},
        {"434721ca40639b3f", "Mi", "e8f9becae97e5d29"},
        {"e8f9becae97e5d29", "Mic", "90038fc6cf13c1db"},
        {"90038fc6cf13c1db", "Mich", "d55e100510128986"},
        {"d55e100510128986", "Michael", "0a942b124ecaa546"},
    };
    uint8_t key[16], want[32], out[32], tk[32], f[160];
    uint16_t ttak[5];
    unsigned i, n, msdu;
    uint32_t icv;

    for (i = 0; i < 6; ++i) {
        const char *s;

        hex(key, mich[i][0]);
        hex(want, mich[i][2]);
        s = mich[i][1];
        for (n = 0; s[n]; ++n)
            f[n] = (uint8_t)s[n];
        wcrypto_michael(key, n ? f : 0, n, out);
        CHECK(equal(out, want, 8));
    }

    for (i = 0; i < 16; ++i)
        key[i] = (uint8_t)(i + 1);
    for (i = 0; i < 32; ++i)
        out[i] = 0;
    wlan_tkip_rc4(key, out, 32);
    hex(want, "9ac7cc9a609d1ef7b2932899cde41b975248c4959014126a6e8a84f11d1a9e1c");
    CHECK(equal(out, want, 32));
    wlan_tkip_rc4(key, 0, 0);

    hex(tk, "12345678901234567890123456789012"
            "34567890123456789012345678901234");
    msdu = sizeof(tkip_plain) - 24 - 8;
    wlan_tkip_michael(tk + 16, tkip_plain + 4, tkip_plain + 16, 0, tkip_plain + 24, msdu, out);
    CHECK(equal(out, tkip_plain + 24 + msdu, 8));

    wlan_tkip_mix1(tk, tkip_plain + 10, 0, ttak);
    for (i = 0; i < 5; ++i)
        CHECK(ttak[i] == tkip_ttak[i]);
    wlan_tkip_mix2(tk, ttak, 1, key);
    CHECK(equal(key, tkip_rc4key, 16));

    copy(f, tkip_plain, 24);
    f[24] = key[0];
    f[25] = key[1];
    f[26] = key[2];
    f[27] = 0x20; /* Ext IV, key 0 */
    f[28] = f[29] = f[30] = f[31] = 0;
    copy(f + 32, tkip_plain + 24, msdu + 8);
    icv = crc32(f + 32, msdu + 8);
    for (i = 0; i < 4; ++i)
        f[32 + msdu + 8 + i] = (uint8_t)(icv >> (8 * i));
    wlan_tkip_rc4(key, f + 32, msdu + 12);
    CHECK(sizeof(tkip_enc) == 32 + msdu + 12 && equal(f, tkip_enc, sizeof(tkip_enc)));

    wlan_tkip_rc4(key, f + 32, msdu + 12);
    CHECK(crc32(f + 32, msdu + 8) == icv);
    CHECK(equal(f + 32, tkip_plain + 24, msdu + 8));
}

/** Run every cipher check without files or processes. */
int main(void)
{
    aes();
    cmac();
    ccmp();
    bip();
    tkip();
    if (failures)
        return 1;
    printf("wcrypto_hosttest: result: passed\n");
    return 0;
}
