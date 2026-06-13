/*
 * mallocqsort.c - Phase 4 integration milestone: a malloc-using musl program.
 *
 * Where Phase 3's hello.c only proved musl could reach main and use buffered
 * stdio, this proves the demand-paged address space works under a real libc
 * allocator: malloc() pulls heap pages via brk and a large request via mmap2,
 * all backed lazily and zero-filled by the Phase 4 fault handler. It fills an
 * array with an LCG sequence, qsort()s it, checks the order, then makes a
 * 256 KB allocation (the mallocng mmap path) and writes across it. Prints
 * "MALLOC QSORT OK" on success.
 *
 * Linked statically against the ported musl (crt1 + libc.a + crtn); see
 * tests/musl/build.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;

    return (x > y) - (x < y);
}

int main(void)
{
    enum { N = 4096 };
    int *a;
    size_t i;
    unsigned seed = 12345u;
    int ok = 1;
    int lo, hi;

    /* ~16 KB: several demand-paged heap pages via brk. */
    a = malloc(N * sizeof *a);
    if (!a)
    {
        printf("MALLOC QSORT FAIL: malloc\n");
        return 1;
    }

    for (i = 0; i < N; i++)
    {
        seed = seed * 1103515245u + 12345u;
        a[i] = (int)(seed >> 8);
    }

    qsort(a, N, sizeof *a, cmp_int);

    for (i = 1; i < N; i++)
    {
        if (a[i - 1] > a[i])
        {
            ok = 0;
            break;
        }
    }
    lo = a[0];
    hi = a[N - 1];
    free(a);

    /* 256 KB: large enough that mallocng services it straight from mmap, so the
     * anonymous-mapping path is exercised end to end (map, write, free/munmap). */
    {
        size_t big = 256u * 1024u;
        unsigned char *p = malloc(big);

        if (!p)
        {
            printf("MALLOC QSORT FAIL: big malloc\n");
            return 1;
        }
        memset(p, 0xAB, big);
        if (p[0] != 0xAB || p[big - 1] != 0xAB)
        {
            printf("MALLOC QSORT FAIL: big buffer\n");
            free(p);
            return 1;
        }
        free(p);
    }

    if (ok)
    {
        printf("MALLOC QSORT OK (n=%d, lo=%d, hi=%d)\n", N, lo, hi);
        return 0;
    }
    printf("MALLOC QSORT FAIL: not sorted\n");
    return 1;
}
