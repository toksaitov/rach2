/*
 * acpi_mapcache_hosttest.c: the windows of physical memory that ACPICA's OS
 * layer maps, rach2/sys/acpi/mapcache.c, over a model map that records each
 * window it is asked for.
 *
 * The kernel's MMIO mappings have no unmap, so a range must be mapped once
 * and found again: a table read field by field, or a register read a
 * thousand times, takes one window. A table longer than a page must come
 * back contiguous, and a request the cache cannot serve must come back 0,
 * never an address of a window that does not hold it.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/acpi/mapcache.c"

/** The number of failed checks, which is the exit status. */
static int fails;

/** Count and print a failure when `cond` is false; `what` says what held. */
#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/* The model's windows are 1 MiB apart from here, so that two never touch. */
#define MODEL_BASE 0x40000000u
#define MODEL_STEP 0x100000u

/** What the model map was last asked for, and how often. */
static int map_calls;
static uint64_t map_phys, map_len;
static int map_fail; /* the next call fails */

/** The model of `map_mmio`: a fresh address for each window, or 0. */
static uintptr_t model_map(uint64_t phys, uint64_t len)
{
    map_phys = phys;
    map_len = len;
    if (map_fail) {
        map_fail = 0;
        return 0;
    }
    return (uintptr_t)(MODEL_BASE + (uintptr_t)map_calls++ * MODEL_STEP);
}

/** A cache of `cap` windows in `w`, below `limit`, over the model. */
static void fresh(struct acpi_mapcache *c, struct acpi_window *w, int cap, uint64_t limit)
{
    c->windows = w;
    c->cap = cap;
    c->n = 0;
    c->limit = limit;
    c->full = 0;
    c->map = model_map;
    map_calls = 0;
    map_fail = 0;
}

/** Requests that no window can serve: none is made and 0 comes back. */
static void refusals(void)
{
    struct acpi_window w[4];
    struct acpi_mapcache c;

    fresh(&c, w, 4, (uint64_t)1 << 32);
    CHECK(acpi_mapcache_get(&c, 0x1000, 0) == 0, "no bytes give 0");
    CHECK(acpi_mapcache_get(&c, ~(uint64_t)0 - 0x10, 0x20) == 0, "a range that wraps gives 0");
    CHECK(acpi_mapcache_get(&c, ~(uint64_t)0 - 0x800, 0x10) == 0,
          "a range in the last page, whose end cannot be rounded up, gives 0");
    CHECK(acpi_mapcache_get(&c, 0xFFFFFFF0u, 0x20) == 0, "a range past the limit gives 0");
    CHECK(acpi_mapcache_get(&c, 0xFFFFFFF0u, 0x10) != 0,
          "a range that ends at the limit is mapped");
    CHECK(map_calls == 1 && c.n == 1, "only the range inside the limit made a window");
    fresh(&c, w, 4, 0);
    CHECK(acpi_mapcache_get(&c, (uint64_t)5 << 32, 0x10) != 0,
          "with no limit, past 4 GiB is mapped");
}

/** A first request maps its whole pages, and a later one inside is found. */
static void reuse(void)
{
    struct acpi_window w[4];
    struct acpi_mapcache c;
    uintptr_t a, b;

    fresh(&c, w, 4, 0);
    a = acpi_mapcache_get(&c, 0x1234, 0x10);
    CHECK(a == MODEL_BASE + 0x234, "the address keeps the offset within the page");
    CHECK(map_phys == 0x1000 && map_len == 0x1000, "the window is the one page the bytes touch");
    CHECK(acpi_mapcache_get(&c, 0x1234, 0x10) == a, "the same range is found again");
    b = acpi_mapcache_get(&c, 0x1F00, 0x100);
    CHECK(b == MODEL_BASE + 0xF00, "a range that ends at the page's end is in the window");
    CHECK(acpi_mapcache_get(&c, 0x1000, 0x1000) == MODEL_BASE, "the whole page is in the window");
    CHECK(map_calls == 1 && c.n == 1, "the requests inside the page made no second window");
}

/** A range across pages comes back contiguous, from a window of its own. */
static void contiguous(void)
{
    struct acpi_window w[4];
    struct acpi_mapcache c;
    uintptr_t a;

    fresh(&c, w, 4, 0);
    (void)acpi_mapcache_get(&c, 0x5000, 0x24);
    a = acpi_mapcache_get(&c, 0x5800, 0x2000);
    CHECK(map_calls == 2, "a range past the first window's page makes a window of its own");
    CHECK(map_phys == 0x5000 && map_len == 0x3000, "the window is every page the range touches");
    CHECK(a == MODEL_BASE + MODEL_STEP + 0x800, "the address is the new window's, offset");
    CHECK(acpi_mapcache_get(&c, 0x7000, 0x800) == MODEL_BASE + MODEL_STEP + 0x2000,
          "a range in the long window's last page is found in it");
    CHECK(acpi_mapcache_get(&c, 0x5004, 0x8) == MODEL_BASE + 0x4,
          "a range both windows hold comes from the first made");
    CHECK(c.n == 2, "two windows were made");
}

/** The array full, a new window is refused and marked, and old ones serve. */
static void full(void)
{
    struct acpi_window w[2];
    struct acpi_mapcache c;

    fresh(&c, w, 2, 0);
    (void)acpi_mapcache_get(&c, 0x1000, 0x10);
    (void)acpi_mapcache_get(&c, 0x3000, 0x10);
    CHECK(!c.full, "two windows of two leave the cache not full");
    CHECK(acpi_mapcache_get(&c, 0x9000, 0x10) == 0, "a third window gives 0");
    CHECK(c.full, "the refused window marks the cache full");
    CHECK(map_calls == 2, "the refused window was not mapped");
    CHECK(acpi_mapcache_get(&c, 0x3008, 0x8) == MODEL_BASE + MODEL_STEP + 0x8,
          "a range in a window made still comes back");
}

/** A map that fails gives 0 and makes no window. */
static void map_fails(void)
{
    struct acpi_window w[2];
    struct acpi_mapcache c;

    fresh(&c, w, 2, 0);
    map_fail = 1;
    CHECK(acpi_mapcache_get(&c, 0x1000, 0x10) == 0, "a failed map gives 0");
    CHECK(c.n == 0 && !c.full, "a failed map makes no window and leaves the cache not full");
    CHECK(acpi_mapcache_get(&c, 0x1000, 0x10) == MODEL_BASE + 0x0, "the next request maps it");
}

int main(void)
{
    refusals();
    reuse();
    contiguous();
    full();
    map_fails();
    if (fails != 0) {
        printf("acpi_mapcache_hosttest: %d checks failed\n", fails);
        return fails > 255 ? 255 : fails;
    }
    printf("acpi_mapcache_hosttest: ok\n");
    return 0;
}
