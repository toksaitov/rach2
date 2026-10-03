/**
 * Host test for the initial stack of a new image (proc/uimage.c).
 *
 * host-deps: rach2/sys/proc/uimage.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/kheap_host.c
 *
 * A frame is a page of a pool here, its physical address one page past its
 * index, and the page directory is a table of the pages mapped. Each row
 * builds a stack through the calls execve makes and reads it back through
 * that table, as the new image reads it: argc, the pointer arrays and their
 * NULLs, the strings, the auxiliary vector, AT_RANDOM's bytes, the 16-byte
 * alignment, and the zeroed bytes below, on one page and on many; the
 * budget of a quarter of the stack limit between 128 KiB and 6 MiB with the
 * pointers set aside first; a string of 128 KiB with its NUL and one of a
 * byte more; a string cut where it was measured; argv[0] dropped for a
 * script's interpreter; and a failed frame, mapping or temporary mapping.
 * After every row each frame is mapped or given back, and no temporary
 * mapping is left open.
 */

#include "uimage.h"
#include "auxv.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "mm/paging.h"
#include "mm/usercopy.h"
#include "arch/i686/mmu.h"

extern int dprintf(int fd, const char *format, ...);

#define POOL 1024u
#define TOP  0xC0000000u
#define MIB  (1024u * 1024u)

/* A word of the stack: argc, a pointer, an auxiliary vector entry. */
#define W ((uint32_t)sizeof(unsigned long))

static uint8_t pool[POOL][PAGE_SIZE];
static uint8_t taken[POOL];
static uint32_t live;
static int frames_left = -1;
static int maps_left = -1;
static int kmap_fails;
static int kmaps_open;
static uint32_t map_va[POOL];
static uint32_t map_frame[POOL];
static uint32_t map_flags[POOL];
static uint32_t nmaps;
static page_dir_t dir;
static char bad[16] = "unreadable";
static char racy[8] = "abcdef";
static int failures;

#define CHECK(c, detail)                                   \
    do {                                                   \
        if (!(c)) {                                        \
            dprintf(2, "uimage-test: fail: %s\n", detail); \
            failures++;                                    \
        }                                                  \
    } while (0)

/** The frame size the architecture would give, a value the vector must
 * carry as it is. */
uintptr_t sigframe_stack_bytes(void)
{
    return 3389u;
}

/** A frame of the pool, filled with a pattern the stack must not show. */
uintptr_t alloc_frame(void)
{
    uint32_t i;

    if (frames_left == 0) {
        return 0;
    }
    for (i = 0; i < POOL; i++) {
        if (!taken[i]) {
            taken[i] = 1;
            live++;
            k_memset(pool[i], 0xA5, PAGE_SIZE);
            if (frames_left > 0) {
                frames_left--;
            }
            return (i + 1u) * PAGE_SIZE;
        }
    }
    return 0;
}

void free_frame(uintptr_t phys)
{
    uint32_t i = phys / PAGE_SIZE - 1u;

    CHECK(i < POOL && taken[i], "a frame is given back once, after it was taken");
    if (i < POOL && taken[i]) {
        taken[i] = 0;
        live--;
    }
}

void *kmap_temp(uintptr_t phys)
{
    if (kmap_fails) {
        return 0;
    }
    kmaps_open++;
    return pool[phys / PAGE_SIZE - 1u];
}

void kunmap_temp(void *va)
{
    (void)va;
    kmaps_open--;
}

int paging_zero_frame(uintptr_t phys)
{
    k_memset(pool[phys / PAGE_SIZE - 1u], 0, PAGE_SIZE);
    return 0;
}

int map_user_frame(page_dir_t *d, uintptr_t virt, uintptr_t phys, uint32_t flags)
{
    CHECK(d == &dir, "a frame is mapped into the directory given");
    if (maps_left == 0) {
        return -1;
    }
    if (maps_left > 0) {
        maps_left--;
    }
    map_va[nmaps] = virt;
    map_frame[nmaps] = phys;
    map_flags[nmaps] = flags;
    nmaps++;
    return 0;
}

