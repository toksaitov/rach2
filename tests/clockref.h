/** Clock reference intervals and their three possible verdicts. */
#ifndef TESTS_CLOCKREF_INTERVAL_H
#define TESTS_CLOCKREF_INTERVAL_H

#define CLOCKREF_NS          1000000000LL
#define CLOCKREF_UNCERTAINTY 100000000LL

/** Nanoseconds on each clock, with guest reads enclosing the host sample. */
struct clockref_sample {
    long long mono_before, real_before;
    long long mono_after, real_after;
    long long host_mono, host_real;
};

/**
 * Judge an error interval against `bound`: 1 when wholly inside, 0 when
 * wholly outside, and -1 when uncertainty prevents either conclusion.
 */
static int clockref_judge(long long low, long long high, long long bound)
{
    if (low > high)
        return -1;
    if (low >= -bound && high <= bound)
        return 1;
    if (low > bound || high < -bound)
        return 0;
    return -1;
}

#endif
