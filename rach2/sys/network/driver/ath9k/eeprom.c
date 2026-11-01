/*
 * eeprom.c: the AR9300 EEPROM of the ath9k driver: the card's calibration,
 * read from its EEPROM or OTP as compressed blocks over a board template,
 * and what the driver takes from it.
 *
 * The reading, unpacking and lookups follow ath9k's ar9003_eeprom.c, pci.c's
 * ath_pci_eeprom_read and eeprom.c (Linux v6.16): ar9300_read_eeprom,
 * ar9300_read_otp, ar9300_comp_hdr_unpack, ar9300_uncompress_block,
 * ar9300_compress_decision, ar9300_eeprom_restore_internal, the target
 * power and edge power lookups, and ath9k_hw_ar9300_set_board_values. Where
 * ath9k branches by chip, this file takes the path of the chips the table
 * has, which are of the AR9462's design, and says where another goes.
 */

#include "ath9k.h"
#include "ar9003_templates.h"
#include "../../../klog.h"
#include "../../../lib/kerrno.h"
#include "../../../lib/krandom.h"
#include "../../../lib/kstring.h"

/* eeprom.h's conformance-limit values: the most power an edge allows, the
 * group of a domain that names no conformance tests, an unused bin, and an
 * edge's power and in-band flag. */
#define MAX_RATE_POWER        63
#define SD_NO_CTL             0xE0
#define AR5416_BCHAN_UNUSED   0xFF
#define CTL_EDGE_TPOWER(_ctl) ((_ctl) & 0x3f)
#define CTL_EDGE_FLAGS(_ctl)  (((_ctl) >> 6) & 0x03)

/* The most blocks ar9300_eeprom_restore_internal reads (its MSTATE), and the
 * longest block it accepts with its header and checksum. */
#define EEPROM_MAX_BLOCKS 100
#define EEPROM_MAX_BLOCK  (COMP_HDR_LEN + 1023 + COMP_CKSUM_LEN)

/* The two stores an image may be read from, one byte at a time downward
 * from `address`, as ath9k's eeprom_read_op does. */
typedef int (*eeprom_read_op)(struct ath9k *ah, int address, uint8_t *buffer, int count);

/** Whether `freq` MHz is in the 2.4 GHz band. */
static int is_2ghz(unsigned freq)
{
    return freq < 4000;
}

/** The frequency of bin `fbin` (ath9k_hw_fbin2freq). */
static int fbin2freq(uint8_t fbin, int is2ghz)
{
    if (fbin == AR5416_BCHAN_UNUSED)
        return fbin;
    return is2ghz ? 2300 + fbin : 4800 + 5 * fbin;
}

/**
 * Read the 16-bit EEPROM word at `offset`, as ath_pci_eeprom_read does: a
 * read of the word's address starts it, and the status register holds the
 * word once its busy and protection bits clear. Returns 0 or -EIO.
 */
int ath9k_eeprom_read_word(struct ath9k *ah, unsigned offset, uint16_t *word)
{
    (void)REG_READ(ah, AR_EEPROM_WORD(offset));
    if (ath9k_wait(ah, AR_EEPROM_STATUS_DATA,
                   AR_EEPROM_STATUS_DATA_BUSY | AR_EEPROM_STATUS_DATA_PROT_ACCESS, 0,
                   ATH9K_WAIT_TIMEOUT) != 0)
        return -EIO;
    *word = (uint16_t)MS(REG_READ(ah, AR_EEPROM_STATUS_DATA), AR_EEPROM_STATUS_DATA_VAL);
    return 0;
}

/** Read the byte at `address` of the EEPROM, a little-endian word stream. */
static int eeprom_byte(struct ath9k *ah, int address, uint8_t *byte)
{
    uint16_t word;

    if (ath9k_eeprom_read_word(ah, (unsigned)address / 2, &word) != 0)
        return -EIO;
    *byte = (uint8_t)(word >> (8 * (address % 2)));
    return 0;
}

/**
 * Read `count` bytes of the EEPROM downward from `address`, as
 * ar9300_read_eeprom does: the byte at `address` goes to buffer[0]. From an
 * odd address on, each word gives two bytes, its high byte first. Returns 0
 * or -EIO.
 */
