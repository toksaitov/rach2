/* stackscan.c: an LD_PRELOAD for gem_readwrite that reports what lies where
 * its read-write subtest keeps `expected`, after IGT's start-up returns. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <getopt.h>
#include <stdio.h>

typedef int (*init_fn)(int *, char **, const char *, const struct option *, const char *,
                       void *, void *);

static unsigned char copy[0x4000];

/* Report the bytes of `expected`, which IGT 2.0's gem_readwrite built by
 * Debian puts 0x8030 below main's stack pointer at its call of
 * igt_subtest_init_parse_opts, and leaves unset but [1024,2048). */
__attribute__((noinline)) static void scan(const unsigned char *c)
{
    const unsigned char *e = c - 0x8030;
    unsigned long i, nz = 0, first = 0, last = 0, deep, shown = 0;
    Dl_info di;

    for (i = 0; i < sizeof(copy); i++)
        copy[i] = e[i];
    for (deep = 0x10000; deep > 0 && !c[-(long)deep]; deep--)
        ;
    for (i = 0; i < sizeof(copy); i++) {
        if ((i >= 1024 && i < 2048) || !copy[i])
            continue;
        if (!nz)
            first = i;
        last = i;
        nz++;
    }
    fprintf(stderr, "stackscan: deepest nonzero byte 0x%lx below main's stack\n", deep);
    fprintf(stderr, "stackscan: expected: %lu nonzero bytes outside [1024,2048), first %lu last %lu\n",
            nz, first, last);
    for (i = 0; i + 8 <= sizeof(copy) && shown < 40; i += 8) {
        void *w = *(void **)(copy + i);

        if (w && dladdr(w, &di) && di.dli_fname) {
            fprintf(stderr, "stackscan: expected[%lu]: %p %s %s+0x%lx\n", i, w, di.dli_fname,
                    di.dli_sname ? di.dli_sname : "?",
                    (unsigned long)((char *)w - (char *)(di.dli_saddr ? di.dli_saddr : di.dli_fbase)));
            shown++;
        }
    }
}

int igt_subtest_init_parse_opts(int *argc, char **argv, const char *s, const struct option *l,
                                const char *h, void *fn, void *d)
{
    init_fn real = (init_fn)dlsym(RTLD_NEXT, "igt_subtest_init_parse_opts");
    /* main's stack pointer at its call: above the return address and the
     * frame pointer this frame pushed. */
    const unsigned char *c = (const unsigned char *)__builtin_frame_address(0) + 16;
    int r = real(argc, argv, s, l, h, fn, d);

    scan(c);
    return r;
}
