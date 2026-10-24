/* family_grow_replay.c: restore and independently compare the complete plan. */
#define _POSIX_C_SOURCE 200809L
#include "family_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct range {
    uint64_t first, length;
};
static int ranges(struct fixture *f, struct range out[1856])
{
    const char *a[] = {"/host-products/growset",    "--save",    "--ranges", "--lba", "266240",
                       family_path(f, "seed.ext2"), "488130928", NULL};
    FILE *fp;
    uint64_t total = 0;
    unsigned n = 0;
    int ok = family_seed(f, "seed.ext2", 1024, "32768") && family_run(f, a, f->work, NULL, NULL) &&
             fx_text(family_path(f, "ranges.txt"), f->out);
    remove(family_path(f, "seed.ext2"));
    fp = ok ? fopen(family_path(f, "ranges.txt"), "r") : NULL;
    if (!fp)
        return 0;
    for (;;) {
        unsigned long long first, length;
        int got = fscanf(fp, "%llu %llu", &first, &length);
        if (got == EOF)
            break;
        if (got != 2 || n == 1856) {
            ok = 0;
            break;
        }
        out[n].first = first;
        out[n++].length = length;
        total += length;
    }
    fclose(fp);
    return ok && n == 1856 && total == 5932200;
}
static void pattern(unsigned char *data, size_t bytes, struct range r)
{
    size_t i;
    for (i = 0; i < bytes; i++) {
        uint64_t v = i % 16 < 8 ? r.first : r.length;
        data[i] = (unsigned char)(v >> (8 * (i % 8)));
    }
}
static int fill(struct fixture *f, struct range r[1856])
{
    unsigned char *data = malloc(1048576), *dirty = malloc(1048576);
    FILE *backup = fopen(family_path(f, "backup.bin"), "wb"), *target;
    unsigned i;
    int ok = data && dirty && backup &&
             family_sparse(family_path(f, "target"), UINT64_C(488397168) * 512);
    target = ok ? fopen(family_path(f, "target"), "r+b") : NULL;
    ok &= target != NULL;
    if (dirty)
        memset(dirty, 0xa5, 1048576);
    for (i = 0; ok && i < 1856; i++) {
        uint64_t left = r[i].length * 512;
        pattern(data, 1048576, r[i]);
        ok = fseeko(target, (off_t)(r[i].first * 512), SEEK_SET) == 0;
        while (ok && left) {
            size_t n = left < 1048576 ? (size_t)left : 1048576;
            ok = fwrite(data, 1, n, backup) == n && fwrite(dirty, 1, n, target) == n;
            left -= n;
        }
    }
    if (backup)
        ok &= fclose(backup) == 0;
    if (target)
        ok &= fclose(target) == 0;
    free(data);
    free(dirty);
    return ok;
}
static int compare(struct fixture *f, struct range r[1856])
{
    unsigned char *want = malloc(1048576), *data = malloc(1048576);
    FILE *target = fopen(family_path(f, "target"), "rb");
    unsigned i;
    int ok = want && data && target;
    for (i = 0; ok && i < 1856; i++) {
        uint64_t left = r[i].length * 512;
        pattern(want, 1048576, r[i]);
        ok = fseeko(target, (off_t)(r[i].first * 512), SEEK_SET) == 0;
        while (ok && left) {
            size_t n = left < 1048576 ? (size_t)left : 1048576;
            ok = fread(data, 1, n, target) == n && !memcmp(data, want, n);
            left -= n;
        }
        if (i + 1 < 1856 && r[i].first + r[i].length < r[i + 1].first) {
            uint64_t edge[] = {r[i].first + r[i].length, r[i + 1].first - 1};
            unsigned j;
            memset(want, 0, 512);
            for (j = 0; ok && j < 2; j++)
                ok = fseeko(target, (off_t)(edge[j] * 512), SEEK_SET) == 0 &&
                     fread(data, 1, 512, target) == 512 && !memcmp(data, want, 512);
        }
    }
    if (target)
        fclose(target);
    free(want);
    free(data);
    return ok;
}
int family_grow_replay(struct fixture *f)
{
    struct range r[1856];
    uint64_t offset = 0;
    unsigned i, lines = 0;
    int ok = ranges(f, r) && fill(f, r) && family_extract(f, "|run_logged|restore_range|");
    char *log, *p;
    fx_env("work", f->work);
    fx_env("saved", f->work);
    fx_env("disk", family_path(f, "target"));
    /* The product replays its plan in one shell, with one logged call per range. */
    if (ok)
        ok = family_eval(f,
                         "offset=0; records=0\n"
                         "while read -r first sectors; do\n"
                         "    amount=$((sectors * 512))\n"
                         "    run_logged restore.log restore_range\n"
                         "    offset=$((offset + amount)); records=$((records + 1))\n"
                         "done < ranges.txt\n"
                         "[ \"$records\" = 1856 ] && [ \"$offset\" = 3037286400 ]\n");
    for (i = 0; ok && i < 1856; i++)
        offset += r[i].length * 512;
    log = fx_read(family_path(f, "restore.log"));
    for (p = log; p && *p; p++)
        lines += *p == '\n';
    free(log);
    ok &= offset == UINT64_C(3037286400) && lines == 1856;
    family_row(f, "records", ok);
    family_row(f, "bytes", ok && compare(f, r));
    remove(family_path(f, "target"));
    remove(family_path(f, "backup.bin"));
    return 1;
}
