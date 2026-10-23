/* family_image_root.c: root make rules on private, actual source inputs. */
#define _POSIX_C_SOURCE 200809L
#include "family_image.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
const char *family_image_build_path(void)
{
    return getenv("ARCH") && !strcmp(getenv("ARCH"), "x86_64") ? "build/x86_64" : "build";
}
int family_image_root_copy(struct fixture *f)
{
    return fx_tree(f->tree, family_path(f, "root"));
}
int family_image_root_make(struct fixture *f, const char *target, const char *option)
{
    const char *a[100];
    static const char *ignored[] = {"preflight",
                                    "krffs",
                                    "rach2",
                                    "ish",
                                    "utils",
                                    "tools/etude/guest",
                                    "musl-programs",
                                    "check-dropbearmulti",
                                    "check-busybox",
                                    "check-setid",
                                    "check-tcc",
                                    "check-pdpmake",
                                    "check-e2fsck",
                                    "ports/doom",
                                    "vendor/musl/.stamp",
                                    "build/krffs/mkfs.krffs",
                                    "build/krffs/stageimage.krffs",
                                    "build/krffs/readfile.krffs",
                                    "build/tools/inspect/setidcheck",
                                    NULL};
    char compiler[4096], arch[80];
    unsigned n = 0, i;
    const char *architecture = !strcmp(family_image_build_path(), "build") ? "i686" : "x86_64";
    snprintf(compiler, sizeof(compiler), "CROSS_CC=/host-products/tcc/%s-tcc",
             !strcmp(architecture, "i686") ? "i386" : "x86_64");
    snprintf(arch, sizeof(arch), "ARCH=%s", architecture);
    a[n++] = "/usr/bin/make";
    a[n++] = "--no-print-directory";
    a[n++] = "-j32";
    for (i = 0; ignored[i]; i++) {
        a[n++] = "-o";
        a[n++] = ignored[i];
    }
    a[n++] = "PROGS=conloadprobe";
    a[n++] = "DEMO_CREDS=";
    a[n++] = "MAN_ALL=";
    a[n++] = "LIBC_STAMP=vendor/musl/.stamp";
    a[n++] = compiler;
    a[n++] = arch;
    if (option)
        a[n++] = option;
    a[n++] = target;
    a[n] = NULL;
    return family_run(f, a, family_path(f, "root"), NULL, NULL);
}
int family_image_root_tools(struct fixture *f)
{
    static const char *tools[] = {"mkfs.krffs", "stageimage.krffs", "readfile.krffs", NULL};
    unsigned i;
    char source[4096], dest[4096];
    for (i = 0; tools[i]; i++) {
        snprintf(source, sizeof(source), "/host-products/%s", tools[i]);
        snprintf(dest, sizeof(dest), "%s/root/build/krffs/%s", f->work, tools[i]);
        if (!fx_copy(source, dest))
            return 0;
    }
    return fx_copy("/host-products/setidcheck",
                   family_path(f, "root/build/tools/inspect/setidcheck")) &&
           fx_text(family_path(f, "root/krffs/manifests/test.manifest"),
                   "exec-dir 0755 / build/tests\n") &&
           fx_text(family_path(f, "root/krffs/manifests/os-real.manifest"),
                   "exec-dir 0755 / build/tests\n") &&
           fx_text(family_path(f, "root/tools/inspect/setid.allow"),
                   "manifest test\nsentinel /conloadprobe 0755:0:0\nsentinel /vmprobe "
                   "0755:0:0\ninit none\n"
                   "manifest os-real\nsentinel /conloadprobe 0755:0:0\nsentinel /vmprobe "
                   "0755:0:0\ninit none\n");
}
static int read_probe(struct fixture *f, const char *output)
{
    char image[8192];
    const char *a[] = {"/host-products/readfile.krffs", image, "/conloadprobe",
                       family_path(f, output), NULL};
    snprintf(image, sizeof(image), "%s/root/%s/images/test.krffs", f->work,
             family_image_build_path());
    return family_run(f, a, f->work, NULL, NULL);
}
static int appending(const char *path, const char *text)
{
    FILE *fp = fopen(path, "a");
    if (!fp)
        return 0;
    fputs(text, fp);
    return !fclose(fp);
}
int family_image_probe(struct fixture *f)
{
    char probe[8192];
    int ok;
    fx_env("ARCH", "i686");
    fx_env("RACH2_ARCH", NULL);
    fx_env("RACH2_PATH_MAP", NULL);
    fx_env("MFLAGS", "");
    if (!family_image_root_copy(f) || !family_image_root_tools(f))
        return 0;
    /* The probe case stages conloadprobe alone, as its original fixture did. */
    fx_text(family_path(f, "root/tools/inspect/setid.allow"),
            "manifest test\nsentinel /conloadprobe 0755:0:0\ninit none\n");
    ok = family_image_root_make(f, "build/images/test.krffs", NULL) &&
         read_probe(f, "initial.probe");
    snprintf(probe, sizeof(probe), "%s/root/build/tests/conloadprobe", f->work);
    ok &= fx_same(probe, family_path(f, "initial.probe"));
    ok &= family_image_old(family_path(f, "root")) &&
          appending(family_path(f, "root/tests/conloadprobe.c"),
                    "\nconst char imagerace_words[] = \"changed probe\";\n") &&
          family_image_root_make(f, "build/images/test.krffs", NULL) &&
          !fx_same(probe, family_path(f, "initial.probe")) && read_probe(f, "staged.probe") &&
          fx_same(probe, family_path(f, "staged.probe"));
    ok &= family_image_root_make(f, "build/images/test.krffs", NULL) &&
          !strstr(f->out, "krffs/mkimage.sh ");
    ok &= family_image_root_make(f, "build/images/test.krffs", "-n") &&
          !strstr(f->out, "krffs/mkimage.sh ");
    family_row(f, "staged", ok);
    return 1;
}
