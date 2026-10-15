/**
 * mallocqsort: check demand-paged memory through musl's malloc and qsort.
 *
 * The program fills an array of `N` ints, 16 KiB, with a linear congruential
 * sequence, sorts it with `qsort`, and checks that the result does not
 * decrease. It then allocates 256 KiB, above mallocng's threshold of 131052
 * bytes for an allocation of its own `mmap`, writes a byte pattern across it,
 * and checks the first and last bytes. Both allocations are anonymous
 * mappings whose pages the fault handler backs with zeroed frames on first
 * touch; mallocng takes the pages of its own bookkeeping with `brk`.
 *
 * It prints `MALLOC QSORT OK` on success, or a `MALLOC QSORT FAIL` line that
 * names the failed step. Linked statically against the ported musl (crt1,
 * libc.a and crtn); see tests/musl/build.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Compare the ints at `a` and `b` for `qsort`: negative, zero or positive as
 * the first is less than, equal to or greater than the second.
 */
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

    a = malloc(N * sizeof *a);
    if (!a) {
        printf("MALLOC QSORT FAIL: malloc\n");
        return 1;
    }

    for (i = 0; i < N; i++) {
        seed = seed * 1103515245u + 12345u;
        a[i] = (int)(seed >> 8);
    }

    qsort(a, N, sizeof *a, cmp_int);

    for (i = 1; i < N; i++) {
        if (a[i - 1] > a[i]) {
            ok = 0;
            break;
        }
    }
    lo = a[0];
    hi = a[N - 1];
    free(a);

    {
        size_t big = 256u * 1024u;
        unsigned char *p = malloc(big);

        if (!p) {
            printf("MALLOC QSORT FAIL: big malloc\n");
            return 1;
        }
        memset(p, 0xAB, big);
        if (p[0] != 0xAB || p[big - 1] != 0xAB) {
            printf("MALLOC QSORT FAIL: big buffer\n");
            free(p);
            return 1;
        }
        free(p);
    }

    if (ok) {
        printf("MALLOC QSORT OK (n=%d, lo=%d, hi=%d)\n", N, lo, hi);
        return 0;
    }
    printf("MALLOC QSORT FAIL: not sorted\n");
    return 1;
}
