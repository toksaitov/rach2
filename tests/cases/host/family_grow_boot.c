/* family_grow_boot.c: FAT boot repair, including a torn environment write. */
#define _POSIX_C_SOURCE 200809L
#include "family_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
static int extent(const char *source, uint64_t from, const char *target, uint64_t to, size_t bytes)
{
    unsigned char data[65536];
    int in = open(source, O_RDONLY), out = open(target, O_CREAT | O_WRONLY, 0600), ok = 1;
    if (in < 0 || out < 0)
        ok = 0;
    while (ok && bytes) {
        size_t n = bytes < sizeof(data) ? bytes : sizeof(data);
        if (pread(in, data, n, (off_t)from) != (ssize_t)n ||
            pwrite(out, data, n, (off_t)to) != (ssize_t)n)
            ok = 0;
        bytes -= n;
        from += n;
        to += n;
    }
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    return ok;
}
static int prepared(struct fixture *f, uint64_t *offset)
{
    char *text, value[64];
    unsigned long long sector = 0;
    int ok = family_eval(f, "prepare_boot");
    text = fx_read(family_path(f, "prepared-boot.status"));
    ok &= sscanf(text, "growboot: environment: %llu", &sector) == 1;
    free(text);
    if (!ok || !sector || sector > 131070)
        return 0;
    *offset = sector * 512;
    snprintf(value, sizeof(value), "%llu", sector);
    fx_env("env_sector", value);
    snprintf(value, sizeof(value), "%llu", (unsigned long long)*offset);
    fx_env("env_offset", value);
    return 1;
}
static int initial(struct fixture *f, int cluster8, uint64_t *offset)
{
    remove(family_path(f, "recovery.env"));
    const char *a[] = {"/host-products/mkfatfixture", family_path(f, "boot.original"),
                       cluster8 ? "--cluster8" : NULL, NULL};
    int fd;
    unsigned char good[1024];
    const char *header =
        "# GRUB Environment Block\n# preserve this comment\nsaved_entry=recovery\n";
    memset(good, '#', sizeof(good));
    memcpy(good, header, strlen(header));
    if (!family_run(f, a, f->work, NULL, NULL))
        return 0;
    fd = open(family_path(f, "boot.original"), O_WRONLY);
    if (fd < 0)
        return 0;
    if (ftruncate(fd, 67108864)) {
        close(fd);
        return 0;
    }
    close(fd);
    return fx_copy(family_path(f, "boot.original"), family_path(f, "saved/boot.img")) &&
           prepared(f, offset) && family_sparse(family_path(f, "disk"), 68157440) &&
           extent(family_path(f, "boot.original"), 0, family_path(f, "disk"), 1048576, 67108864) &&
           fx_write(family_path(f, "good-env"), good, sizeof(good), 0600) &&
           extent(family_path(f, "good-env"), 0, family_path(f, "disk"), 1048576 + *offset, 1024) &&
           fx_text(family_path(f, "writes"), "");
}
static int geometry(struct fixture *f, int cluster8, int held[7])
{
    uint64_t offset;
    char writes[160];
    unsigned char zeros[1024];
    const char *own[] = {"/host-products/growboot", family_path(f, "saved/boot.img"),
                         family_path(f, "saved/boot.img"), NULL};
    if (!initial(f, cluster8, &offset))
        return 0;
    held[0] &= family_eval(f, "keep_recovery") && fx_empty(family_path(f, "writes"));
    memset(zeros, 0, sizeof(zeros));
    if (!fx_write(family_path(f, "zero-env"), zeros, sizeof(zeros), 0600) ||
        !extent(family_path(f, "zero-env"), 0, family_path(f, "disk"), 1048576 + offset, 1024))
        return 0;
    family_eval(f, "bootctl --target \"${disk}p1\" env saved_entry recovery");
    held[1] &=
        f->status != 0 && strstr(f->err, "the environment block header is not GRUB's") != NULL;
    remove(family_path(f, "recovery.env"));
    if (!prepared(f, &offset))
        return 0;
    fx_env("GROW_BOOT_TEAR", "1");
    family_eval(f, "keep_recovery");
    snprintf(writes, sizeof(writes), "%llu 512\n", (unsigned long long)(1048576 + offset));
    held[2] &= f->status != 0 && strstr(f->err, "repairing boot environment") &&
               fx_equal(family_path(f, "writes"), writes);
    fx_env("GROW_BOOT_TEAR", NULL);
    remove(family_path(f, "recovery.env"));
    if (!prepared(f, &offset))
        return 0;
    held[3] &= family_eval(f, "keep_recovery");
    snprintf(writes, sizeof(writes), "%llu 512\n%llu 1024\n",
             (unsigned long long)(1048576 + offset), (unsigned long long)(1048576 + offset));
    held[3] &= fx_equal(family_path(f, "writes"), writes) &&
               fx_copy(family_path(f, "boot.original"), family_path(f, "expected-boot.img")) &&
               extent(family_path(f, "recovery.env"), 0, family_path(f, "expected-boot.img"),
                      offset, 1024) &&
               fx_same(family_path(f, "expected-boot.img"), family_path(f, "current-boot.img"));
    fx_text(family_path(f, "writes"), "");
    held[4] &= family_eval(f, "keep_recovery") && fx_empty(family_path(f, "writes"));
    if (!extent(family_path(f, "boot.original"), offset, family_path(f, "disk"), 1048576 + offset,
                1024))
        return 0;
    snprintf(writes, sizeof(writes), "%llu 1024\n", (unsigned long long)(1048576 + offset));
    held[5] &= family_eval(f, "keep_recovery") && fx_equal(family_path(f, "writes"), writes);
    family_run(f, own, f->work, NULL, NULL);
    held[6] &= f->status != 0 &&
               fx_same(family_path(f, "boot.original"), family_path(f, "saved/boot.img"));
    return 1;
}
int family_grow_boot(struct fixture *f)
{
    static const char *ids[] = {"unchanged",       "unparseable", "torn",     "repaired",
                                "retry-unchanged", "candidate",   "own-input"};
    int held[7] = {1, 1, 1, 1, 1, 1, 1}, ok;
    unsigned i;
    fx_env("work", f->work);
    fx_env("saved", family_path(f, "saved"));
    fx_env("disk", family_path(f, "disk"));
    if (!fx_mkdir(getenv("saved")) || !fx_mock(f, "reflash") || !fx_mock(f, "bootctl") ||
        !family_extract(f, "|hash_slice|prepare_boot|keep_recovery|"))
        return 0;
    ok = geometry(f, 1, held);
    ok &= geometry(f, 0, held);
    for (i = 0; i < 7; i++)
        family_row(f, ids[i], ok && held[i]);
    return 1;
}
