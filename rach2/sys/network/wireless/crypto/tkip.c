/*
 * tkip.c: TKIP's parts for WPA networks: the Michael MIC, the two phases of
 * per-packet key mixing and RC4, as IEEE 802.11-2020 12.5.2 defines them.
 *
 * The mixing follows 12.5.2.5 and the layout of Linux's
 * tkip_mixing_phase1 and tkip_mixing_phase2 (net/mac80211/tkip.c). Its
 * S-box is the standard's table, whose entry i is 2*S[i] in the high byte
 * and 3*S[i] in the low one, S being the AES S-box. TKIP and RC4 index
 * tables by key bytes by design; WPA keeps TKIP only for networks that
 * offer nothing newer, which is also why the AES rule does not apply here.
 * The caller computes and checks the ICV, a CRC-32 of the MSDU and MIC.
 */
#include "wcrypto_int.h"

static const uint16_t tkip_sbox[256] = {
    0xC6A5, 0xF884, 0xEE99, 0xF68D, 0xFF0D, 0xD6BD, 0xDEB1, 0x9154, 0x6050, 0x0203, 0xCEA9, 0x567D,
    0xE719, 0xB562, 0x4DE6, 0xEC9A, 0x8F45, 0x1F9D, 0x8940, 0xFA87, 0xEF15, 0xB2EB, 0x8EC9, 0xFB0B,
    0x41EC, 0xB367, 0x5FFD, 0x45EA, 0x23BF, 0x53F7, 0xE496, 0x9B5B, 0x75C2, 0xE11C, 0x3DAE, 0x4C6A,
    0x6C5A, 0x7E41, 0xF502, 0x834F, 0x685C, 0x51F4, 0xD134, 0xF908, 0xE293, 0xAB73, 0x6253, 0x2A3F,
    0x080C, 0x9552, 0x4665, 0x9D5E, 0x3028, 0x37A1, 0x0A0F, 0x2FB5, 0x0E09, 0x2436, 0x1B9B, 0xDF3D,
    0xCD26, 0x4E69, 0x7FCD, 0xEA9F, 0x121B, 0x1D9E, 0x5874, 0x342E, 0x362D, 0xDCB2, 0xB4EE, 0x5BFB,
    0xA4F6, 0x764D, 0xB761, 0x7DCE, 0x527B, 0xDD3E, 0x5E71, 0x1397, 0xA6F5, 0xB968, 0x0000, 0xC12C,
    0x4060, 0xE31F, 0x79C8, 0xB6ED, 0xD4BE, 0x8D46, 0x67D9, 0x724B, 0x94DE, 0x98D4, 0xB0E8, 0x854A,
    0xBB6B, 0xC52A, 0x4FE5, 0xED16, 0x86C5, 0x9AD7, 0x6655, 0x1194, 0x8ACF, 0xE910, 0x0406, 0xFE81,
    0xA0F0, 0x7844, 0x25BA, 0x4BE3, 0xA2F3, 0x5DFE, 0x80C0, 0x058A, 0x3FAD, 0x21BC, 0x7048, 0xF104,
    0x63DF, 0x77C1, 0xAF75, 0x4263, 0x2030, 0xE51A, 0xFD0E, 0xBF6D, 0x814C, 0x1814, 0x2635, 0xC32F,
    0xBEE1, 0x35A2, 0x88CC, 0x2E39, 0x9357, 0x55F2, 0xFC82, 0x7A47, 0xC8AC, 0xBAE7, 0x322B, 0xE695,
    0xC0A0, 0x1998, 0x9ED1, 0xA37F, 0x4466, 0x547E, 0x3BAB, 0x0B83, 0x8CCA, 0xC729, 0x6BD3, 0x283C,
    0xA779, 0xBCE2, 0x161D, 0xAD76, 0xDB3B, 0x6456, 0x744E, 0x141E, 0x92DB, 0x0C0A, 0x486C, 0xB8E4,
    0x9F5D, 0xBD6E, 0x43EF, 0xC4A6, 0x39A8, 0x31A4, 0xD337, 0xF28B, 0xD532, 0x8B43, 0x6E59, 0xDAB7,
    0x018C, 0xB164, 0x9CD2, 0x49E0, 0xD8B4, 0xACFA, 0xF307, 0xCF25, 0xCAAF, 0xF48E, 0x47E9, 0x1018,
    0x6FD5, 0xF088, 0x4A6F, 0x5C72, 0x3824, 0x57F1, 0x73C7, 0x9751, 0xCB23, 0xA17C, 0xE89C, 0x3E21,
    0x96DD, 0x61DC, 0x0D86, 0x0F85, 0xE090, 0x7C42, 0x71C4, 0xCCAA, 0x90D8, 0x0605, 0xF701, 0x1C12,
    0xC2A3, 0x6A5F, 0xAEF9, 0x69D0, 0x1791, 0x9958, 0x3A27, 0x27B9, 0xD938, 0xEB13, 0x2BB3, 0x2233,
    0xD2BB, 0xA970, 0x0789, 0x33A7, 0x2DB6, 0x3C22, 0x1592, 0xC920, 0x8749, 0xAAFF, 0x5078, 0xA57A,
    0x038F, 0x59F8, 0x0980, 0x1A17, 0x65DA, 0xD731, 0x84C6, 0xD0B8, 0x82C3, 0x29B0, 0x5A77, 0x1E11,
    0x7BCB, 0xA8FC, 0x6DD6, 0x2C3A,
};

