/**
 * vmamusl: check that a process may hold hundreds of mappings at once, and
 * that neighbouring regions merge as Linux merges them.
 *
 * A process's regions are nodes of a tree in the kernel heap, taken in
 * chunks that start with `VMA_FIRST` (16) nodes, up to vm.max_map_count
 * (rach2/sys/mm/vm.h). A program starts with a region per loaded segment and
 * one for its stack, and the heap's comes with its first `brk`; every
 * anonymous `mmap` after that takes one more. A C library's allocator calls
 * `mmap` for each large allocation, so the number of regions bounds the large
 * allocations a program may hold at once, well before memory runs out.
 *
 * The first half takes `WANTED` anonymous pages one at a time and writes to
 * each, since a mapping nothing touches shows only that an address was
 * returned. Each page has its index in two bytes, since one byte would repeat
 * every 256 pages. After the last is taken it checks that every page still
 * has what was written to it, which fails if two regions overlap.
 *
 * The second half never calls `mmap` itself: it asks malloc for
 * `SMALL_BLOCK` bytes at a time and keeps every block in a chain until the
 * end. mallocng maps a new group with `mmap` whenever its groups of that size
 * are full, so this walk takes many regions while it uses little memory. When
 * the table is full the kernel logs, once per process, which process used all
 * its mappings, since malloc's failure alone does not say why.
 *
 * The third half counts the regions /proc/self/maps lists in a range of
 * `MERGE_PAGES` pages between two PROT_NONE guard pages, after each call that
 * Linux's vma_merge answers by merging neighbours of equal protection: the
 * pages mapped one at a time over the guard's reservation, a protection
 * changed and changed back, a page unmapped and mapped again, and the
 * range shrunk and grown back in place with mremap; then two pages of a
 * memfd mapped by two calls, which are one region where the file's offsets
 * continue through one open file; and it grows the heap with brk three
 * times. Linux 6.18 lists one region for the pages at each step but the
 * second and fourth, and one [heap].
 *
 * Prints `VMAMUSL OK` and exits 0 on success, and prints
 * `VMAMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (../probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check: `mappings` (every page was granted), `readback` (each holds
 * what was written to it), `unmap` (each is given back), `small-blocks`
 * (malloc reaches the floor) and `merges` (the regions merge as Linux's do);
 * a failure leaves the checks after it not run.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "../probe_report.h"

/* The pages the first half maps: past the 16 entries the array starts with,
 * so the array must grow, and far under `VMA_LIMIT`, so a region the kernel
 * adds to every process does not make the probe fail. */
#define WANTED 300

/* The second half's block size, and the total it must reach before malloc
 * fails. */
#define SMALL_BLOCK 1024
#define SMALL_FLOOR (8 * 1024 * 1024)

/* The pages of the third half's range, between its two guard pages. */
#define MERGE_PAGES 32

/* /proc/self/maps, read with read(2), since a buffer of stdio's or malloc's
 * could be a new region. */
static char maps_text[1 << 20];

/**
 * Count the regions /proc/self/maps lists that lie in [lo, hi), or, when
 * `name` is not null, those whose line has it. Returns -1 when the file
 * cannot be read whole.
 */
static int count_regions(unsigned long lo, unsigned long hi, const char *name)
{
    char *line, *end;
    unsigned long start;
    size_t got = 0;
    ssize_t n;
    int count = 0;
    int fd;

    fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    while ((n = read(fd, maps_text + got, sizeof(maps_text) - 1 - got)) > 0) {
        got += (size_t)n;
    }
    close(fd);
    if (n < 0 || got == sizeof(maps_text) - 1) {
        return -1;
    }
    maps_text[got] = '\0';
    for (line = maps_text; *line; line = end + 1) {
        end = strchr(line, '\n');
        if (!end) {
            break;
        }
        *end = '\0';
        start = strtoul(line, NULL, 16);
        if (name ? strstr(line, name) != NULL : start >= lo && start < hi) {
            count++;
        }
    }

    return count;
}

