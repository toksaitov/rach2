/* host_image_collection.c: the native image-helper's transfers and statuses. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include "host_image.h"

/** Remove only the private fixture tree this controller created. */
int image_remove_tree(const char *path)
{
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    char child[8192];
    int ok = 1;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    dir = opendir(path);
    if (dir == NULL)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
            !image_remove_tree(child))
            ok = 0;
    }
    closedir(dir);
    return rmdir(path) == 0 && ok;
}

static int bytes(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (f == NULL)
        return 0;
    ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

static int sparse(const char *path, long size)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (f == NULL)
        return 0;
    ok = fseek(f, size - 1, SEEK_SET) == 0 && fputc(0, f) == 0;
    return fclose(f) == 0 && ok;
}

static int run(struct image_check *t, char *const argv[], struct net_run *r)
{
    char output[128];
    snprintf(output, sizeof(output), "collection-command-%u", ++t->commands);
    return net_host(&t->u, argv, output, r) == ETUDE_OK;
}

static void fat(struct image_check *t, const char *disk, const char *directory)
{
    char offset[4400], source[4400], returned[4400];
    char *text;
    struct net_run r = {0};
    char *format[] = {"mformat", "-i", offset, "-T", "32768", "::", NULL};
    char *put[] = {"mcopy", "-i", offset, source, "::/", NULL};
    char *read[] = {"mtype", "-i", offset, "::/Busy Box", NULL};
    char *get[] = {"mcopy", "-i", offset, "::/Busy Box", returned, NULL};
    int ok;
    snprintf(offset, sizeof(offset), "%s@@1048576", disk);
    snprintf(source, sizeof(source), "%s/Busy Box", directory);
    snprintf(returned, sizeof(returned), "%s/returned bytes", directory);
    ok = bytes(source, "input bytes", 11) && run(t, format, &r) && net_exited0(&r);
    net_run_free(&r);
    ok = ok && run(t, put, &r) && net_exited0(&r);
    net_run_free(&r);
    ok = ok && run(t, read, &r) && net_exited0(&r) && r.out && strcmp(r.out, "input bytes") == 0;
    net_run_free(&r);
    image_row(t, "offset and space-containing source basename and guest filename", ok);
    ok = run(t, get, &r) && net_exited0(&r);
    net_run_free(&r);
    text = net_file(returned);
    image_row(t, "copy-out preserves destination bytes",
              ok && text && strcmp(text, "input bytes") == 0);
    free(text);
}

static void failures(struct image_check *t, const char *disk, const char *directory)
{
    char destination[4400];
    char *text;
    struct stat st;
    struct net_run r = {0};
    char *partial[] = {"image-channel-fixture", "-i", (char *)disk, "::/file", destination, NULL};
    char *missing[] = {"mcopy", "-i", (char *)disk, "::/absent", destination, NULL};
    int ok;
    snprintf(destination, sizeof(destination), "%s/partial bytes", directory);
    ok = run(t, partial, &r) && r.end.end == ETUDE_END_EXITED && r.end.value == 7 && r.out &&
         strcmp(r.out, "fixture stdout\n") == 0 && r.err && strcmp(r.err, "fixture stderr\n") == 0;
    net_run_free(&r);
    text = net_file(destination);
    image_row(t, "partial output and exact exit 7 with both streams",
              ok && text && strcmp(text, "partial") == 0);
    free(text);
    snprintf(destination, sizeof(destination), "%s/absent", directory);
    ok = run(t, missing, &r) && r.end.end == ETUDE_END_EXITED && r.end.value != 0;
    net_run_free(&r);
    image_row(t, "failed copy-out creates no missing destination",
              ok && stat(destination, &st) != 0);
}

static uint32_t little32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void ext2(struct image_check *t, const char *disk, const char *directory)
{
    char seed[4400], output[4400], command[8192], *text;
    unsigned char super[1024];
    struct net_run r = {0};
    FILE *f;
    int ok;
    char *argv[] = {"mke2fs",          "-F",         "-t",    "ext2", "-b", "1024", "-d",
                    (char *)directory, (char *)disk, "16384", NULL};
    snprintf(seed, sizeof(seed), "%s/seed", directory);
    ok = bytes(seed, "seed bytes", 10) && run(t, argv, &r) && net_exited0(&r);
    net_run_free(&r);
    f = fopen(disk, "rb");
    ok = ok && f && fseek(f, 1024, SEEK_SET) == 0 &&
         fread(super, 1, sizeof(super), f) == sizeof(super);
    if (f)
        fclose(f);
    image_row(t, "mke2fs block count follows image operand",
              ok && little32(super + 4) == 16384 && super[56] == 0x53 && super[57] == 0xef);
    snprintf(output, sizeof(output), "%s/ext2-seed", directory);
    snprintf(command, sizeof(command), "dump /seed %s", output);
    {
        char *read[] = {"debugfs", "-R", command, (char *)disk, NULL};
        ok = run(t, read, &r) && net_exited0(&r);
        net_run_free(&r);
    }
    text = net_file(output);
    image_row(t, "populated ext2 directory crosses the native helper transport",
              ok && text && strcmp(text, "seed bytes") == 0);
    free(text);
}

int image_collection(struct image_check *t)
{
    char disk[4096], directory[4096];
    const char *p = etude_evidence(t->u.c, "collection-private-directory");
    int ok;
    if (!p || strlen(p) >= sizeof(directory))
        return 0;
    strcpy(directory, p);
    if (mkdir(directory, 0700) != 0)
        return 0;
    if (!net_path(&t->u, "collection-private-disk", disk, sizeof(disk))) {
        image_remove_tree(directory);
        return 0;
    }
    ok = sparse(disk, 17 * 1024 * 1024);
    if (ok) {
        fat(t, disk, directory);
        failures(t, disk, directory);
        ext2(t, disk, directory);
    }
    if (!image_remove_tree(directory))
        ok = 0;
    return ok && t->failed == 0;
}
