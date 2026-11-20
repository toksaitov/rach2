/*
 * fat32name.c: the name rules of fat32name.h.
 */

#include "fat32name.h"
#include "fatcommon.h"

#include "../lib/kerrno.h"

/* Code page 437's bytes 0x80 to 0xFF in Unicode, the table of Linux's
 * nls_cp437; bytes 0x01 to 0x7F are ASCII. Each character here has one
 * byte, so the table read backwards is the encoding. */
static const uint16_t cp437_high[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF,
    0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
    0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* The upper- and lower-case pairs of code page 437 above 0x7F, as Linux's
 * nls_cp437 maps them both ways: C-cedilla, A-umlaut, A-ring, E-acute, AE,
 * O-umlaut, U-umlaut, N-tilde, Sigma and Phi. */
static const uint8_t cp437_case[10][2] = {
    {0x80, 0x87}, {0x8E, 0x84}, {0x8F, 0x86}, {0x90, 0x82}, {0x92, 0x91},
    {0x99, 0x94}, {0x9A, 0x81}, {0xA5, 0xA4}, {0xE4, 0xE5}, {0xE8, 0xED},
};

/** The byte `c` of code page 437 in upper case (`upper` 1) or lower case
 * (0); a byte with no other case is itself. */
static uint8_t cp437_fold(uint8_t c, int upper)
{
    int i;

    if (upper && c >= 'a' && c <= 'z') {
        return (uint8_t)(c - 32);
    }
    if (!upper && c >= 'A' && c <= 'Z') {
        return (uint8_t)(c + 32);
    }
    for (i = 0; i < 10; i++) {
        if (c == cp437_case[i][upper ? 1 : 0]) {
            return cp437_case[i][upper ? 0 : 1];
        }
    }
    return c;
}

/** The byte of code page 437 for the UTF-16 unit `u`, or 0 when the code
 * page has no such character, a surrogate among them. */
static uint8_t cp437_byte(uint32_t u)
{
    int i;

    if (u >= 1 && u <= 0x7F) {
        return (uint8_t)u;
    }
    for (i = 0; i < 128; i++) {
        if (cp437_high[i] == u) {
            return (uint8_t)(0x80 + i);
        }
    }
    return 0;
}

/** Whether a short name may have `c`: a letter, a digit or one of `set`. */
static int short_char(uint32_t c)
{
    static const char set[] = "$%'-_@~`!(){}^#&";
    int i;

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        return 1;
    }
    for (i = 0; set[i]; i++) {
        if (c == (uint32_t)(unsigned char)set[i]) {
            return 1;
        }
    }
    return 0;
}

int fat32_name_short(const char *name, uint8_t *name11, uint8_t *lower)
{
    int base = 0, ext = 0, in_ext = 0;
    int base_upper = 0, base_lower = 0, ext_upper = 0, ext_lower = 0;
    int i;

    for (i = 0; i < 11; i++) {
        name11[i] = ' ';
    }
    for (i = 0; name[i]; i++) {
        uint32_t c = (unsigned char)name[i];

        if (c == '.') {
            if (in_ext || base == 0) {
                return 0;
            }
            in_ext = 1;
            continue;
        }
        if (!short_char(c)) {
            return 0;
        }
        if (c >= 'a' && c <= 'z') {
            if (in_ext) {
                ext_lower = 1;
            } else {
                base_lower = 1;
            }
            c -= 32;
        } else if (c >= 'A' && c <= 'Z') {
            if (in_ext) {
                ext_upper = 1;
            } else {
                base_upper = 1;
            }
        }
        if (in_ext) {
            if (ext >= 3) {
                return 0;
            }
            name11[8 + ext++] = (uint8_t)c;
        } else {
            if (base >= 8) {
                return 0;
            }
            name11[base++] = (uint8_t)c;
        }
    }
    if (base == 0 || (in_ext && ext == 0)) {
        return 0;
    }
    if ((base_upper && base_lower) || (ext_upper && ext_lower)) {
        return 0;
    }
    if (name11[0] == 0xE5) {
        name11[0] = 0x05;
    }
    *lower = (uint8_t)((base_lower ? 0x08 : 0) | (ext_lower ? 0x10 : 0));
    return 1;
}

/** The byte that unit `u` of a long name takes in its alias: an ASCII
 * character the short set has, or one of code page 437's from 0x7F up, in
 * upper case; `_` for anything else. 0x7F is kept, as Linux's
 * to_shortname_char keeps it. */
static uint8_t alias_byte(uint32_t u)
{
    uint8_t c = cp437_byte(u);

    if (c >= 0x7F || (c != 0 && short_char(c))) {
        return cp437_fold(c, 1);
    }
    return '_';
}

