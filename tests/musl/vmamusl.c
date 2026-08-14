/**
 * How many separate mappings one process may hold, which is a limit a program
 * meets long before it runs out of memory.
 *
 * A process's address space here is a fixed array of regions (`VMA_MAX` in
 * rach2/sys/mm/vm.h). Four are spoken for before it runs a line: its code, its
 * data, the heap and the stack. Every anonymous `mmap` after that takes one
 * more, and a C library's allocator reaches for `mmap` once an allocation is
 * large enough, so the array size is the number of large allocations a program
 * may hold AT ONCE.
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
 * the regions overlapped rather than being distinct.
 *
 * Prints "VMAMUSL OK" on success and "VMAMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* More than the twelve the old table left usable, and comfortably under the
 * sixty the new one does: a probe that asked for the exact maximum would be a
 * probe that fails the day somebody adds a region for a good reason. */
#define WANTED 24

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
        /* Write a byte that says which mapping this is, so a later read can
         * tell them apart rather than merely find memory. */
        maps[i][0] = (char)('A' + i);
        maps[i][4095] = (char)('a' + i);
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
        if (maps[i][0] != (char)('A' + i) || maps[i][4095] != (char)('a' + i)) {
            return fail("a mapping does not hold what was written into it");
        }
    }
    for (i = 0; i < taken; i++) {
        if (munmap(maps[i], 4096) != 0) {
            return fail("a mapping could not be given back");
        }
    }

    printf("VMAMUSL OK\n");

    return 0;
}
