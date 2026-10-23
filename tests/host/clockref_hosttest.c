/**
 * Host test of the clock reference's judge, `clockref_judge` in
 * tests/clockref.h.
 *
 * The clock cases compare the guest's clocks with a host timestamp taken
 * between two guest reads, which gives an interval of error rather than one
 * value. The judge passes an interval wholly inside the bound, fails one
 * wholly outside it as a clock mismatch, and calls one that crosses either
 * end of the bound uncertain, since the guest's clock may be right or wrong.
 * An interval whose low end is above its high end is uncertain too. The
 * rows cover each verdict, both ends of the bound exactly, and an interval
 * wider than the whole bound.
 */

#include <stdio.h>

#include "../clockref.h"

/** One interval and the verdict the judge must give it. */
static const struct {
    long long low, high, bound;
    int verdict;
    const char *what;
} rows[] = {
    {-10, 10, 30, 1, "an interval inside the bound"},
    {-30, 30, 30, 1, "an interval that ends on both ends of the bound"},
    {40, 60, 30, 0, "an interval above the bound"},
    {-60, -40, 30, 0, "an interval below the bound"},
    {20, 40, 30, -1, "an interval across the upper end"},
    {-40, -20, 30, -1, "an interval across the lower end"},
    {-1000, 1000, 30, -1, "an interval wider than the bound"},
    {5, -5, 30, -1, "an interval whose ends are reversed"},
};

/** Judge each row; the exit status is the count of rows that fail. */
int main(void)
{
    int fails = 0;
    size_t k;

    printf("clockref_hosttest: the clock reference's judge\n");
    for (k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
        int got = clockref_judge(rows[k].low, rows[k].high, rows[k].bound);

        if (got != rows[k].verdict) {
            fprintf(stderr, "  FAIL: %s: verdict %d, not %d\n", rows[k].what, got, rows[k].verdict);
            fails++;
        }
    }
    if (fails == 0) {
        printf("clockref_hosttest: OK\n");
        return 0;
    }
    printf("clockref_hosttest: %d failure(s)\n", fails);
    return fails;
}
