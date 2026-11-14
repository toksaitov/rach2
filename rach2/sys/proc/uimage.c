/* uimage.c: the initial stack of a new image, as uimage.h describes it. */

#include "uimage.h"
#include "auxv.h"
#include "sched.h"
#include "../timer.h"
#include "sigframe.h"
#include "rseq.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"

/* A temporary mapping of one page of the new stack, kept while the
 * accesses made through it stay in that page. */
struct uwin {
    const struct uimage *u;
    uintptr_t page;
    uint8_t *map;
};

/** The frame that backs `addr`, which lies below the stack's top and above
 * its lowest frame. */
static uintptr_t uimage_frame(const struct uimage *u, uintptr_t addr)
{
    return u->frames[(u->top - 1u - addr) / PAGE_SIZE];
}

/** The kernel address of stack address `addr` through `w`, which maps the
 * page when it is not the one mapped; NULL when no temporary mapping was
 * free. */
static uint8_t *uwin_at(struct uwin *w, uintptr_t addr)
{
    uintptr_t page = addr & ~(uintptr_t)(PAGE_SIZE - 1u);

    if (w->map == NULL || page != w->page) {
        if (w->map != NULL) {
            kunmap_temp(w->map);
        }
        w->map = kmap_temp(uimage_frame(w->u, addr));
        w->page = page;
        if (w->map == NULL) {
            return NULL;
        }
    }
    return w->map + (addr - page);
}

/** Release `w`'s mapping, if it has one. */
static void uwin_close(struct uwin *w)
{
    if (w->map != NULL) {
        kunmap_temp(w->map);
        w->map = NULL;
    }
}

/** Start `w` over `u` with nothing mapped. */
static void uwin_open(struct uwin *w, const struct uimage *u)
{
    w->u = u;
    w->page = 0;
    w->map = NULL;
}

uint32_t uimage_budget(unsigned long stack_limit)
{
    unsigned long limit = stack_limit / 4u;

    if (limit > UIMAGE_ARGS_MAX) {
        limit = UIMAGE_ARGS_MAX;
    }
    if (limit < UIMAGE_ARGS_MIN) {
        limit = UIMAGE_ARGS_MIN;
    }
    return (uint32_t)limit;
}

/**
 * An empty argv has a pointer set aside too, since execve gives such a
 * program an empty argv[0]. The array of frames is sized for the budget,
 * with four pages to spare for the pad and the information block below the
 * strings, and is the only kernel memory the stack takes.
 */
int uimage_start(struct uimage *u, uintptr_t top, unsigned long stack_limit, uint32_t argc,
                 uint32_t envc)
{
    uint32_t limit = uimage_budget(stack_limit);
    uint32_t word = (uint32_t)sizeof(unsigned long);
    uint32_t pointers;

    k_memset(u, 0, sizeof(*u));
    u->word = word;
    u->top = top;
    if (argc > limit / word || envc > limit / word) {
        return -E2BIG;
    }
    pointers = ((argc > 0 ? argc : 1u) + envc) * word;
    if (pointers >= limit) {
        return -E2BIG;
    }
    u->room = limit - pointers;
    u->cap = limit / PAGE_SIZE + 4u;
    u->frames = kzalloc(u->cap * sizeof(u->frames[0]));
    return u->frames != NULL ? 0 : -ENOMEM;
}

/**
 * Take frames until they cover the `bytes` below the top, each zeroed
 * through a temporary mapping before anything is written to it.
 *
 * @return 0; -E2BIG past the frames the array has room for; -ENOMEM when no
 *         frame, or no temporary mapping to clear it through, could be had.
 */
static int uimage_grow(struct uimage *u, uint32_t bytes)
{
    uint32_t need = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;

    if (need > u->cap) {
        return -E2BIG;
    }
    while (u->nframes < need) {
        uintptr_t frame = alloc_frame();

        if (frame == 0) {
            return -ENOMEM;
        }
        if (paging_zero_frame(frame) != 0) {
            free_frame(frame);
            return -ENOMEM;
        }
        u->frames[u->nframes++] = frame;
        sched_service_point();
    }
    return 0;
}