static int read_eeprom(struct ath9k *ah, int address, uint8_t *buffer, int count)
{
    uint16_t word;
    int i;

    if (address < 0 || (address + count) / 2 > AR9300_EEPROM_SIZE - 1)
        return -EIO;
    if (address % 2 == 0) {
        if (eeprom_byte(ah, address--, buffer++) != 0)
            return -EIO;
        count--;
    }
    for (i = 0; i < count / 2; i++) {
        if (ath9k_eeprom_read_word(ah, (unsigned)address / 2, &word) != 0)
            return -EIO;
        *buffer++ = (uint8_t)(word >> 8);
        *buffer++ = (uint8_t)word;
        address -= 2;
    }
    if (count % 2 && eeprom_byte(ah, address, buffer) != 0)
        return -EIO;
    return 0;
}

/** Read OTP word `addr` into *data (ar9300_otp_read_word). */
static int otp_word(struct ath9k *ah, int addr, uint32_t *data)
{
    (void)REG_READ(ah, AR9300_OTP_BASE + 4 * (uint32_t)addr);
    if (ath9k_wait(ah, AR9300_OTP_STATUS, AR9300_OTP_STATUS_TYPE, AR9300_OTP_STATUS_VALID, 1000) !=
        0)
        return -EIO;
    *data = REG_READ(ah, AR9300_OTP_READ_DATA);
    return 0;
}

/** Read `count` bytes of the OTP downward from `address` (ar9300_read_otp). */
static int read_otp(struct ath9k *ah, int address, uint8_t *buffer, int count)
{
    uint32_t data;
    int i;

    for (i = 0; i < count; i++) {
        if (address - i < 0 || otp_word(ah, (address - i) / 4, &data) != 0)
            return -EIO;
        buffer[i] = (uint8_t)(data >> (8 * ((address - i) % 4)));
    }
    return 0;
}

/** Whether a block header's four bytes are neither all zeros nor all ones,
 * which an empty or absent store reads (ar9300_check_header). */
static int header_ok(const uint8_t *h)
{
    int zeros = !h[0] && !h[1] && !h[2] && !h[3];
    int ones = h[0] == 0xff && h[1] == 0xff && h[2] == 0xff && h[3] == 0xff;

    return !zeros && !ones;
}

/** Whether `read` finds a block header at `address`. */
static int found_at(struct ath9k *ah, eeprom_read_op read, int address)
{
    uint8_t header[COMP_HDR_LEN];

    return read(ah, address, header, COMP_HDR_LEN) == 0 && header_ok(header);
}

/** The template whose templateVersion is `id`, or NULL. */
static const struct ar9300_eeprom *template_find(int id)
{
    unsigned i;

    for (i = 0; ar9300_templates[i]; i++)
        if (ar9300_templates[i]->templateVersion == id)
            return ar9300_templates[i];
    return NULL;
}

/**
 * Lay the pairs of a _CompressBlock block over `image`, as
 * ar9300_uncompress_block does: each pair is a skip from the end of the
 * last run and a run's length, then the run's bytes. Returns 0, or -EIO for
 * a run past the image's end.
 */
static int uncompress_block(uint8_t *image, const uint8_t *block, int size)
{
    int it, spot = 0, length;

    for (it = 0; it + 1 < size; it += length + 2) {
        spot += block[it];
        length = block[it + 1];
        if (length == 0)
            continue;
        if (spot + length > (int)sizeof(struct ar9300_eeprom) || it + 2 + length > size)
            return -EIO;
        k_memcpy(image + spot, block + it + 2, (size_t)length);
        spot += length;
    }
    return 0;
}

/**
 * Restore one checked block of `length` bytes after its header at `word`
 * into `image`, as ar9300_compress_decision does: a block of _CompressNone
 * is the whole image; one of _CompressBlock is runs over the template it
 * names, or over the image so far when it names none. Returns 0 or -EIO.
 */
