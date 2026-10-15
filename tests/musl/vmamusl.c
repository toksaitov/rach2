/**
 * vmamusl: check that a process may hold hundreds of mappings at once.
 *
 * A process's regions are an array in the kernel heap that starts with
 * `VMA_FIRST` (16) entries and doubles as it fills, up to `VMA_LIMIT`
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
 * Prints `VMAMUSL OK` and exits 0 on success, and prints
 * `VMAMUSL FAIL (detail)` and exits 1 otherwise.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* The pages the first half maps: past the 16 entries the array starts with,
 * so the array must grow, and far under `VMA_LIMIT`, so a region the kernel
 * adds to every process does not make the probe fail. */
#define WANTED 300

/* The second half's block size, and the total it must reach before malloc
 * fails. */
#define SMALL_BLOCK 1024
#define SMALL_FLOOR (8 * 1024 * 1024)

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("VMAMUSL FAIL (%s)\n", what);

    return 1;
}

int main(void)
{
    char *maps[WANTED];
    int taken = 0;
    int i;

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

    for (i = 0; i < taken; i++) {
        if (maps[i][0] != (char)(i & 0xff) || maps[i][1] != (char)((i >> 8) & 0xff) ||
            maps[i][4095] != (char)(0xff - (i & 0xff))) {
            return fail("a mapping does not hold what was written into it");
        }
    }
    for (i = 0; i < taken; i++) {
        if (munmap(maps[i], 4096) != 0) {
            return fail("a mapping could not be given back");
        }
    }

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

    printf("VMAMUSL OK\n");

    return 0;
}