/**
 * Copy a string of `len` bytes, its NUL counted, below the strings already
 * there: from user space at `user` when it is not NULL, else from `kernel`.
 *
 * The last byte is written as the NUL whatever the source has by then, so
 * every string on the stack ends before the next one begins, and a walk
 * from the lowest string cannot run past the top.
 *
 * @return 0; -E2BIG for a string past UIMAGE_STRING_MAX or past the room
 *         left; -EFAULT; -ENOMEM.
 */
static int uimage_copy(struct uimage *u, const char __user *user, const char *kernel, uint32_t len)
{
    struct uwin w;
    uintptr_t start;
    uint32_t done = 0;
    int rc;

    if (len > UIMAGE_STRING_MAX || len > u->room - u->used) {
        return -E2BIG;
    }
    rc = uimage_grow(u, u->used + len);
    if (rc != 0) {
        return rc;
    }
    start = u->top - u->used - len;
    uwin_open(&w, u);
    while (done < len) {
        uintptr_t addr = start + done;
        uint32_t chunk = PAGE_SIZE - (uint32_t)(addr & (PAGE_SIZE - 1u));
        uint8_t *dst = uwin_at(&w, addr);

        if (chunk > len - done) {
            chunk = len - done;
        }
        if (dst == NULL) {
            return -ENOMEM;
        }
        if (user != NULL) {
            if (copy_from_user(dst, user + done, chunk) != 0) {
                uwin_close(&w);
                return -EFAULT;
            }
        } else {
            k_memcpy(dst, kernel + done, chunk);
        }
        done += chunk;
        if (done == len) {
            dst[chunk - 1u] = '\0';
        }
    }
    uwin_close(&w);
    if (u->used == 0) {
        u->execfn = start;
    }
    u->used += len;
    return 0;
}

int uimage_push_user(struct uimage *u, const char __user *s)
{
    int n = strnlen_user(s, UIMAGE_STRING_MAX);

    if (n == -EFAULT) {
        return -EFAULT;
    }
    if (n < 0) {
        return -E2BIG;
    }
    return uimage_copy(u, s, NULL, (uint32_t)n + 1u);
}

int uimage_push(struct uimage *u, const char *s)
{
    return uimage_copy(u, NULL, s, (uint32_t)k_strlen(s) + 1u);
}

int uimage_drop_first(struct uimage *u)
{
    struct uwin w;
    uintptr_t addr = u->top - u->used;

    uwin_open(&w, u);
    while (addr < u->top) {
        uint8_t *p = uwin_at(&w, addr);
        uint8_t c;

        if (p == NULL) {
            return -ENOMEM;
        }
        c = *p;
        *p = 0;
        addr++;
        if (c == 0) {
            break;
        }
    }
    uwin_close(&w);
    u->used = (uint32_t)(u->top - addr);
    return 0;
}

void uimage_move(struct uimage *u, uintptr_t top, uint32_t pad)
{
    if (u->execfn != 0) {
        u->execfn = u->execfn - u->top + top;
    }
    u->top = top;
    u->pad = pad;
}

/** Where AT_PLATFORM's string goes, just below the strings and the pad,
 * as Linux's `create_elf_tables` copies it; the pad's end without one. */
static uintptr_t uimage_platform_at(const struct uimage *u)
{
    uintptr_t at = u->top - u->used - u->pad;

    return u->platform != NULL ? at - (k_strlen(u->platform) + 1u) : at;
}

/** Where the 16 bytes AT_RANDOM points at go: below the strings, the pad
 * and AT_PLATFORM's string, aligned down to 16 bytes as Linux aligns them,
 * so no word of them crosses a page. */
static uintptr_t uimage_random_at(const struct uimage *u)
{
    return (uimage_platform_at(u) - 16u) & ~(uintptr_t)15u;
}

int uimage_place(struct uimage *u, uint32_t argc, uint32_t envc)
{
    uint32_t words = 3u + argc + envc + 2u * UIMAGE_AUXV;

    u->sp = (uimage_random_at(u) - words * u->word) & ~(uintptr_t)15u;
    return uimage_grow(u, (uint32_t)(u->top - u->sp));
}

