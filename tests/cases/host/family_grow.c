/* family_grow.c: measured counters, plan validation and memory refusal. */
#define _POSIX_C_SOURCE 200809L
#include "family_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int measure(struct fixture *f)
{
    static const char *phase[] = {"table", "check-readonly", "check-repair", "resize"};
    static const unsigned commands[] = {100, 105, 105, 112, 112, 123, 123, 140};
    static const unsigned flushes[] = {20, 21, 21, 21, 21, 23, 23, 26};
    static const char *names[] = {"valid-sub-tick",  "all-zero",          "wrong-phase",
                                  "missing-witness", "duplicate-witness", "wrong-order"};
    unsigned row, i;
    for (row = 0; row < 6; row++) {
        FILE *fp = fopen(family_path(f, names[row]), "w");
        const char *a[] = {"/usr/bin/awk", "-f",
                           family_source(f, "tests/system/checkers/grow_measure.awk"),
                           family_path(f, names[row]), NULL};
        if (!fp)
            return 0;
        if (row != 3)
            fprintf(fp, "grow-witness: whole before disk-commands %u disk-flushes %u\n",
                    row == 1 ? 0 : 98, row == 1 ? 0 : 20);
        for (i = 0; i < 8; i++) {
            const char *p = row == 5 && i == 2 ? "check-repair" : phase[i / 2];
            unsigned c = row == 1 ? 0 : commands[i], fl = row == 1 ? 0 : flushes[i];
            fprintf(fp,
                    "growroot: measure: %s %s monotonic-us %u disk-commands %u disk-flushes %u\n",
                    p, i % 2 ? "after" : "before", row == 1 ? 0 : 1000000,
                    row == 2 && c == 112 ? 113 : c, fl);
            if (i >= 2)
                fprintf(fp, "grow-witness: %s %s disk-commands %u disk-flushes %u\n", p,
                        i % 2 ? "after" : "before", c, fl);
        }
        fprintf(fp, "grow-witness: whole after disk-commands %u disk-flushes %u\n",
                row == 1 ? 0 : 140, row == 1 ? 0 : 26);
        if (row == 4)
            fputs("grow-witness: whole after disk-commands 140 disk-flushes 26\n", fp);
        fclose(fp);
        family_run(f, a, f->work, NULL, NULL);
        family_row(f, names[row], f->status == (row ? 1 : 0));
    }
    return 1;
}
static int recovery_products(struct fixture *f)
{
    static const char *source[] = {"growboot", "growset", "growroot", "reflash",
                                   "diskctl",  "bootctl", "e2fsck",   "resize2fs"};
    static const char *target[] = {"utils/growboot",          "utils/growset",
                                   "utils/growroot",          "utils/reflash",
                                   "utils/diskctl",           "utils/sbin/bootctl",
                                   "vendor/e2fsprogs/e2fsck", "vendor/e2fsprogs/resize2fs"};
    unsigned i;
    for (i = 0; i < 8; i++) {
        char s[4096], t[4096];
        snprintf(s, sizeof(s), "/host-products/%s", source[i]);
        snprintf(t, sizeof(t), "%s/context/build/%s", f->work, target[i]);
        if (!fx_copy(s, t))
            return 0;
    }
    return fx_copy("/host-products/growset",
                   family_path(f, "context/build/tools/inspect/growset")) &&
           fx_copy(family_source(f, "tools/dev/machine_grow_plan.sh"),
                   family_path(f, "context/tools/dev/machine_grow_plan.sh")) &&
           fx_copy(family_source(f, "tools/dev/machine_grow.sh"),
                   family_path(f, "context/tools/dev/machine_grow.sh")) &&
           fx_copy(family_source(f, "tools/setup/e2fsprogs_path.sh"),
                   family_path(f, "context/tools/setup/e2fsprogs_path.sh")) &&
           fx_copy(family_source(f, "mk/arch.sh"), family_path(f, "context/mk/arch.sh")) &&
           fx_mock(f, "shasum");
}
static int shifted(struct fixture *f, const char *path)
{
    char *text = fx_read(path), *at, *last = text;
    unsigned long long first, length;
    FILE *out;
    if (!text || !*text)
        return 0;
    for (at = text; *at; at++)
        if (*at == '\n' && at[1])
            last = at + 1;
    if (sscanf(last, "%llu %llu", &first, &length) != 2) {
        free(text);
        return 0;
    }
    *last = 0;
    out = fopen(family_path(f, "ranges.txt"), "w");
    if (!out) {
        free(text);
        return 0;
    }
    fputs(text, out);
    fprintf(out, "%llu %llu\n", first + length, length);
    free(text);
    return fclose(out) == 0;
}
static int plan(struct fixture *f, unsigned profile)
{
    static const char *names[] = {"small", "samsung", "samsung-gpt"};
    static const char *ids[] = {"1acef00d", "1acef00d", "1acef00d-0f1e-4d2c-8b3a-c0ffee000001"};
    char directory[4096], row[80], setup[16384], line[4096], hash[65] = "";
    unsigned long long sectors = 0, start = 0, seed = 0, count = 0;
    const char *seedpath = family_path(f, profile ? "samsung/seed.ext2" : "small/seed.ext2");
    const char *a[] = {"/bin/sh",  family_path(f, "context/tools/dev/machine_grow_plan.sh"),
                       seedpath,   profile ? "488397168" : "397312",
                       "GROWTEST", ids[profile],
                       directory,  NULL};
    FILE *fp;
    int ok;
    snprintf(directory, sizeof(directory), "%s/%s.plan", f->work, names[profile]);
    ok = family_run(f, a, f->work, NULL, NULL);
    snprintf(line, sizeof(line), "%s/grow.plan", directory);
    fp = fopen(line, "r");
    if (!fp)
        return 0;
    ok &=
        fscanf(fp, "%llu %*s %llu %llu %llu %*s %64s", &sectors, &start, &seed, &count, hash) == 5;
    fclose(fp);
    snprintf(line, sizeof(line), "%s/ranges.txt", directory);
    ok &= fx_copy(line, family_path(f, "ranges.txt"));
    fx_env("store", family_path(f, profile ? "samsung" : "small"));
    fx_env("work", f->work);
    snprintf(setup, sizeof(setup),
             "start=%llu; seed=%llu; count=%llu; sectors=%llu; seed_hash=%s; table=%s; "
             "table_sectors=%u; check_plan",
             start, seed, count, sectors, hash, profile == 2 ? "gpt" : "mbr",
             profile == 2 ? 40 : 8);
    ok &= family_eval(f, setup);
    snprintf(row, sizeof(row), "plan-%s", names[profile]);
    family_row(f, row, ok);
    ok = shifted(f, line);
    family_eval(f, setup);
    snprintf(row, sizeof(row), "ranges-%s", names[profile]);
    family_row(f, row,
               ok && f->status == 1 &&
                   strstr(f->err, "ranges differ from the seed and target geometry"));
    return 1;
}
static int boot_copy(struct fixture *f)
{
    FILE *fp;
    int ok = fx_mkdir(family_path(f, "saved")) && family_sparse(family_path(f, "disk"), 68157440) &&
             family_sparse(family_path(f, "saved/boot.img"), 67108864);
    const char *names[] = {"disk", "saved/boot.img"};
    unsigned i;
    for (i = 0; i < 2 && ok; i++) {
        fp = fopen(family_path(f, names[i]), "r+b");
        ok = fp && fseeko(fp, i ? 67108863 : 68157439, SEEK_SET) == 0 && fputc('x', fp) != EOF;
        if (fp)
            fclose(fp);
    }
    fx_env("disk", family_path(f, "disk"));
    fx_env("saved", family_path(f, "saved"));
    family_row(f, "boot-copy", ok && family_eval(f, "check_boot_copy"));
    fp = fopen(family_path(f, "saved/boot.img"), "r+b");
    ok = fp && fseeko(fp, 67108863, SEEK_SET) == 0 && fputc('y', fp) != EOF;
    if (fp)
        fclose(fp);
    if (ok) {
        char hash[65], sums[100];
        ok = family_digest(family_path(f, "saved/boot.img"), hash);
        snprintf(sums, sizeof(sums), "%s  boot.img\n", hash);
        ok &= fx_text(family_path(f, "saved/SHA256SUMS"), sums);
    }
    {
        const char *a[] = {"/usr/bin/sha256sum", "-c", "SHA256SUMS", NULL};
        ok &= family_run(f, a, family_path(f, "saved"), NULL, NULL);
    }
    family_eval(f, "check_boot_copy");
    family_row(f, "boot-corrupt",
               ok && f->status == 1 && strstr(f->err, "the boot backup differs from the disk"));
    return 1;
}
static int check(struct fixture *f)
{
    unsigned i;
    fx_env("RACH2_ARCH", "i686");
    if (!fx_mkdir(family_path(f, "small")) || !fx_mkdir(family_path(f, "samsung")) ||
        !family_seed(f, "small/seed.ext2", 8, "1024") ||
        !family_seed(f, "samsung/seed.ext2", 1024, "32768") || !recovery_products(f) ||
        !family_extract(f, "|hash_file|hash_slice|check_plan|check_boot_copy|"))
        return 0;
    for (i = 0; i < 3; i++)
        if (!plan(f, i))
            return 0;
    return boot_copy(f);
}
int family_grow(struct fixture *f, const char *id)
{
    if (!strcmp(id, "machine.grow.measure"))
        return measure(f);
    if (!strcmp(id, "machine.grow.check"))
        return check(f);
    if (!strcmp(id, "machine.grow.boot"))
        return family_grow_boot(f);
    if (!strcmp(id, "machine.grow.space"))
        return family_grow_space(f);
    if (!strcmp(id, "machine.grow.replay"))
        return family_grow_replay(f);
    if (!strcmp(id, "machine.grow.memory")) {
        const char *a[] = {"/host-products/growroot-memory", "MBRID=47470001,1", NULL};
        family_run(f, a, f->work, NULL, NULL);
        family_row(f, "status", f->status == 1);
        family_row(
            f, "refused",
            fx_regex(
                f->out,
                "^growroot: refused: [0-9]* MiB of memory; growing a root needs 1048576 MiB$") ||
                fx_regex(
                    f->err,
                    "^growroot: refused: [0-9]* MiB of memory; growing a root needs 1048576 MiB$"));
        return 1;
    }
    return 0;
}
