/* Probe for task 80-6: what getrusage costs a large process, and whether
 * ru_maxrss and VmHWM keep the peak after the memory is unmapped. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>

#ifndef BYTES
#define BYTES (512l << 20)
#endif
#define CALLS 20000

/** Print the lines of /proc/self/status that begin with `key`. */
static void status(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];

    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            printf("maxrss: %s", line);
        }
    }
    if (f) {
        fclose(f);
    }
}

int main(void)
{
    struct rusage ru;
    struct timespec a, b;
    long before, peak, after, ns;
    char *p;
    int i;

    getrusage(RUSAGE_SELF, &ru);
    before = ru.ru_maxrss;
    p = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("maxrss: mmap");
        return 1;
    }
    memset(p, 1, BYTES);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < CALLS; i++) {
        getrusage(RUSAGE_THREAD, &ru);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    ns = (b.tv_sec - a.tv_sec) * 1000000000l + (b.tv_nsec - a.tv_nsec);
    peak = ru.ru_maxrss;
    status("VmHWM");
    status("VmRSS");
    munmap(p, BYTES);
    getrusage(RUSAGE_SELF, &ru);
    after = ru.ru_maxrss;
    status("VmHWM");
    status("VmRSS");
    printf("maxrss: before %ld kB, with %ld MiB touched %ld kB, after munmap %ld kB\n", before,
           BYTES >> 20, peak, after);
    printf("maxrss: %d getrusage calls took %ld ns each\n", CALLS, ns / CALLS);
    if (peak < (BYTES >> 10) || after < peak) {
        printf("maxrss: result: fail: the peak was not kept\n");
        return 1;
    }
    printf("maxrss: result: pass\n");
    return 0;
}