/** Write the word `value`, of `w`'s stack's word size, at `*at` through `w`,
 * and step `*at` past it; 0, or -1 when no temporary mapping was free. The
 * information block is aligned, so no word crosses a page. */
static int uimage_put(struct uwin *w, uintptr_t *at, unsigned long value)
{
    uint8_t *p = uwin_at(w, *at);
    uint32_t v32 = (uint32_t)value;

    if (p == NULL) {
        return -1;
    }
    if (w->u->word == 4u) {
        k_memcpy(p, &v32, sizeof(v32));
    } else {
        k_memcpy(p, &value, sizeof(value));
    }
    *at += w->u->word;
    return 0;
}

/** Copy the string `s`, its NUL included, to the stack address `at`
 * through `w`, a byte at a time, since it may cross a page; 0, or -1 when
 * no temporary mapping was free. */
static int uimage_put_string(struct uwin *w, uintptr_t at, const char *s)
{
    do {
        uint8_t *p = uwin_at(w, at++);

        if (p == NULL) {
            return -1;
        }
        *p = (uint8_t)*s;
    } while (*s++ != '\0');
    return 0;
}

/** Append the pair `type`, `value` to the vector `aux` at `*n` words. */
static void uimage_aux_add(unsigned long *aux, uint32_t *n, unsigned long type, unsigned long value)
{
    aux[(*n)++] = type;
    aux[(*n)++] = value;
}

/** Keep the vector `aux`, 2 * UIMAGE_AUXV words, in `u->auxv` as the stack
 * has it: in words of `u->word` bytes, packed from the array's start. */
static void uimage_keep_auxv(struct uimage *u, const unsigned long *aux)
{
    uint32_t i;

    if (u->word != 4u) {
        k_memcpy(u->auxv, aux, sizeof(u->auxv));
        return;
    }
    k_memset(u->auxv, 0, sizeof(u->auxv));
    for (i = 0; i < 2u * UIMAGE_AUXV; i++) {
        uint32_t v = (uint32_t)aux[i];

        k_memcpy((uint8_t *)u->auxv + i * 4u, &v, sizeof(v));
    }
}

/** Step `*str` from the start of a string on the stack to the start of the
 * next, reading through `w`; 0, or -1 when no temporary mapping was free. */
static int uimage_next(struct uwin *w, uintptr_t *str)
{
    while (*str < w->u->top) {
        uint8_t *p = uwin_at(w, *str);

        if (p == NULL) {
            return -1;
        }
        (*str)++;
        if (*p == 0) {
            break;
        }
    }
    return 0;
}

/**
 * The pointers are found by walking the strings from the lowest, argv[0],
 * as Linux's `create_elf_tables` finds them; the walk stops before any
 * string above the environment, such as the program's path.
 */
