/**
 * Host test of `tls_resolve_base`, arch/i686/tls.c: the check of the
 * user_desc that set_thread_area(2) takes.
 *
 * host-deps: rach2/sys/arch/i686/tls.c
 *
 * The test runs the decoding of the flags under the sanitizers: the request
 * musl makes is taken and its base returned, and every malformed request
 * (another entry, not 32-bit, a code segment, read-only, not present, a
 * null argument) gives -1.
 */

#include "tls.h"
#include "arch/i686/gdt.h" /* GDT_TLS_ENTRY */
#include "mm/usercopy.h"
#include "proc/process.h"

static int fails;

/* The system calls beside the decoding, which no row makes. */
struct percpu cpu_table[NR_CPUS];

/** -1: no row copies. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    (void)dst;
    (void)src;
    (void)len;
    return -1;
}

/** -1: no row copies. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    (void)dst;
    (void)src;
    (void)len;
    return -1;
}

/** Nothing: no row writes the GDT. */
void gdt_set_tls(uint32_t base)
{
    (void)base;
}

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/** Fill `u` with the entry `entry`, the base `base`, a limit of 0xfffff
 * and the flags `flags`. */
static void mk(user_desc_t *u, unsigned int entry, unsigned int base, unsigned int flags)
{
    u->entry_number = entry;
    u->base_addr = base;
    u->limit = 0xfffff;
    u->flags = flags;
}

/** Run every check; the exit status is the count of failed checks, at most
 * 255. */
int main(void)
{
    user_desc_t u;
    unsigned int base;

    /* What musl's __set_thread_area passes: entry -1, flags 0x51. */
    mk(&u, (unsigned int)-1, 0x0804a000u, UD_SEG_32BIT | UD_LIMIT_IN_PAGES | UD_USEABLE);
    base = 0;
    CHECK(tls_resolve_base(&u, &base) == 0);
    CHECK(base == 0x0804a000u);

    /* Entry 6, the TLS entry, named. */
    mk(&u, (unsigned int)GDT_TLS_ENTRY, 0xbf000000u, UD_SEG_32BIT | UD_LIMIT_IN_PAGES);
    base = 0;
    CHECK(tls_resolve_base(&u, &base) == 0);
    CHECK(base == 0xbf000000u);

    /* Another entry: there is one TLS entry. */
    mk(&u, 5u, 0x1000u, UD_SEG_32BIT);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* Not a 32-bit segment. */
    mk(&u, (unsigned int)-1, 0x1000u, UD_LIMIT_IN_PAGES);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* A code or stack segment (contents not 0). */
    mk(&u, (unsigned int)-1, 0x1000u, UD_SEG_32BIT | 0x02u);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* Read-only or execute-only. */
    mk(&u, (unsigned int)-1, 0x1000u, UD_SEG_32BIT | UD_READ_EXEC_ONLY);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* Not present. */
    mk(&u, (unsigned int)-1, 0x1000u, UD_SEG_32BIT | UD_SEG_NOT_PRESENT);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* A null argument. */
    mk(&u, (unsigned int)-1, 0x1000u, UD_SEG_32BIT);
    CHECK(tls_resolve_base(0, &base) == -1);
    CHECK(tls_resolve_base(&u, 0) == -1);

    return fails > 255 ? 255 : fails;
}