/**
 * Map two pages of one memfd at neighbouring addresses between guard pages
 * with two mmap calls and count the regions there as Linux does: the pages
 * are one region where the file's offsets continue, and two where they do
 * not or where the second page maps another open of the file, Linux's
 * other struct file. Returns 0, or 1 with what differed in `detail`.
 */
static int check_file_merges(char *detail, size_t size)
{
    static const char *const cases[] = {"offsets that continue", "offsets that do not",
                                        "a second open of the file"};
    static const int linux_regions[] = {3, 4, 4};
    long pg = sysconf(_SC_PAGESIZE);
    size_t span = 6 * (size_t)pg;
    char path[64];
    char *base, *p, *q;
    int fd, other, got, i;

    fd = memfd_create("vmamusl", 0);
    if (fd < 0 || ftruncate(fd, 4 * pg) != 0) {
        snprintf(detail, size, "the memfd could not be made");
        return 1;
    }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    other = open(path, O_RDWR);
    if (other < 0) {
        close(fd);
        snprintf(detail, size, "the memfd could not be opened again");
        return 1;
    }
    for (i = 0; i < 3; i++) {
        base = mmap(0, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        p = q = MAP_FAILED;
        if (base != MAP_FAILED) {
            p = mmap(base + pg, (size_t)pg, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
            q = mmap(base + 2 * pg, (size_t)pg, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                     i == 2 ? other : fd, i == 1 ? 2 * pg : pg);
        }
        got = p == MAP_FAILED || q == MAP_FAILED
                  ? -2
                  : count_regions((unsigned long)base, (unsigned long)base + span, NULL);
        if (base != MAP_FAILED) {
            munmap(base, span);
        }
        if (got != linux_regions[i]) {
            close(fd);
            close(other);
            if (got == -2) {
                snprintf(detail, size, "a memfd page could not be mapped for %s", cases[i]);
            } else {
                snprintf(detail, size, "%d regions for memfd pages of %s, where Linux has %d", got,
                         cases[i], linux_regions[i]);
            }
            return 1;
        }
    }
    close(fd);
    close(other);

    return 0;
}

/**
 * Run the third half's calls and check the regions after each against
 * Linux's. Returns 0, or 1 with what differed in `detail`.
 */
static int check_merges(char *detail, size_t size)
{
    static const char *const steps[] = {
        "the pages mapped one at a time", "a page made read-only",
        "the page made writable again",   "a page unmapped",
        "the page mapped again",          "the range shrunk by mremap",
        "the range grown back by mremap"};
    static const int linux_regions[] = {3, 5, 3, 4, 3, 3, 3};
    long pg = sysconf(_SC_PAGESIZE);
    size_t span = (size_t)(MERGE_PAGES + 2) * (size_t)pg;
    char *base, *mid, *p;
    unsigned long lo;
    long brk0;
    int step, got, i;

    base = mmap(0, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        snprintf(detail, size, "the range of %d pages could not be reserved", MERGE_PAGES);
        return 1;
    }
    lo = (unsigned long)base;
    mid = base + (1 + MERGE_PAGES / 2) * pg;
    for (step = 0; step < (int)(sizeof(steps) / sizeof(steps[0])); step++) {
        p = base;
        switch (step) {
        case 0:
            for (i = 1; i <= MERGE_PAGES && p != MAP_FAILED; i++) {
                p = mmap(base + i * pg, (size_t)pg, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                if (p != MAP_FAILED) {
                    p[0] = (char)i;
                }
            }
            break;
        case 1:
            p = mprotect(mid, (size_t)pg, PROT_READ) == 0 ? base : MAP_FAILED;
            break;
        case 2:
            p = mprotect(mid, (size_t)pg, PROT_READ | PROT_WRITE) == 0 ? base : MAP_FAILED;
            break;
        case 3:
            p = munmap(mid, (size_t)pg) == 0 ? base : MAP_FAILED;
            break;
        case 4:
            p = mmap(mid, (size_t)pg, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (p != MAP_FAILED) {
                p[0] = 1;
            }
            break;
        case 5:
            p = mremap(base + pg, (size_t)MERGE_PAGES * (size_t)pg,
                       (size_t)(MERGE_PAGES - 1) * (size_t)pg, 0);
            break;
        default:
            p = mremap(base + pg, (size_t)(MERGE_PAGES - 1) * (size_t)pg,
                       (size_t)MERGE_PAGES * (size_t)pg, 0);
            break;
        }
        if (p == MAP_FAILED) {
            snprintf(detail, size, "the call failed at %s", steps[step]);
            munmap(base, span);
            return 1;
        }
        got = count_regions(lo, lo + span, NULL);
        if (got != linux_regions[step]) {
            snprintf(detail, size, "%d regions after %s, where Linux has %d", got, steps[step],
                     linux_regions[step]);
            munmap(base, span);
            return 1;
        }
    }
    munmap(base, span);
    if (check_file_merges(detail, size) != 0) {
        return 1;
    }

    brk0 = syscall(SYS_brk, 0);
    for (i = 1; i <= 3; i++) {
        if (syscall(SYS_brk, brk0 + i * pg) != brk0 + i * pg) {
            snprintf(detail, size, "brk could not grow the heap by %d pages", i);
            return 1;
        }
    }
    got = count_regions(0, 0, "[heap]");
    syscall(SYS_brk, brk0);
    if (got != 1) {
        snprintf(detail, size, "%d [heap] regions after three brk calls, where Linux has 1", got);
        return 1;
    }

    return 0;
}

/** Report one failed check, which fails the step under way and ends the
 * report, and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("VMAMUSL FAIL (%s)\n", what);
    report_stop(what);

    return 1;
}

int main(void)
{
    char *maps[WANTED];
    int taken = 0;
    int i;

    report_begin(report_case(environ, "guest.default.vmamusl"));
    report_plan("mappings");
    report_plan("readback");
    report_plan("unmap");
    report_plan("small-blocks");
    report_plan("merges");

    for (i = 0; i < WANTED; i++) {
        maps[i] = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (maps[i] == MAP_FAILED) {
            break;
        }
        maps[i][0] = (char)(i & 0xff);
        maps[i][1] = (char)((i >> 8) & 0xff);
        maps[i][4095] = (char)(0xff - (i & 0xff));
        taken++;
    }

    if (taken < WANTED) {
        char detail[80];

        snprintf(detail, sizeof(detail), "only %d of %d mappings were granted", taken, WANTED);
        for (i = 0; i < taken; i++) {
            munmap(maps[i], 4096);
        }

        return fail(detail);
    }
    report_held();

    for (i = 0; i < taken; i++) {
        if (maps[i][0] != (char)(i & 0xff) || maps[i][1] != (char)((i >> 8) & 0xff) ||
            maps[i][4095] != (char)(0xff - (i & 0xff))) {
            return fail("a mapping does not hold what was written into it");
        }
    }
    report_held();
    for (i = 0; i < taken; i++) {
        if (munmap(maps[i], 4096) != 0) {
            return fail("a mapping could not be given back");
        }
    }
    report_held();

    {
        void *head = 0;
        unsigned long total = 0;

        for (;;) {
            void **block = malloc(SMALL_BLOCK);

            if (!block) {
                break;
            }
            block[0] = head;
            head = block;
            total += SMALL_BLOCK;
            if (total >= SMALL_FLOOR) {
                break;
            }
        }
        while (head) {
            void **block = head;
            head = block[0];
            free(block);
        }
        if (total < SMALL_FLOOR) {
            char detail[96];

            snprintf(detail, sizeof(detail), "small allocations stopped at %lu bytes, short of %lu",
                     total, (unsigned long)SMALL_FLOOR);

            return fail(detail);
        }
    }
    report_held();
    {
        char detail[96];

        if (check_merges(detail, sizeof(detail)) != 0) {
            return fail(detail);
        }
    }
    report_held();
    report_end();

    printf("VMAMUSL OK\n");

    return 0;
}