static int restore_block(uint8_t *image, int code, int reference, const uint8_t *word, int length)
{
    const struct ar9300_eeprom *t;

    switch (code) {
    case _CompressNone:
        if (length != (int)sizeof(struct ar9300_eeprom))
            return -EIO;
        k_memcpy(image, word + COMP_HDR_LEN, (size_t)length);
        return 0;
    case _CompressBlock:
        if (reference != 0) {
            t = template_find(reference);
            if (!t)
                return -EIO;
            k_memcpy(image, t, sizeof(*t));
        }
        return uncompress_block(image, word + COMP_HDR_LEN, length);
    default:
        return -EIO;
    }
}

/**
 * Find the image and lay its blocks over ar9300_default, as
 * ar9300_eeprom_restore_internal does: the EEPROM at the end of 1 KiB,
 * 4 KiB and 512 bytes, then the OTP at 1 KiB and 512 bytes; from there the
 * blocks run downward, each a header, its bytes and a checksum, until a
 * header reads empty. A block with a bad checksum is skipped. Returns the
 * blocks restored, or -EIO when no store holds a header.
 */
static int restore(struct ath9k *ah, uint8_t *image)
{
    static const int eeprom_at[] = {AR9300_BASE_ADDR, AR9300_BASE_ADDR_4K, AR9300_BASE_ADDR_512};
    static const int otp_at[] = {AR9300_BASE_ADDR, AR9300_BASE_ADDR_512};
    static uint8_t word[EEPROM_MAX_BLOCK]; /* attach is single, under the kernel lock */
    eeprom_read_op read = NULL;
    int cptr = 0, it, restored = 0;
    unsigned i;

    k_memcpy(image, &ar9300_default, sizeof(ar9300_default));
    for (i = 0; !read && i < sizeof(eeprom_at) / sizeof(eeprom_at[0]); i++)
        if (found_at(ah, read_eeprom, eeprom_at[i])) {
            read = read_eeprom;
            cptr = eeprom_at[i];
        }
    for (i = 0; !read && i < sizeof(otp_at) / sizeof(otp_at[0]); i++)
        if (found_at(ah, read_otp, otp_at[i])) {
            read = read_otp;
            cptr = otp_at[i];
        }
    if (!read)
        return -EIO;

    for (it = 0; it < EEPROM_MAX_BLOCKS; it++) {
        int code, reference, length;
        uint16_t sum, stored;

        if (read(ah, cptr, word, COMP_HDR_LEN) != 0)
            return -EIO;
        if (!header_ok(word))
            break;
        /* ar9300_comp_hdr_unpack; the major and minor versions are unused. */
        code = (word[0] >> 5) & 0x0007;
        reference = (word[0] & 0x001f) | ((word[1] >> 2) & 0x0020);
        length = ((word[1] << 4) & 0x07f0) | ((word[2] >> 4) & 0x000f);
        if (length >= 1024 || length > cptr) {
            cptr -= COMP_HDR_LEN;
            continue;
        }
        if (read(ah, cptr, word, COMP_HDR_LEN + length + COMP_CKSUM_LEN) != 0)
            return -EIO;
        for (sum = 0, i = 0; i < (unsigned)length; i++)
            sum = (uint16_t)(sum + word[COMP_HDR_LEN + i]);
        stored = (uint16_t)(word[COMP_HDR_LEN + length] | word[COMP_HDR_LEN + length + 1] << 8);
        if (sum == stored && restore_block(image, code, reference, word, length) == 0)
            restored++;
        cptr -= COMP_HDR_LEN + length + COMP_CKSUM_LEN;
    }
    return restored;
}

/** Whether `mac` is a unicast address other than all zeros
 * (is_valid_ether_addr). */
