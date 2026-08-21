/*
 * diskcmd_hosttest.c: which ATA commands each build policy permits.
 *
 * host-deps: rach2/sys/block/diskcmd.c
 *
 * What this guards is the one promise milestone 24 makes about somebody else's
 * property: a Rach2 built for the laptop must not write its SSD, which has a
 * Windows installation nobody has backed up. So the interesting half of the
 * table is not the commands it permits but the ones it does not, and the
 * sweep below is exhaustive, not a sample: all 256 command bytes are
 * offered under the read-only policy, and exactly four may be accepted.
 *
 * The compiled-in policy is only half of it. This build is whatever the host
 * compiler's default is, so the table is exercised through
 * `diskcmd_allowed_under`, which takes the policy as an argument; that the
 * BUILD switch actually selects the read-only policy is checked on the built
 * artifact instead, by tools/check_diskpolicy.py.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/block/diskcmd.h"

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

int main(void)
{
    unsigned c;
    int permitted = 0;

    /* ---- read-write permits everything, which is what every other image
            this project builds has always done ---- */
    for (c = 0; c < 256u; c++) {
        if (!diskcmd_allowed_under(DISKCMD_READ_WRITE, (uint8_t)c)) {
            fails++;
            printf("  FAIL: read-write refused command 0x%02X\n", c);
            break;
        }
    }

    /* ---- read-only permits four commands and no others ---- */
    for (c = 0; c < 256u; c++) {
        int allowed = diskcmd_allowed_under(DISKCMD_READ_ONLY, (uint8_t)c);
        int expected = (c == DISKCMD_READ_SECTORS || c == DISKCMD_READ_SECTORS_EXT ||
                        c == DISKCMD_READ_DMA_EXT || c == DISKCMD_IDENTIFY);

        if (allowed) {
            permitted++;
        }
        if (allowed != expected) {
            fails++;
            printf("  FAIL: read-only %s command 0x%02X\n", allowed ? "permitted" : "refused", c);
            break;
        }
    }
    CHECK(permitted == 4, "read-only permits exactly four of the 256 command bytes");

    /* ---- and the two that would reach the platter, named ---- */
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_WRITE_DMA_EXT),
          "WRITE DMA EXT is refused");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_WRITE_SECTORS),
          "WRITE SECTORS is refused");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_FLUSH_CACHE_EXT),
          "FLUSH CACHE EXT is refused");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_FLUSH_CACHE), "FLUSH CACHE is refused");
    CHECK(diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_READ_DMA_EXT),
          "READ DMA EXT is permitted, since a read-only kernel still reads");
    CHECK(diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_IDENTIFY),
          "IDENTIFY is permitted, since the diagnostic needs it");

    /* ---- a command nobody here has heard of is refused, not permitted ---- */
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, 0xF8),
          "an unknown command is refused under the read-only policy");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, 0x00),
          "command zero is refused under the read-only policy");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, 0xFF),
          "command 0xFF is refused under the read-only policy");

    /* ---- an unknown policy value is read-only, not read-write ---- */
    CHECK(!diskcmd_allowed_under(42, DISKCMD_WRITE_DMA_EXT),
          "a policy value that is neither of the two still refuses a write");

    /* ---- the banner and the compiled-in policy agree with each other ---- */
    {
        const char *b = diskcmd_policy_banner;
        int i, dash = -1;

        for (i = 0; b[i] != '\0'; i++) {
            if (b[i] == 'r' && b[i + 1] == 'e' && b[i + 2] == 'a' && b[i + 3] == 'd' &&
                b[i + 4] == '-') {
                dash = i;
            }
        }
        CHECK(dash >= 0, "the banner names a policy");
        if (dash >= 0) {
            int says_readonly = (b[dash + 5] == 'o');

            CHECK(says_readonly == (diskcmd_policy() == DISKCMD_READ_ONLY),
                  "the banner says the same policy the code returns");
        }
    }

    return fails;
}
