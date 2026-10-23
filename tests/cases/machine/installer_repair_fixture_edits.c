/* installer_repair_fixture_edits.c: exact FAT and ext2 stopped-state edits. */
#define _POSIX_C_SOURCE 200809L
#include "installer_repair_fixture.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int file_equals(const char *path, const char *text)
{
    FILE *in = fopen(path, "rb");
    size_t n = strlen(text);
    char bytes[100];
    int ok;
    if (!in || n > sizeof(bytes)) {
        if (in)
            fclose(in);
        return 0;
    }
    ok = fread(bytes, 1, n, in) == n && !memcmp(bytes, text, n) && fgetc(in) == EOF && !ferror(in);
    return !fclose(in) && ok;
}
static int debugfs(struct repair_fixture *f, const char *request, int write)
{
    const char *read_args[] = {"/bin/debugfs", "-R", request, f->disk, NULL};
    const char *write_args[] = {"/bin/debugfs", "-w", "-R", request, f->disk, NULL};
    return ir_run(write ? write_args : read_args);
}
int ir_root_keys(struct repair_fixture *f)
{
    static const char *home[] = {"alice", "empty"};
    char request[8500];
    unsigned i;
    if (!ir_text(f, "image-key", "image-key\n"))
        return 0;
    for (i = 0; i < 2; i++) {
        debugfs(f, "mkdir /srv", 1);
        snprintf(request, sizeof(request), "mkdir /srv/%s", home[i]);
        debugfs(f, request, 1);
        snprintf(request, sizeof(request), "mkdir /srv/%s/.ssh", home[i]);
        debugfs(f, request, 1);
        snprintf(request, sizeof(request), "write %s /srv/%s/.ssh/authorized_keys",
                 ir_path(f, "image-key"), home[i]);
        if (!debugfs(f, request, 1))
            return 0;
        unlink(ir_path(f, "verified-key"));
        snprintf(request, sizeof(request), "dump /srv/%s/.ssh/authorized_keys %s", home[i],
                 ir_path(f, "verified-key"));
        if (!debugfs(f, request, 0) || !file_equals(ir_path(f, "verified-key"), "image-key\n"))
            return 0;
    }
    return 1;
}
static int marker_files(struct repair_fixture *f)
{
    char text[4096];
    if (snprintf(text, sizeof(text), "%s\n# orphan-uid-2999\n", f->key) >= (int)sizeof(text) ||
        !ir_text(f, "orphan-key", text))
        return 0;
    if (snprintf(text, sizeof(text), "%s stopped\n", f->name) >= (int)sizeof(text))
        return 0;
    return ir_text(f, "update-in-progress", text);
}
static int unfinished_slot(struct repair_fixture *f)
{
    static const char *account[] = {"passwd", "group", "shadow"};
    char source[4096], target[100];
    unsigned i;
    const char *directory[] = {"::rach2-config-a", "::rach2-config-a/etc", NULL};
    const char *mark[] = {ir_path(f, "update-in-progress"), "::rach2-config-a/update-in-progress",
                          NULL};
    if (!ir_mtools(f, "mmd", directory))
        return 0;
    for (i = 0; i < 3; i++) {
        const char *args[] = {source, target, NULL};
        if (snprintf(source, sizeof(source), "%s/saved-archive/etc/%s", f->work, account[i]) >=
            (int)sizeof(source))
            return 0;
        snprintf(target, sizeof(target), "::rach2-config-a/etc/%s", account[i]);
        if (!ir_mtools(f, "mcopy", args))
            return 0;
    }
    return ir_mtools(f, "mcopy", mark);
}
static int legacy_archive(struct repair_fixture *f)
{
    const char *read[] = {"-s", "::rach2-config-a/*", ir_path(f, "saved-archive/"), NULL};
    const char *remove_a[] = {"::rach2-config-a", NULL};
    const char *remove_b[] = {"::rach2-config-b", NULL};
    const char *make_old[] = {"::rach2-config", NULL};
    if (mkdir(ir_path(f, "saved-archive"), 0700) || !ir_mtools(f, "mcopy", read) ||
        unlink(ir_path(f, "saved-archive/generation")) || !ir_mtools(f, "mdeltree", remove_a))
        return 0;
    ir_mtools(f, "mdeltree", remove_b);
    return ir_mtools(f, "mmd", make_old) &&
           ir_copy_contents(f, ir_path(f, "saved-archive"), "::rach2-config/");
}
int ir_legacy(struct repair_fixture *f)
{
    const char *read[] = {"-n", "::rach2-config/etc/passwd", ir_path(f, "archived-passwd"), NULL};
    const char *make_keys[] = {"-D", "s", "::rach2-config/keys", NULL};
    const char *make_orphan[] = {"::rach2-config/keys/2999", NULL};
    const char *key[] = {ir_path(f, "orphan-key"), "::rach2-config/keys/2999/authorized_keys",
                         NULL};
    const char *passwd[] = {"-o", ir_path(f, "archived-passwd"), "::rach2-config/etc/passwd", NULL};
    const char *mark[] = {"-o", ir_path(f, "update-in-progress"),
                          "::rach2-config/update-in-progress", NULL};
    FILE *out;
    int ok;
    if (!marker_files(f) || !legacy_archive(f) || !ir_mtools(f, "mcopy", read))
        return 0;
    out = fopen(ir_path(f, "archived-passwd"), "a");
    if (!out)
        return 0;
    ok = fputs("extra:x:1001:1001::/home/extra:/bin/sh\n", out) >= 0;
    ok = !fclose(out) && ok;
    ir_mtools(f, "mmd", make_keys);
    return ok && ir_mtools(f, "mmd", make_orphan) && ir_mtools(f, "mcopy", key) &&
           ir_mtools(f, "mcopy", passwd) && ir_mtools(f, "mcopy", mark) && unfinished_slot(f);
}
int ir_slot(struct repair_fixture *f)
{
    const char *make_keys[] = {"-D", "s", "::rach2-config-a/keys", NULL};
    const char *make_orphan[] = {"::rach2-config-a/keys/2999", NULL};
    const char *key[] = {ir_path(f, "orphan-key"), "::rach2-config-a/keys/2999/authorized_keys",
                         NULL};
    const char *mark[] = {"-o", ir_path(f, "update-in-progress"),
                          "::rach2-config-a/update-in-progress", NULL};
    if (!marker_files(f))
        return 0;
    ir_mtools(f, "mmd", make_keys);
    return ir_mtools(f, "mmd", make_orphan) && ir_mtools(f, "mcopy", key) &&
           ir_mtools(f, "mcopy", mark);
}
static int keep_sector(struct repair_fixture *f)
{
    unsigned char sector[512];
    int fd = open(f->disk, O_RDONLY), ok;
    FILE *out;
    if (fd < 0)
        return 0;
    ok =
        lseek(fd, (off_t)(f->boot * 512), SEEK_SET) >= 0 && read(fd, sector, sizeof(sector)) == 512;
    close(fd);
    if (!ok)
        return 0;
    out = fopen(ir_path(f, "boot-keep/sector"), "wb");
    if (!out)
        return 0;
    ok = fwrite(sector, 1, sizeof(sector), out) == sizeof(sector);
    return !fclose(out) && ok;
}
static int zero_boot(struct repair_fixture *f)
{
    unsigned char zero[512];
    int fd = open(f->disk, O_RDWR), ok = 1;
    unsigned i;
    if (fd < 0)
        return 0;
    memset(zero, 0, sizeof(zero));
    if (lseek(fd, (off_t)(f->boot * 512), SEEK_SET) < 0)
        ok = 0;
    for (i = 0; ok && i < 64; i++)
        if (write(fd, zero, sizeof(zero)) != (ssize_t)sizeof(zero))
            ok = 0;
    return !close(fd) && ok;
}
int ir_keep(struct repair_fixture *f)
{
    const char *read[] = {"-s", "-n", "::*", ir_path(f, "boot-keep/files/"), NULL};
    const char *keep[] = {"-s", ir_path(f, "boot-keep"), "::/rach2-boot-keep", NULL};
    char recovery[4200];
    snprintf(recovery, sizeof(recovery), "%s", f->volume);
    if (mkdir(ir_path(f, "boot-keep"), 0700) || mkdir(ir_path(f, "boot-keep/files"), 0700) ||
        !keep_sector(f) || !ir_text(f, "boot-keep/complete", "whole\n"))
        return 0;
    snprintf(f->volume, sizeof(f->volume), "%s@@%llu", f->disk, (unsigned long long)f->boot * 512);
    if (!ir_mtools(f, "mcopy", read))
        return 0;
    strcpy(f->volume, recovery);
    return ir_mtools(f, "mcopy", keep) && zero_boot(f);
}