static int mac_valid(const uint8_t *mac)
{
    return !(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0;
}

/**
 * Read and unpack the image, and take the MAC address, the regulatory
 * domain and the country from it. A card whose image holds no unicast
 * address gets a random locally administered one, as ath9k gives it. The
 * country is "00", the world domain: the driver keeps no table of ath9k's
 * country codes, and the Lenovo's card names none (domain 0x6a).
 */
int ath9k_eeprom_init(struct ath9k *ah)
{
    struct ath9k_eeprom *e = &ah->eeprom;
    int restored;

    e->valid = 0;
    restored = restore(ah, (uint8_t *)&e->image);
    if (restored <= 0) {
        klog(KLOG_ERR, "ath9k", "eeprom: failed: %s",
             restored < 0 ? "no EEPROM or OTP holds an image" : "no block of the image restored");
        return -EIO;
    }
    e->regdomain = e->image.baseEepHeader.regDmn[0];
    k_memcpy(ah->radio.mac, e->image.macAddr, 6);
    if (!mac_valid(ah->radio.mac)) {
        klog(KLOG_WARN, "ath9k",
             "eeprom: mac %02x:%02x:%02x:%02x:%02x:%02x invalid: using a random one",
             ah->radio.mac[0], ah->radio.mac[1], ah->radio.mac[2], ah->radio.mac[3],
             ah->radio.mac[4], ah->radio.mac[5]);
        krandom_bytes(ah->radio.mac, 6);
        ah->radio.mac[0] = (uint8_t)((ah->radio.mac[0] & 0xfe) | 0x02);
    }
    ah->radio.country[0] = '0';
    ah->radio.country[1] = '0';
    e->valid = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Target powers and conformance limits.                               */
/* ------------------------------------------------------------------ */

/** ath9k's interpolate: y at x on the line through (xa, ya) and (xb, yb),
 * rounded away from ya. */
static int interpolate(int x, int xa, int xb, int ya, int yb)
{
    int bf = 2 * (yb - ya) * (x - xa) / (xb - xa);

    return ya + bf / 2 + bf % 2;
}

/**
 * The value at `x` of the `np` points (px, py), as
 * ar9003_hw_power_interpolate does: between the nearest point at or below
 * x and the nearest at or above it, or the nearer one's value outside them.
 */
static int power_interpolate(int x, const int *px, const int *py, unsigned np)
{
    int lx = 0, ly = 0, hx = 0, hy = 0, lhave = 0, hhave = 0;
    unsigned ip;

    for (ip = 0; ip < np; ip++) {
        int dx = x - px[ip];

        if (dx <= 0 && (!hhave || dx > x - hx)) {
            hx = px[ip];
            hy = py[ip];
            hhave = 1;
        }
        if (dx >= 0 && (!lhave || dx < x - lx)) {
            lx = px[ip];
            ly = py[ip];
            lhave = 1;
        }
    }
    if (lhave && hhave)
        return hx == lx ? ly : interpolate(x, lx, hx, ly, hy);
    if (lhave)
        return ly;
    if (hhave)
        return hy;
    return 0;
}

/**
 * The target power of column `rate` at `freq` among `n` piers whose bins are
 * `bins` and whose powers are rows of `stride` bytes at `powers`, as
 * ar9003_hw_eeprom_get_tgt_pwr and its HT and CCK forms do.
 */
static uint8_t target(const uint8_t *bins, const uint8_t *powers, unsigned n, unsigned stride,
                      unsigned rate, unsigned freq, int is2ghz)
{
    int f[AR9300_NUM_5G_20_TARGET_POWERS], p[AR9300_NUM_5G_20_TARGET_POWERS];
    unsigned i;

    for (i = 0; i < n; i++) {
        f[i] = fbin2freq(bins[i], is2ghz);
        p[i] = powers[i * stride + rate];
    }
    return (uint8_t)power_interpolate((int)freq, f, p, n);
}

/**
 * Fill `t` with the target power of each rate at `freq`, in half dB, as
 * ar9003_hw_get_target_power_eeprom does for an HT40 channel, so that every
 * entry is filled: CCK at 2.4 GHz, the OFDM rates, and HT20 and HT40.
 */
void ath9k_eeprom_target_power(const struct ath9k *ah, unsigned freq, uint8_t t[ar9300RateSize])
{
    const struct ar9300_eeprom *e = &ah->eeprom.image;
    int g = is_2ghz(freq);
    const uint8_t *legacy_bins = g ? e->calTarget_freqbin_2G : e->calTarget_freqbin_5G;
    const uint8_t *ht20_bins = g ? e->calTarget_freqbin_2GHT20 : e->calTarget_freqbin_5GHT20;
    const uint8_t *ht40_bins = g ? e->calTarget_freqbin_2GHT40 : e->calTarget_freqbin_5GHT40;
    const uint8_t *legacy = g ? e->calTargetPower2G[0].tPow2x : e->calTargetPower5G[0].tPow2x;
    const uint8_t *ht20 = g ? e->calTargetPower2GHT20[0].tPow2x : e->calTargetPower5GHT20[0].tPow2x;
    const uint8_t *ht40 = g ? e->calTargetPower2GHT40[0].tPow2x : e->calTargetPower5GHT40[0].tPow2x;
    unsigned nl = g ? AR9300_NUM_2G_20_TARGET_POWERS : AR9300_NUM_5G_20_TARGET_POWERS;
    unsigned nh40 = g ? AR9300_NUM_2G_40_TARGET_POWERS : AR9300_NUM_5G_40_TARGET_POWERS;
    unsigned i;

    k_memset(t, 0, ar9300RateSize);
    for (i = 0; i < 4; i++)
        t[ALL_TARGET_LEGACY_6_24 + i] = target(legacy_bins, legacy, nl, 4, i, freq, g);
    if (g)
        for (i = 0; i < 4; i++)
            t[ALL_TARGET_LEGACY_1L_5L + i] =
                target(e->calTarget_freqbin_Cck, e->calTargetPowerCck[0].tPow2x,
                       AR9300_NUM_2G_CCK_TARGET_POWERS, 4, i, freq, 1);
    for (i = 0; i < 14; i++) {
        t[ALL_TARGET_HT20_0_8_16 + i] = target(ht20_bins, ht20, nl, 14, i, freq, g);
        t[ALL_TARGET_HT40_0_8_16 + i] = target(ht40_bins, ht40, nh40, 14, i, freq, g);
    }
}

/**
 * The edge power of conformance test `idx` at `freq`, as
 * ar9003_hw_get_max_edge_power does: an edge at `freq` gives its own, an
 * edge above it the one below when that one holds in band, and none
 * MAX_RATE_POWER; at 2.4 GHz a zero is read as 60.
 */
static unsigned edge_power(const struct ar9300_eeprom *e, unsigned freq, unsigned idx, int g)
{
    const uint8_t *bins = g ? e->ctl_freqbin_2G[idx] : e->ctl_freqbin_5G[idx];
    const uint8_t *edges = g ? e->ctlPowerData_2G[idx].ctlEdges : e->ctlPowerData_5G[idx].ctlEdges;
    unsigned n = g ? AR9300_NUM_BAND_EDGES_2G : AR9300_NUM_BAND_EDGES_5G;
    unsigned edge, power = MAX_RATE_POWER;

    for (edge = 0; edge < n && bins[edge] != AR5416_BCHAN_UNUSED; edge++) {
        if ((int)freq == fbin2freq(bins[edge], g)) {
            power = CTL_EDGE_TPOWER(edges[edge]);
            break;
        }
        if (edge > 0 && (int)freq < fbin2freq(bins[edge], g)) {
            if (fbin2freq(bins[edge - 1], g) < (int)freq && CTL_EDGE_FLAGS(edges[edge - 1]))
                power = CTL_EDGE_TPOWER(edges[edge - 1]);
            break;
        }
    }
    if (g && power == 0)
        power = 60;
    return power;
}

/**
 * The lowest edge power at `freq` among the conformance tests of `mode`,
 * as ar9003_hw_set_power_per_rate_table takes it for a domain of the
 * SD_NO_CTL group, the world domains. For a domain of a named group it is
 * never above that group's own limit, which ath9k reads through its table
 * of domains.
 */
unsigned ath9k_eeprom_ctl_limit(const struct ath9k *ah, unsigned freq, unsigned mode)
{
    const struct ar9300_eeprom *e = &ah->eeprom.image;
    int g = is_2ghz(freq);
    const uint8_t *index = g ? e->ctlIndex_2G : e->ctlIndex_5G;
    unsigned n = g ? AR9300_NUM_CTLS_2G : AR9300_NUM_CTLS_5G;
    unsigned i, limit = MAX_RATE_POWER, power;

    for (i = 0; i < n && index[i]; i++) {
        if ((index[i] & CTL_MODE_M) != (mode & CTL_MODE_M))
            continue;
        power = edge_power(e, freq, i, g);
        if (power < limit)
            limit = power;
    }
    return limit;
}

/* ------------------------------------------------------------------ */
/* The board's settings.                                               */
/* ------------------------------------------------------------------ */

/** The chains the chip transmits on: its row's, narrowed by the EEPROM's
 * transmit mask (ath9k_hw_fill_cap_info). */
static unsigned tx_chains(const struct ath9k *ah)
{
    unsigned mask = ah->hw.chip->chainmask & (ah->eeprom.image.baseEepHeader.txrxMask >> 4);

    return mask ? mask : ah->hw.chip->chainmask;
}

/** Raise the analog bias currents when the EEPROM asks for drive strength
 * (ar9003_hw_drive_strength_apply). */
static void drive_strength(struct ath9k *ah)
{
    uint32_t reg;

    if (!(ah->eeprom.image.baseEepHeader.miscConfiguration & 0x01))
        return;
    reg = REG_READ(ah, AR_PHY_65NM_CH0_BIAS1);
    reg &= ~0x00ffffc0u;
    reg |= 0x5u << 21 | 0x5u << 18 | 0x5u << 15 | 0x5u << 12 | 0x5u << 9 | 0x5u << 6;
    REG_WRITE(ah, AR_PHY_65NM_CH0_BIAS1, reg);
    reg = REG_READ(ah, AR_PHY_65NM_CH0_BIAS2);
    reg &= ~0xffffffe0u;
    reg |= 0x5u << 29 | 0x5u << 26 | 0x5u << 23 | 0x5u << 20 | 0x5u << 17 | 0x5u << 14 |
           0x5u << 11 | 0x5u << 8 | 0x5u << 5;
    REG_WRITE(ah, AR_PHY_65NM_CH0_BIAS2, reg);
    reg = REG_READ(ah, AR_PHY_65NM_CH0_BIAS4);
    reg &= ~0xff800000u;
    reg |= 0x5u << 29 | 0x5u << 26 | 0x5u << 23;
    REG_WRITE(ah, AR_PHY_65NM_CH0_BIAS4, reg);
}

/** Set each transmit chain's external attenuation and its margin from the
 * modal header (ar9003_hw_atten_apply at 2.4 GHz). */
static void attenuation(struct ath9k *ah, const struct ar9300_modal_eep_header *m)
{
    static const uint32_t reg[AR9300_MAX_CHAINS] = {AR_PHY_EXT_ATTEN_CTL_0, AR_PHY_EXT_ATTEN_CTL_1,
                                                    AR_PHY_EXT_ATTEN_CTL_2};
    unsigned chain, mask = tx_chains(ah);

    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++) {
        if (!(mask & (1u << chain)))
            continue;
        REG_RMW_FIELD(ah, reg[chain], AR_PHY_EXT_ATTEN_CTL_XATTEN1_DB, m->xatten1DB[chain]);
        REG_RMW_FIELD(ah, reg[chain], AR_PHY_EXT_ATTEN_CTL_XATTEN1_MARGIN, m->xatten1Margin[chain]);
    }
}

/** Program the internal regulator, or power it down when the EEPROM does
 * not use it (ar9003_hw_internal_regulator_apply, the AR9462 design's
 * path). */
static void regulator(struct ath9k *ah)
{
    const struct ar9300_base_eep_hdr *b = &ah->eeprom.image.baseEepHeader;

    if (b->featureEnable & 0x10)
        REG_WRITE(ah, AR_PHY_PMU1, b->swreg);
    else
        REG_RMW_FIELD(ah, AR_PHY_PMU1, AR_PHY_PMU1_PWD, 0x1);
}

/** Load the crystal's tuning capacitors (ar9003_hw_apply_tuning_caps). */
static void tuning_caps(struct ath9k *ah)
{
    const struct ar9300_base_eep_hdr *b = &ah->eeprom.image.baseEepHeader;
    unsigned caps = b->params_for_tuning_caps[0] & 0x7f;

    if (!(b->featureEnable & 0x40))
        return;
    REG_RMW_FIELD(ah, AR_CH0_XTAL, AR_CH0_XTAL_CAPINDAC, caps);
    REG_RMW_FIELD(ah, AR_CH0_XTAL, AR_CH0_XTAL_CAPOUTDAC, caps);
}

/** Set each transmit chain's minimum CCA power from the noise-floor
 * thresholds when the EEPROM enables it (ar9003_hw_apply_minccapwr_thresh). */
static void min_cca_power(struct ath9k *ah, const struct ar9300_modal_eep_header *m, int g)
{
    static const uint32_t reg[AR9300_MAX_CHAINS] = {AR_PHY_CCA_CTRL_0, AR_PHY_CCA_CTRL_1,
                                                    AR_PHY_CCA_CTRL_2};
    unsigned chain, mask = tx_chains(ah);

    if (!(ah->eeprom.image.base_ext1.misc_enable & (g ? 0x04 : 0x08)))
        return;
    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++)
        if (mask & (1u << chain))
            REG_RMW_FIELD(ah, reg[chain], AR_PHY_EXT_CCA0_THRESH62_1,
                          (uint8_t)m->noiseFloorThreshCh[chain]);
}

