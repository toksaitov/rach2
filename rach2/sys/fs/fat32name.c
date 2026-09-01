/*
 * fat32name.c: the name rules of fat32name.h.
 */

#include "fat32name.h"
#include "fatcommon.h"

#include "../lib/kerrno.h"

/* The characters a short name may hold besides letters and digits. */
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

void fat32_alias(const char *name, uint8_t *name11)
{
    int i, last_dot = -1, base = 0, ext = 0;

    for (i = 0; i < 11; i++) {
        name11[i] = ' ';
    }
    for (i = 0; name[i]; i++) {
        if (name[i] == '.') {
            last_dot = i;
        }
    }
    for (i = 0; name[i] && base < 6; i++) {
        uint32_t c = (unsigned char)name[i];

        if (i == last_dot) {
            break;
        }
        if (c == '.' || c == ' ' || (c & 0xC0) == 0x80) {
            continue; /* dots, spaces, and the tail bytes of a character */
        }
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        } else if (!short_char(c)) {
            c = '_';
        }
        name11[base++] = (uint8_t)c;
    }
    if (last_dot >= 0) {
        for (i = last_dot + 1; name[i] && ext < 3; i++) {
            uint32_t c = (unsigned char)name[i];

            if (c == ' ' || (c & 0xC0) == 0x80) {
                continue;
            }
            if (c >= 'a' && c <= 'z') {
                c -= 32;
            } else if (!short_char(c)) {
                c = '_';
            }
            name11[8 + ext++] = (uint8_t)c;
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
