/* family_image_tree.c: private make input and deterministic file times. */
#define _POSIX_C_SOURCE 200809L
#include "family_image.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
int family_image_tree(struct fixture *f)
{
    static const char *scripts[] = {"mkimage.sh", "manifest_deps.sh", "programs.sh", NULL};
    static const char *tools[] = {"mkfs.krffs", "stageimage.krffs", NULL};
    char text[16384], src[4096], dst[4096];
    unsigned i;
    for (i = 0; scripts[i]; i++) {
        snprintf(src, sizeof(src), "krffs/%s", scripts[i]);
        snprintf(dst, sizeof(dst), "%s/repo/krffs/%s", f->work, scripts[i]);
        if (!fx_copy(family_source(f, src), dst))
            return 0;
    }
    for (i = 0; tools[i]; i++) {
        snprintf(src, sizeof(src), "/host-products/%s", tools[i]);
        snprintf(dst, sizeof(dst), "%s/repo/build/krffs/%s", f->work, tools[i]);
        if (!fx_copy(src, dst))
            return 0;
    }
    if (!fx_copy("/host-products/setidcheck",
                 family_path(f, "repo/build/tools/inspect/setidcheck")) ||
        !fx_copy(family_source(f, "tools/inspect/setid.allow"),
                 family_path(f, "repo/tools/inspect/setid.allow")) ||
        !fx_text(family_path(f, "repo/Makefile"), "build/tools/inspect/setidcheck: ;\n") ||
        !fx_mkdir(family_path(f, "tree/bin")))
        return 0;
    snprintf(text, sizeof(text), "exec-dir 0755 / %s/tree/bin\n", f->work);
    if (!fx_text(family_path(f, "tree/imagerace-tree.manifest"), text) ||
        !fx_text(family_path(f, "tree/prog.c"),
                 "int main(void) {return 0;}\nconst char words[] = \"IMAGERACE FIRST WORDS\";\n"))
        return 0;
    snprintf(text, sizeof(text),
             "IMG = $(TREE)/tree.krffs\nMANIFEST = $(TREE)/imagerace-tree.manifest\n"
             "STAGED := $(shell sh krffs/manifest_deps.sh $(MANIFEST))\n"
             "$(IMG): $(MANIFEST) $(STAGED) | prog\n\tsh krffs/mkimage.sh $(MANIFEST) $@ 2\n"
             "$(STAGED): prog\n\t+@:\n-include $(IMG).d\n"
             "_ := $(shell sh krffs/programs.sh check $(wildcard $(IMG).programs))\n"
             "prog:\n\t$(MAKE) --no-print-directory -f $(TREE)/prog.mk TREE=$(TREE) "
             "$(TREE)/bin/prog\n.PHONY: prog\n");
    return fx_text(family_path(f, "tree/Makefile"), text) &&
           fx_text(family_path(f, "tree/prog.mk"),
                   "$(TREE)/bin/prog: $(TREE)/prog.c\n\tcc -o $@ $(TREE)/prog.c\n");
}
int family_image_make(struct fixture *f, const char *target)
{
    char tree[8192];
    const char *a[] = {"/usr/bin/make",
                       "--no-print-directory",
                       "-f",
                       family_path(f, "tree/Makefile"),
                       tree,
                       target,
                       NULL};
    snprintf(tree, sizeof(tree), "TREE=%s/tree", f->work);
    return family_run(f, a, family_path(f, "repo"), NULL, NULL);
}
int family_image_build(struct fixture *f, struct fx_child *p, const char *const env[])
{
    const char *a[] = {"/bin/sh",
                       family_path(f, "repo/krffs/mkimage.sh"),
                       family_path(f, "tree/imagerace-tree.manifest"),
                       family_path(f, "tree/tree.krffs"),
                       "2",
                       NULL};
    return fx_start(f, p, a, family_path(f, "repo"), env, NULL);
}
int family_image_left(struct fixture *f)
{
    DIR *dir = opendir(family_path(f, "tree"));
    struct dirent *entry;
    int count = 0;
    if (!dir)
        return -1;
    while ((entry = readdir(dir))) {
        const char *s = entry->d_name;
        if (!strncmp(s, "tree.krffs.", 11) && strcmp(s, "tree.krffs.d") &&
            strcmp(s, "tree.krffs.programs"))
            count++;
    }
    closedir(dir);
    return count;
}
int family_image_old(const char *root)
{
    struct stat st;
    struct timespec time[2] = {{946684800, 0}, {946684800, 0}};
    int ok;
    if (lstat(root, &st))
        return 0;
    ok = 1;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(root);
        struct dirent *e;
        if (!dir)
            return 0;
        while ((e = readdir(dir))) {
            char path[8192];
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
            ok &= family_image_old(path);
        }
        closedir(dir);
    }
    return utimensat(AT_FDCWD, root, time, AT_SYMLINK_NOFOLLOW) == 0 && ok;
}