/** Choose the chain whose thermometer the chip reads, or none
 * (ar9003_hw_thermometer_apply). */
static void thermometer(struct ath9k *ah)
{
    static const uint32_t reg[AR9300_MAX_CHAINS] = {AR_PHY_65NM_CH0_RXTX4, AR_PHY_65NM_CH1_RXTX4,
                                                    AR_PHY_65NM_CH2_RXTX4};
    int which = ((ah->eeprom.image.baseEepHeader.miscConfiguration >> 1) & 0x3) - 1;
    unsigned chain;

    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++)
        if (chain == 0 || (ah->hw.chip->chainmask & (1u << chain)))
            REG_RMW_FIELD(ah, reg[chain], AR_PHY_65NM_CH0_RXTX4_THERM_ON_OVR, which >= 0);
    for (chain = 0; chain < AR9300_MAX_CHAINS; chain++)
        if (chain == 0 || (ah->hw.chip->chainmask & (1u << chain)))
            REG_RMW_FIELD(ah, reg[chain], AR_PHY_65NM_CH0_RXTX4_THERM_ON, which == (int)chain);
}

/**
 * Write the board's settings for `chan`, in ath9k_hw_ar9300_set_board_values'
 * order, leaving the antenna's to antenna.c. The steps ath9k takes only on
 * chips of other designs are left out: the transmit PA timing (AR9300,
 * AR9340, AR9580, AR9531, AR9561), the external LNA bias (AR9300), quick
 * drop (AR9300, AR9580, AR9340) and the thermometer's OTP calibration
 * (AR9462 2.0); a row of one of them adds its step here under a test of its
 * caps. The PA bias is the AR9462 design's field of AR_CH0_TOP.
 */
void ath9k_eeprom_set_board_values(struct ath9k *ah, const struct wlan_channel *chan)
{
    int g = is_2ghz(chan->frequency);
    const struct ar9300_modal_eep_header *m =
        g ? &ah->eeprom.image.modalHeader2G : &ah->eeprom.image.modalHeader5G;

    REG_RMW_FIELD(ah, AR_CH0_TOP, AR_CH0_TOP_XPABIASLVL, m->xpaBiasLvl);
    drive_strength(ah);
    attenuation(ah, m);
    regulator(ah);
    tuning_caps(ah);
    min_cca_power(ah, m, g);
    REG_RMW_FIELD(ah, AR_PHY_XPA_TIMING_CTL, AR_PHY_XPA_TIMING_CTL_TX_END_XPAB_OFF,
                  m->txEndToXpaOff);
    REG_RMW_FIELD(ah, AR_PHY_XPA_TIMING_CTL, AR_PHY_XPA_TIMING_CTL_TX_END_XPAA_OFF,
                  m->txEndToXpaOff);
    thermometer(ah);
}