int uimage_write(struct uimage *u, uint32_t argc, uint32_t envc, const elf_image_t *img,
                 const uint8_t random16[16], const struct uimage_aux *ids)
{
    struct uwin out;
    struct uwin in;
    uintptr_t random_at = uimage_random_at(u);
    unsigned long aux[2u * UIMAGE_AUXV];
    uintptr_t at;
    uintptr_t str = u->top - u->used;
    uint8_t *random;
    uint32_t i;
    uint32_t n = 0;
    int rc = 0;

    uimage_aux_add(aux, &n, AT_PHDR, img->phdr_vaddr);
    uimage_aux_add(aux, &n, AT_PHENT, img->phent);
    uimage_aux_add(aux, &n, AT_PHNUM, img->phnum);
    uimage_aux_add(aux, &n, AT_ENTRY, img->entry);
    uimage_aux_add(aux, &n, AT_PAGESZ, PAGE_SIZE);
    uimage_aux_add(aux, &n, AT_CLKTCK, USER_HZ);
    uimage_aux_add(aux, &n, AT_SECURE, ids->secure ? 1u : 0u);
    uimage_aux_add(aux, &n, AT_RANDOM, random_at);
    uimage_aux_add(aux, &n, AT_BASE, ids->base);
    uimage_aux_add(aux, &n, AT_UID, ids->uid);
    uimage_aux_add(aux, &n, AT_EUID, ids->euid);
    uimage_aux_add(aux, &n, AT_GID, ids->gid);
    uimage_aux_add(aux, &n, AT_EGID, ids->egid);
    uimage_aux_add(aux, &n, AT_EXECFN, u->execfn);
    uimage_aux_add(aux, &n, AT_MINSIGSTKSZ, sigframe_stack_bytes());
    uimage_aux_add(aux, &n, AT_RSEQ_FEATURE_SIZE, RSEQ_FEATURE_SIZE);
    uimage_aux_add(aux, &n, AT_RSEQ_ALIGN, RSEQ_ALIGN);
    uimage_aux_add(aux, &n, AT_HWCAP, ids->hwcap);
    uimage_aux_add(aux, &n, AT_FLAGS, 0);
    uimage_aux_add(aux, &n, AT_HWCAP2, ids->hwcap2);
    if (u->platform != NULL) {
        uimage_aux_add(aux, &n, AT_PLATFORM, uimage_platform_at(u));
    }
    if (ids->sysinfo != 0) {
        uimage_aux_add(aux, &n, AT_SYSINFO, ids->sysinfo);
    }
    if (ids->sysinfo_ehdr != 0) {
        uimage_aux_add(aux, &n, AT_SYSINFO_EHDR, ids->sysinfo_ehdr);
    }
    while (n < 2u * UIMAGE_AUXV) {
        uimage_aux_add(aux, &n, AT_NULL, 0);
    }

    uwin_open(&out, u);
    uwin_open(&in, u);
    if (u->platform != NULL && uimage_put_string(&out, uimage_platform_at(u), u->platform) != 0) {
        rc = -1;
    }
    random = rc == 0 ? uwin_at(&out, random_at) : NULL;
    if (random == NULL) {
        rc = -1;
    } else {
        k_memcpy(random, random16, 16);
    }
    at = u->sp;
    u->arg_start = str;
    u->env_start = str;
    if (rc == 0) {
        rc = uimage_put(&out, &at, argc);
    }
    for (i = 0; i < argc + envc && rc == 0; i++) {
        if (i == argc) {
            rc = uimage_put(&out, &at, 0);
            u->env_start = str;
        }
        if (rc == 0) {
            rc = uimage_put(&out, &at, str);
        }
        if (rc == 0) {
            rc = uimage_next(&in, &str);
        }
    }
    if (envc == 0) {
        u->env_start = str;
    }
    u->env_end = str;
    if (rc == 0 && envc == 0) {
        rc = uimage_put(&out, &at, 0);
    }
    if (rc == 0) {
        rc = uimage_put(&out, &at, 0);
    }
    for (i = 0; i < 2u * UIMAGE_AUXV && rc == 0; i++) {
        rc = uimage_put(&out, &at, aux[i]);
    }
    uimage_keep_auxv(u, aux);
    uwin_close(&out);
    uwin_close(&in);
    return rc;
}

int uimage_map(struct uimage *u, struct page_dir *dir)
{
    while (u->given < u->nframes) {
        uintptr_t page = u->top - (uintptr_t)(u->given + 1u) * PAGE_SIZE;

        if (map_user_frame(dir, page, u->frames[u->given], PTE_P | PTE_W | PTE_U | PTE_NOEXEC) !=
            0) {
            return -1;
        }
        u->given++;
    }
    return 0;
}

uintptr_t uimage_bottom(const struct uimage *u)
{
    return u->top - (uintptr_t)u->nframes * PAGE_SIZE;
}

void uimage_release(struct uimage *u)
{
    uint32_t i;

    for (i = u->given; i < u->nframes; i++) {
        free_frame(u->frames[i]);
    }
    kfree(u->frames);
    u->frames = NULL;
    u->cap = 0;
    u->nframes = 0;
    u->given = 0;
}
