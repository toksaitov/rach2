/*
 * modlayout.c: the boot-module placement check.
 *
 * Pure arithmetic over ranges the bootloader chose, so the host test can craft
 * placements no bootloader offers. Every comparison subtracts rather than
 * adds, because a crafted or corrupt descriptor can name a range that wraps
 * the 32-bit address space and an addition would wrap with it.
 */

#include "modlayout.h"

int modlayout_check(const struct modlayout_range *mods, uint32_t count, uint32_t kernel_end,
                    uint32_t limit, uint32_t *bad)
{
    uint32_t i, j;

    for (i = 0; i < count; i++) {
        if (bad) {
            *bad = i;
        }
        if (mods[i].end <= mods[i].start) {
            return MODLAYOUT_EMPTY;
        }
        if (mods[i].end > limit) {
            return MODLAYOUT_PAST_LIMIT;
        }
        if (mods[i].start < kernel_end) {
            return MODLAYOUT_HITS_KERNEL;
        }
    }

    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (mods[i].start < mods[j].end && mods[j].start < mods[i].end) {
                if (bad) {
                    *bad = j;
                }
                return MODLAYOUT_OVERLAP;
            }
        }
    }

    if (bad) {
        *bad = 0;
    }
    return MODLAYOUT_OK;
}

const char *modlayout_reason(int code)
{
    switch (code) {
    case MODLAYOUT_OK:
        return "the module layout is usable";
    case MODLAYOUT_EMPTY:
        return "a boot module is empty or ends before it starts";
    case MODLAYOUT_PAST_LIMIT:
        return "a boot module ends past the 32 MiB memory limit";
    case MODLAYOUT_HITS_KERNEL:
        return "a boot module overlaps the kernel image";
    case MODLAYOUT_OVERLAP:
        return "two boot modules overlap";
    default:
        return "the module layout failed an unknown check";
    }
}

/** Append as much of `src` as fits; `*pos` tracks the used length. */
static void ml_put(char *buf, uint32_t buflen, uint32_t *pos, const char *src)
{
    while (*src != '\0' && *pos + 1 < buflen) {
        buf[*pos] = *src;
        (*pos)++;
        src++;
    }
}

/** Append `v` as eight hexadecimal digits with the 0x prefix. */
static void ml_put_hex(char *buf, uint32_t buflen, uint32_t *pos, uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    ml_put(buf, buflen, pos, "0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        if (*pos + 1 < buflen) {
            buf[*pos] = digits[(v >> shift) & 0xF];
            (*pos)++;
        }
    }
}

/** Append `v` as decimal. */
static void ml_put_dec(char *buf, uint32_t buflen, uint32_t *pos, uint32_t v)
{
    char tmp[11];
    int n = 0;

    if (v == 0) {
        ml_put(buf, buflen, pos, "0");
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n] = (char)('0' + (v % 10u));
        n++;
        v /= 10u;
    }
    while (n > 0 && *pos + 1 < buflen) {
        n--;
        buf[*pos] = tmp[n];
        (*pos)++;
    }
}

void modlayout_describe(int code, uint32_t index, uint32_t start, uint32_t end, char *buf,
                        uint32_t buflen)
{
    uint32_t pos = 0;

    if (buflen == 0) {
        return;
    }
    ml_put(buf, buflen, &pos, modlayout_reason(code));
    ml_put(buf, buflen, &pos, ": module ");
    ml_put_dec(buf, buflen, &pos, index);
    ml_put(buf, buflen, &pos, " spans ");
    ml_put_hex(buf, buflen, &pos, start);
    ml_put(buf, buflen, &pos, " - ");
    ml_put_hex(buf, buflen, &pos, end);
    buf[pos] = '\0';
}