void fat32_alias(const uint16_t *units, uint32_t count, uint8_t *name11)
{
    uint32_t i;
    int32_t last_dot = -1;
    int base = 0, ext = 0;

    for (i = 0; i < 11; i++) {
        name11[i] = ' ';
    }
    for (i = 0; i < count; i++) {
        if (units[i] == '.') {
            last_dot = (int32_t)i;
        }
    }
    for (i = 0; i < count && base < 6; i++) {
        if ((int32_t)i == last_dot) {
            break;
        }
        if (units[i] == '.' || units[i] == ' ') {
            continue;
        }
        name11[base++] = alias_byte(units[i]);
    }
    if (last_dot >= 0) {
        for (i = (uint32_t)last_dot + 1; i < count && ext < 3; i++) {
            if (units[i] == ' ') {
                continue;
            }
            name11[8 + ext++] = alias_byte(units[i]);
        }
    }
    if (base == 0) {
        name11[base++] = '_';
    }
    name11[base] = '~';
    name11[base + 1] = '1';
    if (name11[0] == 0xE5) {
        name11[0] = 0x05;
    }
}

void fat32_alias_number(uint8_t *name11, uint32_t n)
{
    char digits[8];
    int nd = 0, keep, i;

    do {
        digits[nd++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0 && nd < 7);
    keep = 8 - 1 - nd;
    for (i = 0; i < keep; i++) {
        if (name11[i] == '~' || name11[i] == ' ') {
            break;
        }
    }
    keep = i;
    name11[keep] = '~';
    for (i = 0; i < nd; i++) {
        name11[keep + 1 + i] = (uint8_t)digits[nd - 1 - i];
    }
    for (i = keep + 1 + nd; i < 8; i++) {
        name11[i] = ' ';
    }
}

uint8_t fat32_short_sum(const uint8_t *name11)
{
    uint8_t sum = 0;
    int i;

    for (i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
    }
    return sum;
}

uint32_t fat32_long_entries(uint32_t count)
{
    return (count + 12) / 13;
}

void fat32_long_entry(uint8_t *raw, uint32_t ord, uint32_t entries, uint8_t sum,
                      const uint16_t *units, uint32_t count)
{
    static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    uint32_t i;

    for (i = 0; i < 32; i++) {
        raw[i] = 0;
    }
    raw[0] = (uint8_t)(ord | (ord == entries ? 0x40 : 0));
    raw[11] = 0x0F;
    raw[13] = sum;
    for (i = 0; i < 13; i++) {
        uint32_t k = (ord - 1) * 13 + i;
        uint16_t u = k < count ? units[k] : (k == count ? 0 : 0xFFFF);

        raw[at[i]] = (uint8_t)u;
        raw[at[i] + 1] = (uint8_t)(u >> 8);
    }
}

/** Append the UTF-8 of the code page 437 byte `c` to `out` at `*n`, `?` for
 * the byte 0, which the code page does not map. */
static void cp437_put(uint8_t c, char *out, int *n)
{
    uint32_t u = c < 0x80 ? c : cp437_high[c - 0x80];

    if (u == 0) {
        u = '?';
    }
    if (u < 0x80) {
        out[(*n)++] = (char)u;
    } else if (u < 0x800) {
        out[(*n)++] = (char)(0xC0 | (u >> 6));
        out[(*n)++] = (char)(0x80 | (u & 0x3F));
    } else {
        out[(*n)++] = (char)(0xE0 | (u >> 12));
        out[(*n)++] = (char)(0x80 | ((u >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (u & 0x3F));
    }
}

/** Append bytes `from` to `to` of `name11` to `out` at `*n`, up to the last
 * that is not a space and stopping at a 0 byte, in lower case when `lower`;
 * 1 when a byte was appended. */
static int short_part(const uint8_t *name11, int from, int to, int lower, char *out, int *n)
{
    int end, i;

    for (end = from; end < to && name11[end] != 0; end++) {
    }
    while (end > from && name11[end - 1] == ' ') {
        end--;
    }
    for (i = from; i < end; i++) {
        uint8_t c = name11[i];

        if (i == 0 && c == 0x05) {
            c = 0xE5;
        }
        cp437_put(lower ? cp437_fold(c, 0) : c, out, n);
    }
    return end > from;
}

void fat32_short_utf8(const uint8_t *name11, uint8_t case_bits, char *out)
{
    char ext[3 * 3];
    int n = 0, e = 0, i;

    short_part(name11, 0, 8, case_bits & 0x08, out, &n);
    if (short_part(name11, 8, 11, case_bits & 0x10, ext, &e)) {
        out[n++] = '.';
        for (i = 0; i < e; i++) {
            out[n++] = ext[i];
        }
    }
    out[n] = '\0';
}
