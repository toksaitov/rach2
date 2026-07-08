/**
 * Dual-target seam test for build_user_stack (proc/uimage.c).
 *
 * host-deps: rach2/sys/proc/uimage.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The SysV/i386 initial-stack layout is built into a plain buffer and verified
 * natively under ASan/UBSan: argc, the argv/envp pointer arrays and their NULL
 * terminators, the auxv pairs and AT_NULL, AT_RANDOM pointing at the supplied
 * bytes, and 16-byte ESP alignment. Returns the failed-check count (0 == all
 * passed).
 */

#include "uimage.h"
#include "auxv.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* Compare the string at virtual address v (within the page) to s. */
static int streq_at(const uint8_t *page, uint32_t base, uint32_t v, const char *s)
{
    const uint8_t *p = page + (v - base);

    while (*s) {
        if (*p != (uint8_t)*s) {
            return 0;
        }
        p++;
        s++;
    }
    return *p == 0;
}

int main(void)
{
    static uint8_t page[4096];
    uint32_t base = 0xBFFFE000u;
    char *argv[] = {"prog", "arg one", 0};
    char *envp[] = {"PATH=/bin", "HOME=/", 0};
    elf_image_t img;
    uint8_t rnd[16];
    uint32_t esp;
    uint32_t *w;
    int i;
    int off;
    int saw_null;
    uint32_t rnd_v;
    uint32_t phdr_v;
    uint32_t entry_v;
    uint32_t pagesz_v;
    uint32_t secure_v;
    int saw_secure;

    for (i = 0; i < 16; i++) {
        rnd[i] = (uint8_t)(i * 7 + 1);
    }
    for (i = 0; i < 4096; i++) {
        page[i] = 0;
    }
    img.entry = 0x08048100u;
    img.phdr_vaddr = 0x08048034u;
    img.phent = 32;
    img.phnum = 3;

    esp = build_user_stack(page, base, 4096, 2, argv, envp, &img, rnd, 0);

    CHECK(esp != 0);
    CHECK((esp & 0xF) == 0);
    CHECK(esp >= base && esp < base + 4096);

    w = (uint32_t *)(page + (esp - base));
    CHECK(w[0] == 2); /* argc */
    CHECK(streq_at(page, base, w[1], "prog"));
    CHECK(streq_at(page, base, w[2], "arg one"));
    CHECK(w[3] == 0); /* argv NULL */
    CHECK(streq_at(page, base, w[4], "PATH=/bin"));
    CHECK(streq_at(page, base, w[5], "HOME=/"));
    CHECK(w[6] == 0); /* envp NULL */

    /* Scan the auxv pairs starting at w[7]. */
    saw_null = 0;
    saw_secure = 0;
    secure_v = 99;
    rnd_v = 0;
    phdr_v = 0;
    entry_v = 0;
    pagesz_v = 0;
    for (off = 7; off < 1000; off += 2) {
        uint32_t t = w[off];
        uint32_t val = w[off + 1];

        if (t == AT_NULL) {
            saw_null = 1;
            break;
        }
        if (t == AT_RANDOM) {
            rnd_v = val;
        }
        if (t == AT_PHDR) {
            phdr_v = val;
        }
        if (t == AT_ENTRY) {
            entry_v = val;
        }
        if (t == AT_PAGESZ) {
            pagesz_v = val;
        }
        if (t == AT_SECURE) {
            secure_v = val;
            saw_secure = 1;
        }
    }
    CHECK(saw_null);
    CHECK(saw_secure && secure_v == 0);
    CHECK(phdr_v == 0x08048034u);
    CHECK(entry_v == 0x08048100u);
    CHECK(pagesz_v == 4096);

    /* AT_RANDOM points at the 16 bytes we supplied. */
    CHECK(rnd_v != 0);
    if (rnd_v != 0) {
        const uint8_t *rp = page + (rnd_v - base);
        int ok = 1;

        for (i = 0; i < 16; i++) {
            if (rp[i] != rnd[i]) {
                ok = 0;
            }
        }
        CHECK(ok);
    }

    /* A request too big for the page is rejected (0), not allowed to
     * overflow. */
    {
        static char bigstr[5000];
        char *bigargv[2];
        uint32_t e2;

        for (i = 0; i < 4999; i++) {
            bigstr[i] = 'x';
        }
        bigstr[4999] = 0;
        bigargv[0] = bigstr;
        bigargv[1] = 0;
        e2 = build_user_stack(page, base, 4096, 1, bigargv, 0, &img, rnd, 0);
        CHECK(e2 == 0);
    }

    /* A set-id exec publishes AT_SECURE 1. */
    {
        uint32_t e3 = build_user_stack(page, base, 4096, 2, argv, envp, &img, rnd, 1);
        uint32_t *w3 = (uint32_t *)(page + (e3 - base));
        uint32_t sec3 = 99;
        CHECK(e3 != 0);
        for (off = 7; off < 1000; off += 2) {
            if (w3[off] == AT_NULL) {
                break;
            }
            if (w3[off] == AT_SECURE) {
                sec3 = w3[off + 1];
            }
        }
        CHECK(sec3 == 1);
    }

    return (fails > 120) ? 120 : fails;
}
