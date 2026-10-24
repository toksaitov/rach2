/* family_export.c: bounded reports beside the retained private fixture disk. */
#define _POSIX_C_SOURCE 200809L
#include "family_check.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void quoted(FILE *fp, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    for (; *p; p++) {
        if (*p < 32 || *p > 126 || *p == '\\')
            fprintf(fp, "\\x%02x", *p);
        else
            fputc(*p, fp);
    }
}

static int valid_id(const char *id)
{
    return *id && strlen(id) <= 128 &&
           strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789_.-") == strlen(id) && strcmp(id, ".") &&
           strcmp(id, "..");
}

static int export_entry(const char *root, const char *id, const char *name, FILE *list,
                        FILE *manifest, size_t *budget)
{
    char path[8192], member[8192];
    struct stat st;
    if (snprintf(member, sizeof(member), "%s/%s", id, name) >= (int)sizeof(member) ||
        snprintf(path, sizeof(path), "%s/%s", root, member) >= (int)sizeof(path) ||
        lstat(path, &st))
        return 0;
    int include = S_ISREG(st.st_mode) && st.st_size >= 0 && st.st_size <= 1048576;
    size_t charge = include ? 4096 + ((size_t)st.st_size + 511) / 512 * 512 : 0;
    include &= charge <= *budget;
    if (include) {
        *budget -= charge;
        fwrite(member, 1, strlen(member) + 1, list);
    }
    fprintf(manifest, "%s %o %lld ", include ? "archive" : "disk-only", (unsigned)st.st_mode,
            (long long)st.st_size);
    quoted(manifest, name);
    fputc('\n', manifest);
    return 1;
}

int family_export(const char *root, const char *id)
{
    char path[8192];
    FILE *list = NULL, *manifest = NULL;
    DIR *dir = NULL;
    struct dirent *entry;
    size_t budget = 7 * 1048576;
    unsigned count = 0;
    int ok = 0;
    if (!valid_id(id) || snprintf(path, sizeof(path), "%s/%s", root, id) >= (int)sizeof(path) ||
        !(dir = opendir(path)))
        goto done;
    if (snprintf(path, sizeof(path), "%s/family-export.list", root) >= (int)sizeof(path) ||
        !(list = fopen(path, "wb")))
        goto done;
    if (snprintf(path, sizeof(path), "%s/family-export.txt", root) >= (int)sizeof(path) ||
        !(manifest = fopen(path, "wb")))
        goto done;
    fputs("Complete selected files are in this archive. All other files, including\n"
          "directory contents, remain on the retained private products disk.\n"
          "The inventory below describes the fixture directory's direct entries.\n"
          "Storage, mode, bytes, name:\n",
          manifest);
    fwrite("family-export.txt", 1, sizeof("family-export.txt"), list);
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno)
                goto done;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (++count > 4096 || ftell(manifest) > 250000) {
            fputs("Remaining entries are on the private disk; inventory limit reached.\n",
                  manifest);
            break;
        }
        if (!export_entry(root, id, entry->d_name, list, manifest, &budget))
            goto done;
    }
    ok = !ferror(list) && !ferror(manifest);
done:
    if (dir)
        ok &= closedir(dir) == 0;
    if (list)
        ok &= fclose(list) == 0;
    if (manifest)
        ok &= fclose(manifest) == 0;
    return ok ? 0 : 1;
}
