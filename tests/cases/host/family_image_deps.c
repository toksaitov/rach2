/* family_image_deps.c: actual manifest expansion and root dependency rules. */
#define _POSIX_C_SOURCE 200809L
#include "family_image.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static void mapped(char out[8192], struct fixture *f, const char *path)
{
    int x64 = strcmp(family_image_build_path(), "build") != 0;
    if (x64 && !strncmp(path, "build/", 6) && strncmp(path, "build/man/", 10) &&
        strncmp(path, "build/tests/system/", 19))
        snprintf(out, 8192, "%s/root/build/x86_64/%s", f->work, path + 6);
    else if (x64 && !strncmp(path, "vendor/musl/install/", 20))
        snprintf(out, 8192, "%s/root/build/x86_64/vendor-work/musl/install/%s", f->work, path + 20);
    else
        snprintf(out, 8192, "%s/root/%s", f->work, path);
}
static int fixture_manifest(struct fixture *f, const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[8192], kind[32], source[4096], full[8192], child[16384];
    int ok = fp != NULL;
    while (fp && fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%31s %*s %*s %4095s", kind, source) != 2)
            continue;
        mapped(full, f, source);
        if (!strcmp(kind, "tree") || !strcmp(kind, "exec-dir")) {
            ok &= fx_mkdir(full);
            snprintf(child, sizeof(child), "%s/closure-visible", full);
            ok &= fx_write(child, "fixture\n", 8, 0755);
            snprintf(child, sizeof(child), "%s/.closure-hidden", full);
            ok &= fx_write(child, "hidden\n", 7, 0755);
            snprintf(child, sizeof(child), "%s/closure-nested/file", full);
            ok &= fx_text(child, "nested\n");
        } else if ((!strcmp(kind, "file") || !strcmp(kind, "optional-file")) && !fx_exists(full))
            ok &= fx_text(full, "fixture\n");
    }
    if (fp)
        fclose(fp);
    return ok;
}
/* Derive coverage from the verified source tree, not a historical total. */
static unsigned source_manifests(struct fixture *f)
{
    DIR *dir = opendir(family_source(f, "krffs/manifests"));
    struct dirent *entry;
    unsigned count = 0;
    if (!dir)
        return 0;
    while ((entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (n >= 9 && !strcmp(entry->d_name + n - 9, ".manifest"))
            count++;
    }
    closedir(dir);
    return count;
}
static int closure(struct fixture *f)
{
    DIR *dir = opendir(family_path(f, "root/krffs/manifests"));
    struct dirent *e;
    int ok = dir != NULL;
    unsigned manifests = 0, paths = 0, expected = source_manifests(f);
    FILE *mk;
    if (!dir)
        return 0;
    mk = fopen(family_path(f, "root/Makefile"), "a");
    if (!mk) {
        closedir(dir);
        return 0;
    }
    fputs("\n.PHONY: family-deps\nfamily-deps:\n\t@$(MANIFEST_DEPS) $(FAMILY_MANIFEST)\n", mk);
    fclose(mk);
    while ((e = readdir(dir))) {
        char manifest[8192], option[8192], *line, *next;
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 9, ".manifest"))
            continue;
        manifests++;
        snprintf(manifest, sizeof(manifest), "%s/root/krffs/manifests/%s", f->work, e->d_name);
        if (!fixture_manifest(f, manifest)) {
            ok = 0;
            continue;
        }
        snprintf(option, sizeof(option), "FAMILY_MANIFEST=krffs/manifests/%s", e->d_name);
        ok &= family_image_root_make(f, "family-deps", option);
        ok &= strstr(f->out, "/.closure-hidden") == NULL;
        for (line = f->out; line && *line; line = next) {
            char full[16384];
            struct stat st;
            next = strchr(line, '\n');
            if (next)
                *next++ = 0;
            else
                next = line + strlen(line);
            if (*line == '/')
                snprintf(full, sizeof(full), "%s", line);
            else
                snprintf(full, sizeof(full), "%s/root/%s", f->work, line);
            if (!stat(full, &st)) {
                paths++;
                ok &= !S_ISDIR(st.st_mode);
            }
        }
    }
    closedir(dir);
    printf("manifest coverage: %u of %u source manifests, %u file paths\n", manifests, expected,
           paths);
    return ok && expected && manifests == expected && paths > manifests;
}
static int no_builder(struct fixture *f, const char *target)
{
    return family_image_root_make(f, target, "-n") && !strstr(f->out, "krffs/mkimage.sh ") &&
           !strstr(f->out, "mkext2.sh ") && !strstr(f->out, "mkdisk_grub.sh ") &&
           !strstr(f->out, "mkinstallstick.sh ");
}
static int db_has(struct fixture *f, const char *target, const char *dependency, int exact)
{
    char *at = f->out;
    size_t length = strlen(target);
    while (at && *at) {
        char *end = strchr(at, '\n');
        if (!end)
            end = at + strlen(at);
        if (!strncmp(at, target, length) && at[length] == ':') {
            char *found = strstr(at + length + 1, dependency);
            if (found && found < end && (!exact || found + strlen(dependency) == end))
                return 1;
        }
        at = *end ? end + 1 : end;
    }
    return 0;
}
int family_image_prerequisites(struct fixture *f)
{
    char test[4096], real[4096], probe[8192], vmprobe[8192], marker[8192], dependency[8192];
    int ok;
    if (!family_image_root_copy(f))
        return 0;
    family_row(f, "files", closure(f));
    fx_remove(family_path(f, "root/build"));
    if (!family_image_root_tools(f) || !family_image_root_make(f, "tests", NULL))
        return 0;
    snprintf(probe, sizeof(probe), "%s/root/%s/tests/conloadprobe", f->work,
             family_image_build_path());
    snprintf(vmprobe, sizeof(vmprobe), "%s/root/%s/tests/vmprobe", f->work,
             family_image_build_path());
    if (!fx_copy(probe, vmprobe))
        return 0;
    snprintf(test, sizeof(test), "%s/images/test.krffs", family_image_build_path());
    snprintf(real, sizeof(real), "%s/images/os-real.krffs", family_image_build_path());
    ok = family_image_root_make(f, test, NULL) && family_image_root_make(f, real, NULL);
    family_row(f, "current", ok && no_builder(f, test) && no_builder(f, real));
    snprintf(marker, sizeof(marker), "%s/root/%s/tests/system/imagerace.d/marker", f->work,
             family_image_build_path());
    family_row(f, "beneath", fx_text(marker, "nested\n") && no_builder(f, test));
    family_image_root_make(f, test, "-pn");
    snprintf(dependency, sizeof(dependency), " %s/tests/vmprobe ", family_image_build_path());
    family_row(f, "named", db_has(f, test, dependency, 0));
    snprintf(vmprobe, sizeof(vmprobe), "%s/tests/vmprobe", family_image_build_path());
    family_row(f, "waits", db_has(f, vmprobe, " tests", 1));
    return 1;
}
