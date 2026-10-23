/* family_grow_space.c: work placement, log failures and measured output. */
#define _POSIX_C_SOURCE 200809L
#include "family_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int functions(struct fixture *f)
{
    const char *src = family_path(f, "functions.sh"), *tmp = family_path(f, "edited.sh");
    return family_extract(f, "|alert|prepare_work|hash_file|check_plan|run_logged|run_measured|") &&
           fx_replace(src, tmp, "/proc/mounts", "\"$W/mounts\"") && fx_copy(tmp, src) &&
           fx_replace(src, tmp, "[ -c /dev/console ]", "[ -f \"$W/console\" ]") &&
           fx_copy(tmp, src) && fx_replace(src, tmp, "> /dev/console", "> \"$W/console\"") &&
           fx_copy(tmp, src);
}
static int setup(struct fixture *f)
{
    const char *a[] = {
        "/host-products/growset",          "--save",    "--ranges", "--lba", "266240",
        family_path(f, "store/seed.ext2"), "488130928", NULL};
    char hash[65], mounts[8192];
    if (!fx_mkdir(family_path(f, "store")) || !fx_mkdir(family_path(f, "work")) ||
        !fx_mkdir(family_path(f, "plan")) || !family_seed(f, "store/seed.ext2", 1024, "32768") ||
        !family_run(f, a, f->work, NULL, NULL) || !fx_text(family_path(f, "ranges.txt"), f->out) ||
        !fx_text(family_path(f, "work/ranges.txt"), f->out) ||
        !fx_text(family_path(f, "plan/ranges.txt"), f->out) ||
        !family_digest(family_path(f, "store/seed.ext2"), hash) || !functions(f))
        return 0;
    fx_env("seed_hash", hash);
    fx_env("store", family_path(f, "store"));
    fx_env("work", family_path(f, "work"));
    fx_env("plan", family_path(f, "plan"));
    fx_env("disk", "/dev/disk0");
    fx_env("source_device", "/dev/disk1p1");
    fx_env("start", "266240");
    fx_env("seed", "2097152");
    fx_env("count", "488130928");
    fx_env("table", "mbr");
    fx_env("identity", "MBRID=1acef00d");
    snprintf(mounts, sizeof(mounts), "/dev/disk1p1 %s/store exfat rw 0 0\n", f->work);
    return fx_text(family_path(f, "mounts"), mounts) &&
           fx_copy(family_path(f, "mounts"), family_path(f, "mounts.saved")) &&
           fx_text(family_path(f, "plan/grow.plan"), "plan\n") && fx_mock(f, "producer") &&
           fx_mock(f, "umount") && fx_mock(f, "mount") && fx_mock(f, "growroot");
}
static int run(struct fixture *f, const char *id, const char *call, int status, const char *message)
{
    char command[8192];
    int ok;
    fx_env("GROW_MODE", id);
    fx_text(family_path(f, "console"), "");
    remove(family_path(f, "work/command.status"));
    remove(family_path(f, "work/output.log"));
    snprintf(command, sizeof(command), "fail() { alert \"$*\"; exit 1; }; %s", call);
    family_eval(f, command);
    ok = f->status == status && (fx_regex(f->out, message) || fx_regex(f->err, message) ||
                                 fx_has(family_path(f, "console"), message));
    fx_text(family_path(f, id), f->err);
    return ok;
}
int family_grow_space(struct fixture *f)
{
    int ok;
    if (!setup(f))
        return 0;
    ok = run(f, "work", "prepare_work; printf '%s\\n' \"$work\"", 0, "logs:");
    {
        char expected[8192];
        snprintf(expected, sizeof(expected), "%s/store/machine-grow.", f->work);
        ok &= strstr(f->out, expected) != NULL;
    }
    family_row(f, "work", ok);
    fx_text(family_path(f, "mounts"), "");
    family_row(f, "absent",
               run(f, "absent", "prepare_work", 1, "store must be a mounted exFAT volume"));
    fx_copy(family_path(f, "mounts.saved"), family_path(f, "mounts"));
    family_row(f, "producer",
               run(f, "producer", "run_logged producer.log producer", 1,
                   "command failed; see .*producer.log"));
    ok = run(f, "log", "trap '' XFSZ; ulimit -f 1; run_logged output.log producer", 1,
             "writing .*output.log");
    family_row(f, "log",
               ok && fx_has(family_path(f, "console"), "writing ") &&
                   fx_has(family_path(f, "console"), "/output.log"));
    family_row(f, "ranges", run(f, "ranges", "check_plan", 0, ""));
    ok = run(f, "output", "trap '' XFSZ; ulimit -f 1; check_plan", 1,
             "creating .*expected-ranges.txt");
    family_row(f, "output",
               ok && fx_has(family_path(f, "work/growset.log"), "writing the range list failed:") &&
                   !strstr(f->err, "geometry is not supported"));
    family_row(f, "geometry",
               run(f, "geometry", "count=8; check_plan", 1, "smaller than the volume"));
    fx_text(family_path(f, "transitions"), "");
    ok = run(f, "measured", "run_measured", 0, "measured output");
    family_row(f, "measured",
               ok && fx_equal(family_path(f, "transitions"), "unmounted\nmounted\n") &&
                   fx_has(family_path(f, "work/grow.log"), "measured output"));
    family_row(f, "measure-fail",
               run(f, "measure-fail", "run_measured", 1, "measured grow failed"));
    family_row(f, "overflow", run(f, "overflow", "run_measured", 1, "grow output exceeds 64 KiB"));
    return 1;
}
