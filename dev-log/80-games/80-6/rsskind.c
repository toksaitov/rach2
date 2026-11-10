/* Probe for task 80-6: how /proc/self/status divides the resident pages
 * among RssAnon, RssFile and RssShmem as a process maps and writes memory
 * of each kind. Usage: rsskind FILE, a scratch file on a disk, removed. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MIB (1l << 20)

static long last[4];

/** Print how the four counts changed since the last step, under `step`.
 * The four are taken from one read of the file, so that they are of one
 * moment: a second read faults in pages of its own the first time. */
static void report(const char *step)
{
    static const char *keys[4] = {"VmRSS:", "RssAnon:", "RssFile:", "RssShmem:"};
    static char text[8192];
    long now[4] = {-1, -1, -1, -1};
    int fd = open("/proc/self/status", O_RDONLY);
    long n = fd < 0 ? 0 : read(fd, text, sizeof(text) - 1);
    int i;

    close(fd);
    text[n > 0 ? n : 0] = 0;
    for (i = 0; i < 4; i++) {
        const char *at = strstr(text, keys[i]);

        if (at) {
            now[i] = atol(at + strlen(keys[i]));
        }
    }
    printf("rsskind: %-34s rss %+7ld anon %+7ld file %+7ld shmem %+7ld kB\n", step,
           now[0] - last[0], now[1] - last[1], now[2] - last[2], now[3] - last[3]);
    memcpy(last, now, sizeof(last));
}

/** Read one byte of each page of the `bytes` at `p`; the sum is returned
 * so that the reads are not removed. */
static long touch(const char *p, long bytes)
{
    long i, sum = 0;

    for (i = 0; i < bytes; i += 4096) {
        sum += p[i];
    }
    return sum;
}

/** Map LEN bytes as mmap(2) does with PROT and FLAGS over FD at OFF, and
 * exit with status 1 when the mapping fails. */
static char *map(long len, int prot, int flags, int fd, long off)
{
    char *p = mmap(NULL, len, prot, flags, fd, off);

    if (p == MAP_FAILED) {
        perror("rsskind: mmap");
        exit(1);
    }
    return p;
}

/** Map and write memory of each kind and print how the four counts change
 * at each step; FILE, argv[1], is the scratch file, unlinked once open.
 * Exit 0 unless a file, a mapping or a write fails. */
int main(int argc, char **argv)
{
    char *anon, *shared, *mem, *priv, *file;
    long sum = 0;
    int fd, mfd;

    if (argc < 2) {
        fprintf(stderr, "rsskind: usage: rsskind FILE\n");
        return 2;
    }
    fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0600);
    mfd = memfd_create("rsskind", 0);
    if (fd < 0 || mfd < 0 || ftruncate(fd, 8 * MIB) != 0 || ftruncate(mfd, 16 * MIB) != 0) {
        perror("rsskind: files");
        return 1;
    }
    unlink(argv[1]);
    anon = malloc(MIB);
    if (anon == NULL) {
        perror("rsskind: malloc");
        return 1;
    }
    memset(anon, 1, MIB);
    if (write(fd, anon, MIB) != MIB || fsync(fd) != 0) {
        perror("rsskind: write");
        return 1;
    }
    report("warm");
    report("start");
    printf("rsskind: at the start: rss %ld anon %ld file %ld shmem %ld kB\n", last[0], last[1],
           last[2], last[3]);

    anon = map(32 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(anon, 1, 32 * MIB);
    report("32 MiB private anonymous written");

    shared = map(8 * MIB, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset(shared, 1, 8 * MIB);
    report("8 MiB shared anonymous written");

    mem = map(16 * MIB, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    memset(mem, 1, 16 * MIB);
    report("16 MiB shared memfd written");

    priv = map(4 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    sum += touch(priv, 4 * MIB);
    report("4 MiB private disk file read");
    memset(priv, 2, MIB);
    report("1 MiB of it written");

    file = map(2 * MIB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4 * MIB);
    sum += touch(file, 2 * MIB);
    report("2 MiB shared disk file read");
    memset(file, 3, MIB);
    report("1 MiB of it written");

    priv = map(4 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE, mfd, 0);
    sum += touch(priv, 4 * MIB);
    report("4 MiB private memfd read");
    memset(priv, 4, MIB);
    report("1 MiB of it written");

    munmap(anon, 32 * MIB);
    report("the private anonymous unmapped");
    munmap(mem, 16 * MIB);
    report("the shared memfd unmapped");
    printf("rsskind: at the end: rss %ld anon %ld file %ld shmem %ld kB, of which %ld\n", last[0],
           last[1], last[2], last[3], last[1] + last[2] + last[3]);
    return sum == 0;
}
