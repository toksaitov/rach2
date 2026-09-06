/**
 * How many separate mappings one process may hold, which is a limit a program
 * meets long before it runs out of memory.
 *
 * A process's address space here is an array of regions that grows as it
 * fills, from sixteen to the limit in rach2/sys/mm/vm.h. Four are spoken for
 * before it runs a line: its code, its data, the heap and the stack. Every
 * anonymous `mmap` after that takes one more, and a C library's allocator
 * reaches for `mmap` once an allocation is large enough, so the regions a
 * process may hold is the number of large allocations a program may hold AT
 * ONCE.
 *
 * The compiler is what found the old number too small. Reading musl's 1.9 MB
 * `libc.a`, tcc reported "memory full (malloc)" with physical memory barely
 * touched and the address space almost entirely free: the table was full at
 * twelve usable entries, and malloc's failure said nothing about tables.
 *
 * The probe takes mappings one at a time and writes to each, since a mapping
 * nothing touches proves only that a number was returned. It requires more of
 * them than the old ceiling allowed, and requires the ones it took to still
 * hold what was written after the last was taken, which is what would break if
 * the regions overlapped instead of being distinct.
 *
 * The second half of the probe keeps its blocks in a chain, so nothing is
 * freed until the end and every one is still held when the ceiling is met. It
 * is the case that found the ceiling too low a third time: a program that
 * never calls `mmap` itself, asking the allocator for 1 KB at a time. musl
 * takes a mapping for each block it cannot fit in what it holds, so such a
 * walk stops at the table, not at memory, and it stopped at about 3.1 MB while
 * a single 64 MB request succeeded. What made that hard to see was silence:
 * the kernel refused the mapping and said nothing, so the failure looked like
 * the library's own. The kernel now says which process ran out, on the serial
 * log.
 *
 * Prints "VMAMUSL OK" on success and "VMAMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* More mappings than the 128-entry table could grant, and comfortably under
 * what the 512-entry one does: this is the boundary that regressing the table
 * to 128 fails, without asking for the exact maximum, which would make the
 * probe fail the day somebody adds a region for a good reason. The table has
 * been outgrown three times: 16 died reading the 1.9 MB libc.a and 64 died
 * linking login, both on the machine's own compiler, and 128 was met by an
 * ordinary program asking for small blocks, which is the case below. */
#define WANTED 300

/* The other half of the same limit, and the one a program meets without ever
 * calling mmap: musl carves a mapping for each small block it cannot fit in
 * what it already has, so a walk of 1 KB allocations stops at the table long
 * before it stops at memory. It stopped at about 3.1 MB with a table of 128.
 * Eight MiB is the floor asked for here, which 128 cannot reach and 512
 * passes with room to spare. */
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
        /* Write the mapping's own index, both bytes of it, so a later read
         * tells them apart rather than merely finding memory: one byte would
         * repeat every 256 mappings and let a later one alias an earlier. */
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
