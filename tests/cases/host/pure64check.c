/*
 * pure64check.c: host.host.pure64, the x86-64 kernel built without the i386
 * ABI (`make IA32=0`), read from the symbol table of its kernel.debug beside
 * the default kernel's, since the kernel the loaders boot keeps no symbols but
 * `_start` and `_end`.
 */
#include "native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The symbols of the i386 ABI that a kernel built with IA32=0 must not
 * have and the default kernel has: the compat table's filler, an i386
 * handler, the i386 signal frame, the 32-bit vDSO's builder and the boot
 * option's switch. */
static const char *const absent[] = {"ia32_syscall_init", "compat_sys_execve",
                                     "ia32_sigframe_setup", "vdso32_build", "ia32_enabled"};

/* What both kernels keep: the x86-64 calls of the i386 ABI's descriptors,
 * which Linux's x86-64 table has too, and the entry a `syscall` from 32-bit
 * code takes where the kernel runs no i386 call. */
static const char *const kept[] = {"sys_modify_ldt", "sys_set_thread_area", "syscall32_ignore"};

/** The little-endian value of the `n` bytes at `p`. */
static unsigned long long get(const unsigned char *p, int n)
{
    unsigned long long v = 0;

    while (n-- > 0)
        v = v << 8 | p[n];
    return v;
}

/**
 * Whether the ELF64 x86-64 file of `size` bytes at `f` has the symbol `name`
 * in its SHT_SYMTAB: 1 or 0, or -1 for a file that is no such ELF file or
 * whose tables lie outside it.
 */
static int has_symbol(const unsigned char *f, size_t size, const char *name)
{
    unsigned long long shoff, i, j;
    unsigned shnum, shentsize;

    if (size < 64 || memcmp(f, "\177ELF", 4) || f[4] != 2 || f[5] != 1 || get(f + 18, 2) != 62)
        return -1;
    shoff = get(f + 40, 8);
    shentsize = (unsigned)get(f + 58, 2);
    shnum = (unsigned)get(f + 60, 2);
    if (shentsize != 64 || shoff > size || (size - shoff) / 64 < shnum)
        return -1;
    for (i = 0; i < shnum; i++) {
        const unsigned char *sh = f + shoff + i * 64, *str;
        unsigned long long off, len, link, soff, slen;

        if (get(sh + 4, 4) != 2) /* SHT_SYMTAB */
            continue;
        off = get(sh + 24, 8);
        len = get(sh + 32, 8);
        link = get(sh + 40, 4);
        if (link >= shnum || off > size || len > size - off)
            return -1;
        soff = get(f + shoff + link * 64 + 24, 8);
        slen = get(f + shoff + link * 64 + 32, 8);
        if (soff > size || slen > size - soff)
            return -1;
        str = f + soff;
        for (j = 0; j + 24 <= len; j += 24) {
            unsigned long long at = get(f + off + j, 4);

            if (at < slen && strnlen((const char *)str + at, slen - at) < slen - at &&
                !strcmp((const char *)str + at, name))
                return 1;
        }
        return 0;
    }
    return -1;
}

int main(void)
{
    struct native_case test;
    char pure[4096], full[4096], why[256];
    unsigned char *p, *k;
    size_t psize = 0, ksize = 0;
    size_t i;
    int ok;

    if (native_begin(&test, "host.host.pure64"))
        return 2;
    native_tool(&test, pure, "x86_64/rach2-pure64/kernel.debug");
    native_tool(&test, full, "x86_64/rach2/kernel.debug");
    p = native_read(pure, &psize);
    k = native_read(full, &ksize);
    native_assert(&test, "linked",
                  p && k && has_symbol(p, psize, "init_syscall") == 1 &&
                      has_symbol(k, ksize, "init_syscall") == 1,
                  "a kernel could not be read as an ELF64 x86-64 file with its symbol table");
    ok = p && k;
    why[0] = 0;
    for (i = 0; ok && i < sizeof(absent) / sizeof(*absent); i++) {
        if (has_symbol(p, psize, absent[i]) != 0 || has_symbol(k, ksize, absent[i]) != 1) {
            snprintf(why, sizeof(why), "%s is not absent from the IA32=0 kernel alone", absent[i]);
            ok = 0;
        }
    }
    native_assert(&test, "no-compat", ok, why);
    ok = p && k;
    for (i = 0; ok && i < sizeof(kept) / sizeof(*kept); i++) {
        if (has_symbol(p, psize, kept[i]) != 1 || has_symbol(k, ksize, kept[i]) != 1) {
            snprintf(why, sizeof(why), "%s is not in both kernels", kept[i]);
            ok = 0;
        }
    }
    native_assert(&test, "kept", ok, why);
    free(p);
    free(k);
    return native_end(&test);
}
