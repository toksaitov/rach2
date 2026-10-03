/**
 * Host test for tls_resolve_base (proc/tls.c): the user_desc validation that
 * backs set_thread_area(2).
 *
 * host-deps: rach2/sys/arch/i686/tls.c
 *
 * The bit decoding is the fallible part, so it is exercised natively under
 * ASan/UBSan: a musl-style request is accepted and its base returned, while
 * every malformed request (wrong entry, not 32-bit, code segment, read-only,
 * not-present, NULL args) is rejected. Returns the failed-check count
 * (0 == all passed).
 */

#include "tls.h"
#include "arch/i686/gdt.h" /* GDT_TLS_ENTRY */
#include "mm/usercopy.h"
#include "proc/process.h"

static int fails;

/* The system calls beside the decoding, which no row makes. */
process_t *current_proc;

int copy_from_user(void *dst, const void __user *src, size_t len)
{
    (void)dst;
    (void)src;
    (void)len;
    return -1;
}

int copy_to_user(void __user *dst, const void *src, size_t len)
{
    (void)dst;
    (void)src;
    (void)len;
    return -1;
}

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

/* A musl-style user_desc: allocate (-1), 4 GB data, present, writable. */
static void mk(user_desc_t *u, unsigned int entry, unsigned int base, unsigned int flags)
{
    u->entry_number = entry;
    u->base_addr = base;
    u->limit = 0xfffff;
    u->flags = flags;
}

int main(void)
{
    user_desc_t u;
    unsigned int base;

    /* Exactly what musl's __set_thread_area passes: entry -1, flags 0x51. */
    mk(&u, (unsigned int)-1, 0x0804a000u, UD_SEG_32BIT | UD_LIMIT_IN_PAGES | UD_USEABLE);
    base = 0;
    CHECK(tls_resolve_base(&u, &base) == 0);
    CHECK(base == 0x0804a000u);

    /* An explicit, valid entry 6 is also accepted. */
    mk(&u, (unsigned int)GDT_TLS_ENTRY, 0xbf000000u, UD_SEG_32BIT | UD_LIMIT_IN_PAGES);
    base = 0;
    CHECK(tls_resolve_base(&u, &base) == 0);
    CHECK(base == 0xbf000000u);

    /* A different entry number cannot be represented (one TLS slot). */
    mk(&u, 5u, 0x1000u, UD_SEG_32BIT);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* Not a 32-bit segment. */
    mk(&u, (unsigned int)-1, 0x1000u, UD_LIMIT_IN_PAGES);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* Code/stack segment requested (contents != 0). */
    mk(&u, (unsigned int)-1, 0x1000u, UD_SEG_32BIT | 0x02u);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* Read/exec-only (not writable). */
    mk(&u, (unsigned int)-1, 0x1000u, UD_SEG_32BIT | UD_READ_EXEC_ONLY);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* Not present. */
    mk(&u, (unsigned int)-1, 0x1000u, UD_SEG_32BIT | UD_SEG_NOT_PRESENT);
    CHECK(tls_resolve_base(&u, &base) == -1);

    /* NULL guards. */
    mk(&u, (unsigned int)-1, 0x1000u, UD_SEG_32BIT);
    CHECK(tls_resolve_base(0, &base) == -1);
    CHECK(tls_resolve_base(&u, 0) == -1);

    return fails > 255 ? 255 : fails;
}