/** Whether `p` points into the bytes that fault. */
static int is_bad(const char *p)
{
    return p >= bad && p < bad + sizeof(bad);
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (is_bad(src)) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

/** The length of a user string; `racy` measures 3, as a string another
 * thread lengthens after it was measured would. */
int strnlen_user(const char *src, uint32_t n)
{
    uint32_t i;

    if (is_bad(src)) {
        return -EFAULT;
    }
    if (src == racy) {
        return 3;
    }
    for (i = 0; i < n; i++) {
        if (src[i] == '\0') {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;
}

void sched_service_point(void)
{}

/** The byte at user address `va` through the directory, or NULL when no
 * page is mapped there. */
static uint8_t *at(uint32_t va)
{
    uint32_t i;

    for (i = 0; i < nmaps; i++) {
        if (map_va[i] == (va & ~(PAGE_SIZE - 1u))) {
            return pool[map_frame[i] / PAGE_SIZE - 1u] + (va & (PAGE_SIZE - 1u));
        }
    }
    return 0;
}

/** The byte at stack address `va` through `u`'s own frames, before they
 * are mapped. */
static uint8_t *frame_at(const struct uimage *u, uint32_t va)
{
    uint32_t frame = u->frames[(TOP - 1u - va) / PAGE_SIZE];

    return pool[frame / PAGE_SIZE - 1u] + (va & (PAGE_SIZE - 1u));
}

/** Whether the string at stack address `va` in `u`'s own frames is `s`. */
static int frame_str_is(const struct uimage *u, uint32_t va, const char *s)
{
    for (;; va++, s++) {
        if (*frame_at(u, va) != (uint8_t)*s) {
            return 0;
        }
        if (*s == '\0') {
            return 1;
        }
    }
}

/** The word at user address `va`, or 0xDEADBEEF when it is not mapped. */
static unsigned long word(uintptr_t va)
{
    uint8_t *p = at((uint32_t)va);
    unsigned long w = 0xDEADBEEFu;

    if (p != 0) {
        k_memcpy(&w, p, sizeof(w));
    }
    return w;
}

/** Whether the string at user address `va` is `s`. */
static int str_is(unsigned long va, const char *s)
{
    for (;; va++, s++) {
        uint8_t *p = at((uint32_t)va);

        if (p == 0 || *p != (uint8_t)*s) {
            return 0;
        }
        if (*s == '\0') {
            return 1;
        }
    }
}

/** Give back the mapped frames, as the directory's teardown does, and
 * check that nothing else is held. */
static void end_row(const char *row)
{
    uint32_t i;

    for (i = 0; i < nmaps; i++) {
        free_frame(map_frame[i]);
    }
    nmaps = 0;
    frames_left = -1;
    maps_left = -1;
    kmap_fails = 0;
    CHECK(live == 0, row);
    CHECK(kmaps_open == 0, row);
}

/** A program image for the auxiliary vector. */
static void image(elf_image_t *img)
{
    k_memset(img, 0, sizeof(*img));
    img->entry = 0x08048100u;
    img->phdr_vaddr = 0x08048034u;
    img->phent = 32;
    img->phnum = 3;
}

/** The 16 bytes for AT_RANDOM. */
static void random_bytes(uint8_t rnd[16])
{
    int i;

    for (i = 0; i < 16; i++) {
        rnd[i] = (uint8_t)(i * 7 + 1);
    }
}

/** The interpreter's base, the ids and AT_SECURE for the auxiliary vector:
 * a set-user-id image when `secure`. */
static void aux_ids(struct uimage_aux *aux, int secure)
{
    aux->base = 0xAFF00000u;
    aux->uid = 1000u;
    aux->euid = secure ? 0u : 1000u;
    aux->gid = 100u;
    aux->egid = 100u;
    aux->secure = secure;
}

/** Check the auxiliary vector at `va` against `img`, `rnd`, `aux` and the
 * program's `path`, in the order the stack has it. */
static void check_auxv(uintptr_t va, const elf_image_t *img, const uint8_t rnd[16],
                       const struct uimage_aux *aux, const char *path)
{
    unsigned long want[15];
    unsigned long rnd_at = word(va + 15u * W);
    int secure = aux->secure;
    int i;

    want[0] = AT_PHDR;
    want[1] = img->phdr_vaddr;
    want[2] = AT_PHENT;
    want[3] = img->phent;
    want[4] = AT_PHNUM;
    want[5] = img->phnum;
    want[6] = AT_ENTRY;
    want[7] = img->entry;
    want[8] = AT_PAGESZ;
    want[9] = PAGE_SIZE;
    want[10] = AT_CLKTCK;
    want[11] = 100;
    want[12] = AT_SECURE;
    want[13] = (unsigned long)secure;
    want[14] = AT_RANDOM;
    for (i = 0; i < 15; i++) {
        CHECK(word(va + W * (uint32_t)i) == want[i],
              "the auxiliary vector has the entries execve gives");
    }
    want[0] = AT_BASE;
    want[1] = aux->base;
    want[2] = AT_UID;
    want[3] = aux->uid;
    want[4] = AT_EUID;
    want[5] = aux->euid;
    want[6] = AT_GID;
    want[7] = aux->gid;
    want[8] = AT_EGID;
    want[9] = aux->egid;
    want[10] = AT_EXECFN;
    for (i = 0; i < 11; i++) {
        CHECK(word(va + W * (16u + (uint32_t)i)) == want[i],
              "the auxiliary vector has the interpreter's base and the ids");
    }
    CHECK(str_is(word(va + 27u * W), path), "AT_EXECFN points at the program's path");
    CHECK(word(va + 28u * W) == AT_MINSIGSTKSZ && word(va + 29u * W) == 3389u,
          "AT_MINSIGSTKSZ gives the stack a signal frame needs");
    CHECK(word(va + 30u * W) == AT_NULL && word(va + 31u * W) == 0,
          "the auxiliary vector ends in AT_NULL");
    CHECK((rnd_at & 15u) == 0, "AT_RANDOM is aligned to 16 bytes");
    for (i = 0; i < 16; i++) {
        uint8_t *p = at((uint32_t)rnd_at + (uint32_t)i);

        CHECK(p != 0 && *p == rnd[i], "AT_RANDOM points at the bytes given");
    }
}

/** Push the path, then `envc` strings of `envp` and `argc` of `argv`, each
 * last first, as execve does; the first error, or 0. */
static int push_all(struct uimage *u, const char *path, char **argv, uint32_t argc, char **envp,
                    uint32_t envc)
{
    int rc = uimage_push_user(u, path);

    while (rc == 0 && envc > 0) {
        rc = uimage_push_user(u, envp[--envc]);
    }
    while (rc == 0 && argc > 0) {
        rc = uimage_push_user(u, argv[--argc]);
    }
    return rc;
}

/** Two arguments and two variables on one page: the layout word by word. */
static void row_one_page(void)
{
    static char *argv[] = {"prog", "arg one"};
    static char *envp[] = {"PATH=/bin", "HOME=/"};
    struct uimage u;
    elf_image_t img;
    uint8_t rnd[16];
    struct uimage_aux aux;
    uintptr_t sp;
    uint32_t va;
    int zero = 1;

    image(&img);
    random_bytes(rnd);
    aux_ids(&aux, 0);
    CHECK(uimage_start(&u, TOP, 8u * MIB, 2, 2) == 0, "one page: the stack is started");
    CHECK(push_all(&u, "/bin/prog", argv, 2, envp, 2) == 0, "one page: the strings are pushed");
    CHECK(uimage_place(&u, 2, 2) == 0, "one page: the information block is placed");
    CHECK(uimage_write(&u, 2, 2, &img, rnd, &aux) == 0, "one page: the stack is written");
    CHECK(uimage_map(&u, &dir) == 0, "one page: the stack is mapped");
    sp = u.sp;
    CHECK(u.nframes == 1 && uimage_bottom(&u) == TOP - PAGE_SIZE,
          "one page: the stack takes one page below the top");
    CHECK(nmaps == 1 && map_va[0] == TOP - PAGE_SIZE && map_flags[0] == 0x7u,
          "one page: the page is mapped user and writable below the top");
    CHECK((sp & 15u) == 0 && sp >= TOP - PAGE_SIZE,
          "one page: the stack pointer is aligned to 16 bytes");
    CHECK(word(sp) == 2, "one page: argc is 2");
    CHECK(str_is(word(sp + W), "prog") && str_is(word(sp + 2u * W), "arg one"),
          "one page: argv points at its strings");
    CHECK(word(sp + 3u * W) == 0, "one page: argv ends in NULL");
    CHECK(str_is(word(sp + 4u * W), "PATH=/bin") && str_is(word(sp + 5u * W), "HOME=/"),
          "one page: envp points at its strings");
    CHECK(word(sp + 6u * W) == 0, "one page: envp ends in NULL");
    check_auxv(sp + 7u * W, &img, rnd, &aux, "/bin/prog");
    CHECK(str_is(u.arg_start, "prog") && str_is(u.env_start, "PATH=/bin") &&
              u.env_end == u.execfn && u.auxv[0] == AT_PHDR && u.auxv[27] == u.execfn,
          "one page: the strings' ranges and the auxiliary vector are kept for /proc");
    CHECK(str_is(TOP - 10u, "/bin/prog"), "one page: the path is at the top");
    for (va = TOP - PAGE_SIZE; va < sp; va++) {
        uint8_t *p = at(va);

        zero = zero && p != 0 && *p == 0;
    }
    CHECK(zero, "one page: the bytes below the stack pointer are zero");
    uimage_release(&u);
    end_row("one page: every frame is mapped or given back");
}

/** Three thousand arguments and a long variable, on pages enough for them. */
static void row_many_pages(void)
{
    static char names[3000][8];
    static char *argv[3000];
    static char env[10001];
    char *envp[1];
    struct uimage u;
    elf_image_t img;
    uint8_t rnd[16];
    struct uimage_aux aux;
    uint32_t i;
    int right = 1;

    for (i = 0; i < 3000; i++) {
        names[i][0] = 'a';
        names[i][1] = (char)('0' + i / 1000);
        names[i][2] = (char)('0' + i / 100 % 10);
        names[i][3] = (char)('0' + i / 10 % 10);
        names[i][4] = (char)('0' + i % 10);
        names[i][5] = '\0';
        argv[i] = names[i];
    }
    k_memset(env, 'e', 10000);
    env[10000] = '\0';
    envp[0] = env;
    image(&img);
    random_bytes(rnd);
    aux_ids(&aux, 0);
    CHECK(uimage_start(&u, TOP, 8u * MIB, 3000, 1) == 0, "many pages: the stack is started");
    CHECK(push_all(&u, "/p", argv, 3000, envp, 1) == 0, "many pages: the strings are pushed");
    CHECK(uimage_place(&u, 3000, 1) == 0, "many pages: the information block is placed");
    CHECK(uimage_write(&u, 3000, 1, &img, rnd, &aux) == 0, "many pages: the stack is written");
    CHECK(uimage_map(&u, &dir) == 0, "many pages: the stack is mapped");
    CHECK(u.nframes == (TOP - u.sp + PAGE_SIZE - 1u) / PAGE_SIZE && u.nframes > 5,
          "many pages: the frames cover the stack");
    CHECK(uimage_bottom(&u) == TOP - u.nframes * PAGE_SIZE,
          "many pages: the bottom is the lowest frame's page");
    for (i = 0; i < u.nframes; i++) {
        right = right && nmaps == u.nframes && map_va[i] == TOP - (i + 1u) * PAGE_SIZE;
    }
    CHECK(right, "many pages: each page is mapped where the stack is");
    CHECK(word(u.sp) == 3000, "many pages: argc is 3000");
    for (i = 0; i < 3000; i++) {
        right = right && str_is(word(u.sp + W * (1u + i)), names[i]);
    }
    CHECK(right, "many pages: each argv pointer points at its string");
    CHECK(word(u.sp + W * 3001u) == 0 && str_is(word(u.sp + W * 3002u), env) &&
              word(u.sp + W * 3003u) == 0,
          "many pages: envp points at its string and ends in NULL");
    check_auxv(u.sp + W * 3004u, &img, rnd, &aux, "/p");
    uimage_release(&u);
    end_row("many pages: every frame is mapped or given back");
}

/** The budget: a quarter of the stack limit, between 128 KiB and 6 MiB. */
static void row_budget(void)
{
    CHECK(uimage_budget(0) == 131072u, "budget: a limit of 0 gives 128 KiB");
    CHECK(uimage_budget(512u * 1024u) == 131072u, "budget: 512 KiB gives 128 KiB");
    CHECK(uimage_budget(MIB) == 262144u, "budget: 1 MiB gives 256 KiB");
    CHECK(uimage_budget(MIB + 7u) == 262145u, "budget: 1 MiB and 7 bytes gives a quarter of it");
    CHECK(uimage_budget(8u * MIB) == 2u * MIB, "budget: 8 MiB gives 2 MiB");
    CHECK(uimage_budget(24u * MIB) == 6u * MIB, "budget: 24 MiB gives 6 MiB");
    CHECK(uimage_budget(64u * MIB) == 6u * MIB, "budget: 64 MiB gives 6 MiB");
    CHECK(uimage_budget(0xFFFFFFFFu) == 6u * MIB, "budget: an unlimited stack gives 6 MiB");
}

/** The pointers are set aside before any string: one for argv[0] even when
 * argv is empty. */
static void row_pointers(void)
{
    struct uimage u;

    CHECK(uimage_start(&u, TOP, 0, 131072u / W, 0) == -E2BIG,
          "pointers: pointers that take the whole budget are refused");
    uimage_release(&u);
    CHECK(uimage_start(&u, TOP, 0, 0, 131072u / W - 1u) == -E2BIG,
          "pointers: an empty argv has a pointer set aside for argv[0]");
    uimage_release(&u);
    CHECK(uimage_start(&u, TOP, 0, 131072u / W - 1u, 0) == 0 && u.room == W,
          "pointers: the budget less one pointer is taken");
    CHECK(uimage_push(&u, &"abcdefg"[8u - W]) == 0 && uimage_push(&u, "") == -E2BIG,
          "pointers: the strings get what the pointers leave");
    uimage_release(&u);
    end_row("pointers: every frame is given back");
}

/** One string of 128 KiB with its NUL, and not a byte more; a string that
 * cannot be read; and one cut where it was measured. */
static void row_string(void)
{
    static char s[131074];
    struct uimage u;
    uint32_t used;

    k_memset(s, 's', sizeof(s) - 1u);
    s[sizeof(s) - 1u] = '\0';
    CHECK(uimage_start(&u, TOP, 8u * MIB, 1, 0) == 0, "string: the stack is started");
    CHECK(uimage_push_user(&u, s + 1) == -E2BIG,
          "string: a string of 128 KiB and a NUL is refused");
    CHECK(u.used == 0, "string: a refused string takes no room");
    CHECK(uimage_push_user(&u, s + 2) == 0 && u.used == 131072u,
          "string: a string of 128 KiB with its NUL is taken");
    used = u.used;
    CHECK(uimage_push_user(&u, bad) == -EFAULT && u.used == used,
          "string: an unreadable string is -EFAULT");
    CHECK(uimage_push_user(&u, racy) == 0 && u.used == used + 4u &&
              frame_str_is(&u, TOP - u.used, "abc"),
          "string: a string that grew after it was measured ends where it was measured");
    uimage_release(&u);
    end_row("string: every frame is given back");
}

/** The strings fill the budget to its last byte, and no further. */
static void row_budget_edge(void)
{
    static char s[131072];
    struct uimage u;

    CHECK(uimage_start(&u, TOP, 0, 2, 0) == 0 && u.room == 131072u - 2u * W,
          "edge: the strings have the budget less the pointers");
    k_memset(s, 'b', u.room - 3u);
    s[u.room - 3u] = '\0';
    CHECK(uimage_push_user(&u, "/p") == 0, "edge: the path is taken");
    CHECK(uimage_push_user(&u, s) == -E2BIG, "edge: a string a byte past the budget is refused");
    CHECK(uimage_push_user(&u, s + 1) == 0 && u.used == u.room,
          "edge: the strings may fill the budget to its last byte");
    CHECK(uimage_push_user(&u, "") == -E2BIG, "edge: a string past a full budget is refused");
    CHECK(uimage_place(&u, 2, 0) == 0 && uimage_bottom(&u) <= u.sp,
          "edge: the information block below a full budget has frames");
    uimage_release(&u);
    end_row("edge: every frame is given back");
}

/** A script's interpreter in place of argv[0], as execve puts it. */
static void row_script(void)
{
    static char *argv[] = {"script", "x"};
    static char *envp[] = {"E=1"};
    struct uimage u;
    elf_image_t img;
    uint8_t rnd[16];
    struct uimage_aux aux;
    uint32_t used;
    uint32_t i;
    int cleared = 1;

    image(&img);
    random_bytes(rnd);
    aux_ids(&aux, 1);
    CHECK(uimage_start(&u, TOP, 8u * MIB, 2, 1) == 0, "script: the stack is started");
    CHECK(push_all(&u, "/s/script", argv, 2, envp, 1) == 0, "script: the strings are pushed");
    used = u.used;
    CHECK(uimage_drop_first(&u) == 0 && u.used == used - 7u, "script: argv[0] is dropped");
    for (i = 0; i < 7u; i++) {
        cleared = cleared && *frame_at(&u, TOP - used + i) == 0;
    }
    CHECK(cleared, "script: the bytes of argv[0] are cleared");
    CHECK(uimage_push(&u, "/s/script") == 0 && uimage_push(&u, "-e") == 0 &&
              uimage_push(&u, "/bin/sh") == 0,
          "script: the interpreter's strings are taken");
    CHECK(uimage_place(&u, 4, 1) == 0 && uimage_write(&u, 4, 1, &img, rnd, &aux) == 0 &&
              uimage_map(&u, &dir) == 0,
          "script: the stack is built");
    CHECK(word(u.sp) == 4 && str_is(word(u.sp + W), "/bin/sh") &&
              str_is(word(u.sp + 2u * W), "-e") && str_is(word(u.sp + 3u * W), "/s/script") &&
              str_is(word(u.sp + 4u * W), "x") && word(u.sp + 5u * W) == 0,
          "script: argv is the interpreter, its argument, the script and the rest");
    CHECK(str_is(word(u.sp + 6u * W), "E=1") && word(u.sp + 7u * W) == 0, "script: envp is kept");
    check_auxv(u.sp + 8u * W, &img, rnd, &aux, "/s/script");
    uimage_release(&u);
    end_row("script: every frame is mapped or given back");
}

/** No frame, no mapping, no temporary mapping: each is an error, and what
 * was taken goes back. */
static void row_failures(void)
{
    static char s[20000];
    struct uimage u;
    elf_image_t img;
    uint8_t rnd[16];
    struct uimage_aux aux;

    image(&img);
    random_bytes(rnd);
    aux_ids(&aux, 0);
    k_memset(s, 'f', sizeof(s) - 1u);
    s[sizeof(s) - 1u] = '\0';
    CHECK(uimage_start(&u, TOP, 8u * MIB, 1, 0) == 0, "failures: the stack is started");
    frames_left = 2;
    CHECK(uimage_push_user(&u, s) == -ENOMEM && u.used == 0,
          "failures: a string without its frames is -ENOMEM and takes no room");
    frames_left = -1;
    CHECK(uimage_push_user(&u, s) == 0 && uimage_place(&u, 1, 0) == 0,
          "failures: the string is taken once frames are there");
    kmap_fails = 1;
    CHECK(uimage_write(&u, 1, 0, &img, rnd, &aux) == -1,
          "failures: a write without a temporary mapping is -1");
    kmap_fails = 0;
    maps_left = 2;
    CHECK(uimage_map(&u, &dir) == -1 && u.given == 2 && nmaps == 2,
          "failures: a map that fails keeps the frames not mapped");
    uimage_release(&u);
    CHECK(live == 2, "failures: the frames not mapped are given back");
    end_row("failures: every frame is mapped or given back");
}

int main(void)
{
    row_one_page();
    row_many_pages();
    row_budget();
    row_pointers();
    row_string();
    row_budget_edge();
    row_script();
    row_failures();
    if (failures != 0) {
        return 1;
    }
    dprintf(1, "uimage-test: pass: every row\n");
    return 0;
}
