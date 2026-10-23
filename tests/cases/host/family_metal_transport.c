/* family_metal_transport.c: the complete acceptance script under 34 faults. */
#define _POSIX_C_SOURCE 200809L
#include "family_metal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int setup(struct fixture *f)
{
    static const char *mocks[] = {"ssh",        "mcopy",          "sh", "mkfs.krffs",
                                  "fsck.krffs", "readfile.krffs", NULL};
    unsigned i;
    if (!fx_copy(family_source(f, "tests/system/accept/metal_accept.sh"),
                 family_path(f, "transport/tests/system/accept/metal_accept.sh")) ||
        !fx_text(family_path(f, "transport/tests/system/accept/su_command.sh"), "") ||
        !fx_text(family_path(f, "transport/krffs/mkimage.sh"), "") ||
        !fx_text(family_path(f, "key"), ""))
        return 0;
    for (i = 0; mocks[i]; i++)
        if (!fx_mock(f, mocks[i]))
            return 0;
    for (i = 3; mocks[i]; i++) {
        char bin[8192], out[8192];
        snprintf(bin, sizeof(bin), "%s/bin/%s", f->work, mocks[i]);
        snprintf(out, sizeof(out), "%s/transport/build/krffs/%s", f->work, mocks[i]);
        if (!fx_copy(bin, out))
            return 0;
    }
    fx_env("RACH2_ACCEPT_KEY", family_path(f, "key"));
    fx_env("RACH2_ACCEPT_PASSWORD", "unused-fixture-value");
    fx_env("RACH2_ACCEPT_DD", NULL);
    fx_env("RACH2_ACCEPT_BOOTLOG", NULL);
    fx_env("RACH2_ACCEPT_FAT_IMAGE", NULL);
    fx_env("RACH2_ACCEPT_RECORDS", NULL);
    fx_env("RACH2_ACCEPT_USER", NULL);
    fx_env("FAMILY_METAL", "transport");
    return fx_text(family_path(f, "healthy-bootlog"), "boot: Rach2 Booting\next2: check: clean\n");
}
static int stick_files(struct fixture *f)
{
    return fx_text(family_path(f, "disk1.img"), "disk1 original bytes\n") &&
           fx_text(family_path(f, "disk1.before"), "disk1 original bytes\n") &&
           fx_text(family_path(f, "disk10.img"), "disk10 original bytes\n") &&
           fx_text(family_path(f, "disk10.before"), "disk10 original bytes\n");
}
static int check_stick(struct fixture *f, const char *mode, int expected)
{
    int ok = fx_same(family_path(f, "disk10.img"), family_path(f, "disk10.before")) &&
             fx_equal(family_path(f, "disk1.img"), "disk1 original bytes\naccept\n");
    if (!expected)
        ok &= strstr(f->out, "stick: the volume TESTVOL on /dev/disk1p1 mounts") != NULL;
    if (!strcmp(mode, "fat-busy") || !strcmp(mode, "fat-remount"))
        ok &= strstr(f->out, "stick: cached bytes match") != NULL &&
              !fx_exists(family_path(f, "host-read")) &&
              !strstr(f->out, "stick: remount bytes match");
    if (!strcmp(mode, "fat-good") || !strcmp(mode, "fat-host"))
        ok &= strstr(f->out, "stick: remount bytes match after successful unmounts") != NULL &&
              fx_exists(family_path(f, "host-read"));
    if (!strcmp(mode, "fat-busy"))
        ok &= strstr(f->out, "FAIL: stick: first unmount failed") != NULL;
    if (!strcmp(mode, "fat-remount"))
        ok &= strstr(f->out, "FAIL: stick: remount bytes differ") != NULL;
    if (!strcmp(mode, "fat-host"))
        ok &= strstr(f->out, "FAIL: stick: host FAT bytes differ") != NULL;
    if (!strcmp(mode, "fat-good"))
        ok &= strstr(f->out, "stick: host FAT bytes match while unmounted") != NULL;
    return ok;
}
static int check(struct fixture *f, const char *mode, int expected, unsigned su)
{
    int stick = !strcmp(mode, "disk-prefix") || !strncmp(mode, "fat-", 4), ok;
    const char *a[] = {"/bin/sh",
                       family_path(f, "transport/tests/system/accept/metal_accept.sh"),
                       "fixture",
                       stick ? "22" : NULL,
                       stick ? "APPROVED01" : NULL,
                       NULL};
    char name[4096];
    fx_env("ACCEPT_CASE", mode);
    fx_env("RACH2_ACCEPT_SU", su ? "1" : "0");
    remove(family_path(f, "host-read"));
    if (stick && !stick_files(f))
        return 0;
    family_run(f, a, f->work, NULL, NULL);
    ok = expected == 1 ? f->status != 0 : f->status == expected;
    if (!expected)
        ok &= fx_regex(f->out, "^metal-accept: PASS$");
    else
        ok &= !fx_regex(f->out, "^metal-accept: PASS");
    if (stick)
        ok &= check_stick(f, mode, expected);
    if (!strcmp(mode, "hash-fail"))
        ok &= strstr(f->out, "the MBR hashes to") == NULL;
    snprintf(name, sizeof(name), "transport.%u.%s.out", su, mode);
    fx_text(family_path(f, name), f->out);
    snprintf(name, sizeof(name), "transport.%u.%s.err", su, mode);
    fx_text(family_path(f, name), f->err);
    fprintf(stderr, "transport: su=%u %s: %s\n", su, mode, ok ? "passed" : "failed");
    return ok;
}
int family_metal_transport(struct fixture *f)
{
    static const char *faults[] = {"cmdline-fail", "uname-fail", "hash-fail",
                                   "image-fail",   "tmp-fail",   "tmp-empty",
                                   "put-fail",     "live-fault", NULL};
    unsigned su, i;
    int ok = setup(f);
    if (!ok)
        return 0;
    for (su = 0; su < 2; su++) {
        ok &= check(f, "good", 0, su);
        for (i = 0; faults[i]; i++)
            ok &= check(f, faults[i], 1, su);
        ok &= check(f, "live-clean", 0, su);
        ok &= check(f, "disk-prefix", 0, su);
        fx_env("RACH2_ACCEPT_FAT_IMAGE", "fixture");
        ok &= check(f, "fat-good", 0, su);
        ok &= check(f, "fat-busy", 1, su);
        ok &= check(f, "fat-remount", 1, su);
        ok &= check(f, "fat-host", 1, su);
        fx_env("RACH2_ACCEPT_FAT_IMAGE", NULL);
        fx_env("RACH2_ACCEPT_DD", "/tmp/reader");
        ok &= check(f, "reader-override", 2, su);
        fx_env("RACH2_ACCEPT_DD", NULL);
        fx_env("RACH2_ACCEPT_BOOTLOG", family_path(f, "healthy-bootlog"));
        ok &= check(f, "saved-bootlog", 2, su);
        fx_env("RACH2_ACCEPT_BOOTLOG", NULL);
    }
    family_row(f, "transport", ok);
    return 1;
}