/* Michael's state over a message given in pieces. */
struct michael {
    uint32_t l, r;
    uint32_t m;      /* bytes of the word being gathered, little endian */
    unsigned nbytes; /* bytes in m, 0 to 3 */
};

/** Rotate left by n, 0 < n < 32. */
static uint32_t rol32(uint32_t x, unsigned n)
{
    return x << n | x >> (32 - n);
}

/** Mix one 32-bit word into the state: Michael's block function b. */
static void michael_block(struct michael *s, uint32_t w)
{
    uint32_t l, r;

    l = s->l ^ w;
    r = s->r;
    r ^= rol32(l, 17);
    l += r;
    r ^= ((l & 0xff00ff00u) >> 8) | ((l & 0x00ff00ffu) << 8);
    l += r;
    r ^= rol32(l, 3);
    l += r;
    r ^= rol32(l, 30);
    l += r;
    s->l = l;
    s->r = r;
}

/** Start Michael under an 8-byte key. */
static void michael_init(struct michael *s, const uint8_t key[8])
{
    s->l =
        (uint32_t)key[0] | (uint32_t)key[1] << 8 | (uint32_t)key[2] << 16 | (uint32_t)key[3] << 24;
    s->r =
        (uint32_t)key[4] | (uint32_t)key[5] << 8 | (uint32_t)key[6] << 16 | (uint32_t)key[7] << 24;
    s->m = 0;
    s->nbytes = 0;
}

/** Add len bytes of the message. */
static void michael_update(struct michael *s, const uint8_t *data, unsigned len)
{
    while (len-- > 0) {
        s->m |= (uint32_t)*data++ << (8 * s->nbytes);
        if (++s->nbytes == 4) {
            michael_block(s, s->m);
            s->m = 0;
            s->nbytes = 0;
        }
    }
}

/** Pad with 0x5a and four to seven zeros, and write the MIC as L then R. */
static void michael_final(struct michael *s, uint8_t mic[8])
{
    michael_block(s, s->m | (uint32_t)0x5a << (8 * s->nbytes));
    michael_block(s, 0);
    mic[0] = (uint8_t)s->l;
    mic[1] = (uint8_t)(s->l >> 8);
    mic[2] = (uint8_t)(s->l >> 16);
    mic[3] = (uint8_t)(s->l >> 24);
    mic[4] = (uint8_t)s->r;
    mic[5] = (uint8_t)(s->r >> 8);
    mic[6] = (uint8_t)(s->r >> 16);
    mic[7] = (uint8_t)(s->r >> 24);
    wcrypto_wipe(s, sizeof(*s));
}

/** Compute Michael over one contiguous message. */
void wcrypto_michael(const uint8_t key[8], const uint8_t *data, unsigned len, uint8_t mic[8])
{
    struct michael s;

    michael_init(&s, key);
    michael_update(&s, data, len);
    michael_final(&s, mic);
}

/** Compute the TKIP MIC over its pseudo-header and the MSDU. */
void wlan_tkip_michael(const uint8_t key[8], const uint8_t da[6], const uint8_t sa[6],
                       unsigned priority, const uint8_t *data, unsigned len, uint8_t mic[8])
{
    struct michael s;
    uint8_t pad[4];

    pad[0] = (uint8_t)priority;
    pad[1] = pad[2] = pad[3] = 0;
    michael_init(&s, key);
    michael_update(&s, da, 6);
    michael_update(&s, sa, 6);
    michael_update(&s, pad, 4);
    michael_update(&s, data, len);
    michael_final(&s, mic);
}

/** Read the little-endian 16-bit word at byte i of the temporal key. */
static uint16_t tk16(const uint8_t tk[16], unsigned i)
{
    return (uint16_t)(tk[i] | tk[i + 1] << 8);
}

/** TKIP's 16-bit S-box, built from two lookups of the 8-bit table. */
static uint16_t tkip_s(uint16_t v)
{
    uint16_t hi;

    hi = tkip_sbox[v >> 8];
    return (uint16_t)(tkip_sbox[v & 0xff] ^ (uint16_t)(hi >> 8 | hi << 8));
}

