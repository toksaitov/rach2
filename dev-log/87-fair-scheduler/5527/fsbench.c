/* fsbench.c DIR [N]: the file calls a fresh browser profile makes most, timed
 * in DIR on the monotonic clock, for 5527's comparison of Rach2 and Linux:
 * N rounds of a SQLite rollback-journal commit (create the journal, write
 * and fsync it, write and fsync the database, unlink the journal), N
 * atomic replaces (write a temporary, fsync it, rename it over the target),
 * and N stats of a missing name. Build static: cc -O2 -static -o fsbench
 * fsbench.c */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void report(const char *what, int n, double t)
{
    printf("fsbench: %-26s %6d in %7.3f s, %9.1f us each\n", what, n, t, t * 1e6 / n);
}

int main(int argc, char **argv)
{
    char db[4096], jr[4096], tmp[4096], tgt[4096], miss[4096], buf[32768];
    int n = argc > 2 ? atoi(argv[2]) : 500, i, fd, dbfd;
    double t;
    struct stat st;

    if (argc < 2) {
        fprintf(stderr, "usage: fsbench DIR [N]\n");
        return 2;
    }
    memset(buf, 'x', sizeof buf);
    snprintf(db, sizeof db, "%s/bench.sqlite", argv[1]);
    snprintf(jr, sizeof jr, "%s/bench.sqlite-journal", argv[1]);
    snprintf(tmp, sizeof tmp, "%s/prefs.js.tmp", argv[1]);
    snprintf(tgt, sizeof tgt, "%s/prefs.js", argv[1]);
    snprintf(miss, sizeof miss, "%s/missing", argv[1]);
    dbfd = open(db, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (dbfd < 0 || write(dbfd, buf, sizeof buf) != (ssize_t)sizeof buf) {
        perror("fsbench: db");
        return 1;
    }
    fsync(dbfd);

    t = now();
    for (i = 0; i < n; i++) {
        fd = open(jr, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || pwrite(fd, buf, 8192, 0) != 8192) {
            perror("fsbench: journal");
            return 1;
        }
        fsync(fd);
        if (pwrite(dbfd, buf, 4096, (off_t)(i % 8) * 4096) != 4096) {
            perror("fsbench: database");
            return 1;
        }
        fdatasync(dbfd);
        close(fd);
        unlink(jr);
    }
    report("journal commit", n, now() - t);

    t = now();
    for (i = 0; i < n; i++) {
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write(fd, buf, 6000) != 6000) {
            perror("fsbench: replace");
            return 1;
        }
        fsync(fd);
        close(fd);
        rename(tmp, tgt);
    }
    report("write, fsync, rename", n, now() - t);

    t = now();
    for (i = 0; i < n * 20; i++) {
        stat(miss, &st);
    }
    report("stat of a missing name", n * 20, now() - t);
    close(dbfd);
    unlink(db);
    unlink(tgt);
    return 0;
}
