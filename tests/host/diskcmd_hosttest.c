/*
 * diskcmd_hosttest.c: which ATA commands each build policy of
 * rach2/sys/block/diskcmd.c permits.
 *
 * host-deps: rach2/sys/block/diskcmd.c
 *
 * The read-only policy is for a kernel on a machine whose disk must not be
 * written, so the commands it refuses matter more than those it permits. The
 * test offers all 256 command bytes under it, and exactly five may be
 * permitted: the four reads and IDENTIFY.
 *
 * The policy compiled into this test is the host build's, so the table is
 * run through `diskcmd_allowed_under`, which takes the policy as an argument.
 * That the build switch selects the read-only policy is checked on the built
 * kernel by tools/inspect/check_diskpolicy.sh.
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

/**
 * Run the rows. Read-write must permit every command byte, and read-only
 * exactly the five above. Named rows require the writes and flushes to be
 * refused and the reads and IDENTIFY permitted under read-only, unknown
 * command bytes refused, and a policy value that is neither of the two
 * judged as read-only. The last row requires the banner to name the policy
 * `diskcmd_policy` returns. Returns the count of failed checks, at most 255.
 */
int main(void)
{
    unsigned c;
    int permitted = 0;

    for (c = 0; c < 256u; c++) {
        if (!diskcmd_allowed_under(DISKCMD_READ_WRITE, (uint8_t)c)) {
            fails++;
            printf("  FAIL: read-write refused command 0x%02X\n", c);
            break;
        }
    }

    for (c = 0; c < 256u; c++) {
        int allowed = diskcmd_allowed_under(DISKCMD_READ_ONLY, (uint8_t)c);
        int expected =
            (c == DISKCMD_READ_SECTORS || c == DISKCMD_READ_SECTORS_EXT || c == DISKCMD_READ_DMA ||
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
    CHECK(permitted == 5, "read-only permits exactly five of the 256 command bytes");

    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_WRITE_DMA_EXT),
          "WRITE DMA EXT is refused");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_WRITE_SECTORS),
          "WRITE SECTORS is refused");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_FLUSH_CACHE_EXT),
          "FLUSH CACHE EXT is refused");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_FLUSH_CACHE), "FLUSH CACHE is refused");
    CHECK(diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_READ_DMA),
          "read-only allows READ DMA, the 28-bit form the driver uses below 2^28");
    CHECK(diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_READ_DMA_EXT),
          "READ DMA EXT is permitted, since a read-only kernel still reads");
    CHECK(diskcmd_allowed_under(DISKCMD_READ_ONLY, DISKCMD_IDENTIFY),
          "IDENTIFY is permitted, since the diagnostic needs it");

    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, 0xF8),
          "an unknown command is refused under the read-only policy");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, 0x00),
          "command zero is refused under the read-only policy");
    CHECK(!diskcmd_allowed_under(DISKCMD_READ_ONLY, 0xFF),
          "command 0xFF is refused under the read-only policy");

    CHECK(!diskcmd_allowed_under(42, DISKCMD_WRITE_DMA_EXT),
          "a policy value that is neither of the two still refuses a write");

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

    return fails > 255 ? 255 : fails;
}
