/* A quiet CPU worker for the runner's descendant activity test. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>

/** Consume six seconds of processor time without writing output. This is a
 * deliberate load interval; deadline bounds its completion on a slow host. */
int main(void)
{
    struct timespec start, current;

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start) != 0)
        return 125;
    do {
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &current) != 0)
            return 125;
    } while ((current.tv_sec - start.tv_sec) + (current.tv_nsec - start.tv_nsec) / 1000000000.0 <
             6.0);
    return 0;
}