/** TKIP's first phase: mix TK, TA and IV32 into TTAK in eight rounds. */
void wlan_tkip_mix1(const uint8_t tk[16], const uint8_t ta[6], uint32_t iv32, uint16_t ttak[5])
{
    unsigned i, j;

    ttak[0] = (uint16_t)iv32;
    ttak[1] = (uint16_t)(iv32 >> 16);
    ttak[2] = (uint16_t)(ta[0] | ta[1] << 8);
    ttak[3] = (uint16_t)(ta[2] | ta[3] << 8);
    ttak[4] = (uint16_t)(ta[4] | ta[5] << 8);
    for (i = 0; i < 8; i++) {
        j = 2 * (i & 1);
        ttak[0] = (uint16_t)(ttak[0] + tkip_s((uint16_t)(ttak[4] ^ tk16(tk, j))));
        ttak[1] = (uint16_t)(ttak[1] + tkip_s((uint16_t)(ttak[0] ^ tk16(tk, 4 + j))));
        ttak[2] = (uint16_t)(ttak[2] + tkip_s((uint16_t)(ttak[1] ^ tk16(tk, 8 + j))));
        ttak[3] = (uint16_t)(ttak[3] + tkip_s((uint16_t)(ttak[2] ^ tk16(tk, 12 + j))));
        ttak[4] = (uint16_t)(ttak[4] + tkip_s((uint16_t)(ttak[3] ^ tk16(tk, j))) + i);
    }
}

/** Rotate a 16-bit word right by one. */
static uint16_t ror16(uint16_t x)
{
    return (uint16_t)(x >> 1 | x << 15);
}

/** TKIP's second phase: mix TTAK, TK and IV16 into the RC4 key of one
 * packet, whose first three bytes are the WEP-style IV with its weak-key bit
 * cleared. */
void wlan_tkip_mix2(const uint8_t tk[16], const uint16_t ttak[5], uint16_t iv16, uint8_t rc4key[16])
{
    uint16_t ppk[6];
    unsigned i;

    for (i = 0; i < 5; i++)
        ppk[i] = ttak[i];
    ppk[5] = (uint16_t)(ttak[4] + iv16);

    ppk[0] = (uint16_t)(ppk[0] + tkip_s((uint16_t)(ppk[5] ^ tk16(tk, 0))));
    ppk[1] = (uint16_t)(ppk[1] + tkip_s((uint16_t)(ppk[0] ^ tk16(tk, 2))));
    ppk[2] = (uint16_t)(ppk[2] + tkip_s((uint16_t)(ppk[1] ^ tk16(tk, 4))));
    ppk[3] = (uint16_t)(ppk[3] + tkip_s((uint16_t)(ppk[2] ^ tk16(tk, 6))));
    ppk[4] = (uint16_t)(ppk[4] + tkip_s((uint16_t)(ppk[3] ^ tk16(tk, 8))));
    ppk[5] = (uint16_t)(ppk[5] + tkip_s((uint16_t)(ppk[4] ^ tk16(tk, 10))));
    ppk[0] = (uint16_t)(ppk[0] + ror16((uint16_t)(ppk[5] ^ tk16(tk, 12))));
    ppk[1] = (uint16_t)(ppk[1] + ror16((uint16_t)(ppk[0] ^ tk16(tk, 14))));
    ppk[2] = (uint16_t)(ppk[2] + ror16(ppk[1]));
    ppk[3] = (uint16_t)(ppk[3] + ror16(ppk[2]));
    ppk[4] = (uint16_t)(ppk[4] + ror16(ppk[3]));
    ppk[5] = (uint16_t)(ppk[5] + ror16(ppk[4]));

    rc4key[0] = (uint8_t)(iv16 >> 8);
    rc4key[1] = (uint8_t)(((iv16 >> 8) | 0x20) & 0x7f);
    rc4key[2] = (uint8_t)iv16;
    rc4key[3] = (uint8_t)((ppk[5] ^ tk16(tk, 0)) >> 1);
    for (i = 0; i < 6; i++) {
        rc4key[4 + 2 * i] = (uint8_t)ppk[i];
        rc4key[5 + 2 * i] = (uint8_t)(ppk[i] >> 8);
    }
    wcrypto_wipe(ppk, sizeof(ppk));
}

/** XOR len bytes with the RC4 keystream of a 16-byte key. */
void wlan_tkip_rc4(const uint8_t key[16], uint8_t *data, unsigned len)
{
    uint8_t s[256], t;
    unsigned i, j, n;

    for (i = 0; i < 256; i++)
        s[i] = (uint8_t)i;
    for (i = 0, j = 0; i < 256; i++) {
        j = (j + s[i] + key[i & 15]) & 0xff;
        t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
    for (n = 0, i = 0, j = 0; n < len; n++) {
        i = (i + 1) & 0xff;
        j = (j + s[i]) & 0xff;
        t = s[i];
        s[i] = s[j];
        s[j] = t;
        data[n] ^= s[(s[i] + s[j]) & 0xff];
    }
    wcrypto_wipe(s, sizeof(s));
}
