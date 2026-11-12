/* rcat.c: read stdin to its end as cat does, with a 128 KiB buffer, and
 * print the reads by size and the voluntary context switches. */
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>
static char buf[131072];
int main(void)
{
    long n, reads = 0, h[8] = {0};
    long long bytes = 0;
    struct rusage ru;
    int i;

    while ((n = read(0, buf, sizeof buf)) > 0) {
        reads++;
        bytes += n;
        i = (int)((n - 1) / 10240);
        h[i > 7 ? 7 : i]++;
    }
    getrusage(RUSAGE_SELF, &ru);
    printf("rcat: reads %ld bytes %lld nvcsw %ld nivcsw %ld by 10k:", reads, bytes, ru.ru_nvcsw, ru.ru_nivcsw);
    for (i = 0; i < 8; i++) printf(" %ld", h[i]);
    printf("\n");
    return 0;
}
