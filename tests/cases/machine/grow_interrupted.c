/*
 * grow_interrupted.c: the scenario of machine.grow.interrupted-mbr and
 * machine.grow.interrupted-gpt: tools/dev/machine_grow.sh refusing a bad
 * plan and a corrupt boot copy before it writes the target, and each of
 * save, grow, restore and the repair of the boot environment cut at a
 * completed transfer and finished by a retry, after which the target has
 * every byte it had and boots without the stick.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lib_grow.h"

/* The assertions of both cases. */
static const char *const ids[] = {
    "bad-ranges",       "bad-boot",       "rejected-unchanged", "save",          "grown",
    "save-cut",         "save-retried",   "save-bytes",         "save-boots",    "grow-cut",
    "grow-restored",    "grow-writes",    "grow-bytes",         "grow-boots",    "restore-cut",
    "restore-restored", "restore-writes", "restore-bytes",      "restore-boots", "repair-cut",
    "repair-restored",  "repair-writes",  "repair-bytes",       "repair-boots",  NULL};

/* The points at which a step is cut, in the order they run. */
static const char *const points[] = {"save", "grow", "restore", "repair", NULL};

/* The disks of the case and what the scenario keeps of them between steps. */
struct state {
    const char *target; /* the target's overlay */
    const char *store;  /* the private copy of the store */
    char empty[1024];   /* the overlay before any boot */
    char grown[1024];   /* the overlay after the grow */
    char saved[1024];   /* the store after the grow, with the checked backup */
    char seed[1024];    /* the plan's seed, read off the store */
    int plan_ok;        /* the seed and the plan's ranges were read */
    long env;           /* the boot environment's first sector on the disk */
};

/**
 * Say whether the boot of `d` ran without a call that did not complete and
 * without `machine-grow: failed:`, putting why not in `why`.
 */
static int boot_clean(const struct drive *d, char *why, size_t size)
{
    if (d->failed) {
        snprintf(why, size, "%s", d->why);
        return 0;
    }
    if (grow_console_has(d, "machine-grow: failed:")) {
        snprintf(why, size, "the console says machine-grow: failed:");
        return 0;
    }
    return 1;
}

/** Remove the raw image NAME.raw that grow_raw_same kept as evidence. */
static void drop_raw(struct etude_case *c, const char *name)
{
    char raw[96];

    snprintf(raw, sizeof(raw), "%s.raw", name);
    unlink(etude_evidence(c, raw));
}

/*
 * rejected, save and grow each return 0 when the cuts can start from what
 * they left, -1 when they cannot, and 1 when the case was stopped.
 */

/**
 * Run the unchanged script on a plan whose last range of the grow is moved
 * by its own length, and its copy that changes the boot copy before its
 * checksum is made: each save must be refused before it writes the target.
 * The store is then made the case's store again.
 */
static int rejected(struct etude_case *c, struct state *s)
{
    char why[300], *out = NULL;
    struct drive d;
    int st, ranges, boot, ok;

    if (etude_step(c, "rejected") != ETUDE_OK)
        return 1;
    grow_recovery_boot(&d, c, "");
    grow_run(&d, "cp /tmp/grow-plan/ranges.txt /tmp/grow-plan/good-ranges.txt && "
                 "cp /tmp/grow-plan/bad-ranges.txt /tmp/grow-plan/ranges.txt");
    st = grow_shell(&d, "BADRANGES", &out,
                    "sh /tmp/grow-plan/machine_grow.sh save /dev/disk0 /mnt/grow /tmp/grow-plan");
    ranges = st == 1 && strstr(out, "ranges differ from the seed and target geometry") != NULL;
    free(out);
    ranges = grow_shell(&d, "PENDING", NULL, "test ! -e /mnt/grow/saved.pending") == 0 && ranges;
    grow_run(&d, "cp /tmp/grow-plan/good-ranges.txt /tmp/grow-plan/ranges.txt");
    st = grow_shell(&d, "BADBOOT", &out,
                    "sh /tmp/grow-plan/corrupt-boot.sh save /dev/disk0 /mnt/grow /tmp/grow-plan");
    boot = st == 1 && strstr(out, "the boot backup differs from the disk") != NULL;
    free(out);
    boot = grow_shell(&d, "UNSAVED", NULL, "test ! -e /mnt/grow/saved") == 0 && boot;
    grow_run(&d, "umount /mnt/grow");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    grow_assert(c, "bad-ranges", !d.failed && ranges,
                d.failed ? d.why
                         : "save of the moved range did not end with status 1 saying the ranges "
                           "differ, or left saved.pending");
    grow_assert(c, "bad-boot", !d.failed && boot,
                d.failed ? d.why
                         : "save of the corrupt boot copy did not end with status 1 saying it "
                           "differs, or published saved");
    ok = grow_raw_same(c, "rejected", 0, why, sizeof(why));
    grow_assert(c, "rejected-unchanged", ok, why);
    return grow_copy_file(etude_artifact(c, "store"), s->store);
}

/** Save the target and check the backup on the host, as the grow needs. */
static int save(struct etude_case *c, struct state *s)
{
    char why[300], *sums;
    struct drive d;
    int st, ok;

    if (etude_step(c, "save") != ETUDE_OK)
        return 1;
    grow_recovery_boot(&d, c, "");
    st = grow_shell(&d, "SAVED", NULL,
                    "sh /tmp/grow-plan/machine_grow.sh save /dev/disk0 /mnt/grow /tmp/grow-plan");
    grow_run(&d, "umount /mnt/grow");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    s->plan_ok = grow_read_plan(c, s->seed, sizeof(s->seed)) == 0;
    ok = boot_clean(&d, why, sizeof(why));
    if (ok && st != 0) {
        snprintf(why, sizeof(why), "save ended with status %d", st);
        ok = 0;
    }
    if (ok && grow_read_saved(c, s->store, "saved-", NULL) != 0) {
        snprintf(why, sizeof(why), "the saved files cannot be read on the host");
        ok = 0;
    }
    sums = ok ? evidence_text(c, "saved-SHA256SUMS") : NULL;
    ok = ok && sums != NULL && grow_sums_hold(c, sums, "saved-", why, sizeof(why));
    free(sums);
    grow_assert(c, "save", ok, why);
    return ok ? 0 : -1;
}

/**
 * Return the checksums to the store, grow the seed and zero the target's
 * first sector, as the grow before a restore leaves the disk, and keep the
 * overlay and the store as they are then for the cuts that start there.
 */
static int grow(struct etude_case *c, struct state *s)
{
    char why[300], *out = NULL, *sums, *status;
    struct drive d;
    int st, ok;

    if (etude_step(c, "grow") != ETUDE_OK)
        return 1;
    sums = evidence_text(c, "saved-SHA256SUMS");
    grow_recovery_boot(&d, c, "");
    grow_type_sums(&d, sums != NULL ? sums : "");
    free(sums);
    st = grow_shell(&d, "FINISHED", &out,
                    "sh /tmp/grow-plan/machine_grow.sh grow /dev/disk0 /mnt/grow /tmp/grow-plan");
    ok = st == 0 && strstr(out, "the measured grow and read-only check passed") != NULL;
    free(out);
    grow_run(&d, "dd if=/dev/zero of=/tmp/broken-mbr bs=512 count=1 && "
                 "reflash /dev/disk0 0 512 < /tmp/broken-mbr && diskctl reread /dev/disk0");
    grow_run(&d, "umount /mnt/grow");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    if (!boot_clean(&d, why, sizeof(why)))
        ok = 0;
    else if (!ok)
        snprintf(why, sizeof(why),
                 "grow did not end with status 0 saying the measured grow and "
                 "read-only check passed");
    status = evidence_text(c, "saved-boot.status");
    s->env = status != NULL ? grow_env_sector(status) : -1;
    free(status);
    if (ok && s->env < 0) {
        snprintf(why, sizeof(why), "the saved boot status puts the environment in no sector");
        ok = 0;
    }
    if (ok &&
        (grow_evidence(c, "grown.qcow2", s->grown, sizeof(s->grown)) == NULL ||
         grow_evidence(c, "store-saved.img", s->saved, sizeof(s->saved)) == NULL ||
         grow_copy_file(s->target, s->grown) != 0 || grow_copy_file(s->store, s->saved) != 0)) {
        snprintf(why, sizeof(why), "the grown target and its store cannot be kept");
        ok = 0;
    }
    grow_made(s->grown);
    grow_made(s->saved);
    grow_assert(c, "grown", ok, why);
    s->env = ok ? GROW_BOOT_FIRST + s->env : -1;
    return ok ? 0 : -1;
}

/**
 * Put the target and the store as `point` starts from, run its action
 * through machine-cut.sh, which stops the guest after a completed transfer
 * while the action still has work to do, and judge what the cut left.
 * Before the repair's cut the grown target's MBR is written back, so that
 * bootctl reads the environment, which is zeroed.
 */
static void cut(struct etude_case *c, struct state *s, const char *point)
{
    int restart = strcmp(point, "save") == 0 || strcmp(point, "grow") == 0;
    const char *action = strcmp(point, "repair") == 0 ? "restore" : point;
    char why[300], differ[200], id[64], text[96], *out = NULL;
    struct drive d;
    int ok = 1, same;

    why[0] = '\0';
    snprintf(id, sizeof(id), "%s-cut", point);
    if (grow_copy_file(restart ? s->empty : s->grown, s->target) != 0 ||
        grow_copy_file(strcmp(point, "save") == 0 ? etude_artifact(c, "store") : s->saved,
                       s->store) != 0) {
        grow_assert(c, id, 0, "the target and the store cannot be put as the cut starts");
        return;
    }
    grow_recovery_boot(&d, c, "");
    if (strcmp(point, "repair") == 0) {
        grow_run(&d, "dd if=/mnt/grow/saved/backup.bin of=/tmp/mbr bs=512 count=1 && "
                     "reflash /dev/disk0 0 512 < /tmp/mbr && diskctl reread /dev/disk0");
        grow_run(&d,
                 "dd if=/dev/zero of=/tmp/bad-env bs=512 count=2 && "
                 "reflash /dev/disk0 %ld 1024 < /tmp/bad-env",
                 s->env * 512);
        ok = grow_shell(&d, "UNPARSED", &out,
                        "bootctl --target /dev/disk0p1 env saved_entry recovery") == 1 &&
             strstr(out, "the environment block header is not GRUB") != NULL;
        free(out);
        if (!ok && !d.failed)
            snprintf(why, sizeof(why), "bootctl did not refuse the zeroed environment's header");
    }
    drive_send(&d,
               "GROW_TEST_CUT=%s sh /tmp/grow-plan/machine-cut.sh %s /dev/disk0 /mnt/grow "
               "/tmp/grow-plan; echo GROW_MACHINE_\"\"DONE",
               point, action);
    drive_expect(&d, "GROW_MACHINE_DONE");
    snprintf(text, sizeof(text), "machine-grow-test: cut: %s", point);
    if (ok && !d.failed && !grow_console_has(&d, text)) {
        snprintf(why, sizeof(why), "%s ended before its cut", action);
        ok = 0;
    }
    drive_stop(&d);
    if (drive_halted())
        return;
    if (!boot_clean(&d, why, sizeof(why)))
        ok = 0;
    same = grow_raw_same(c, id, 0, differ, sizeof(differ));
    if (strcmp(point, "save") == 0) {
        if (ok && !same)
            snprintf(why, sizeof(why), "the cut save changed the target: %s", differ);
        ok = ok && same;
    } else {
        if (!same)
            drop_raw(c, id);
        if (ok && same) {
            snprintf(why, sizeof(why), "%s left no work to restore", point);
            ok = 0;
        }
    }
    grow_assert(c, id, ok, why);
}

/**
 * Save again after the cut save, with its saved.pending moved aside as the
 * script asks, and require the backup to be the first save's.
 */
static void save_retry(struct etude_case *c, struct state *s)
{
    static const char *const names[] = {"backup.bin", "grow.plan", "ranges.txt",
                                        "SHA256SUMS", "boot.img",  NULL};
    char why[300], first[96], again[96], *sums;
    struct drive d;
    int st, ok, i;

    grow_recovery_boot(&d, c, "");
    st = grow_shell(&d, "RETRIED", NULL,
                    "test ! -e /mnt/grow/saved && test -d /mnt/grow/saved.pending && "
                    "mv /mnt/grow/saved.pending /mnt/grow/interrupted-save && "
                    "sh /tmp/grow-plan/machine_grow.sh save /dev/disk0 /mnt/grow /tmp/grow-plan");
    grow_run(&d, "umount /mnt/grow");
    drive_stop(&d);
    if (drive_halted())
        return;
    ok = boot_clean(&d, why, sizeof(why));
    if (ok && st != 0) {
        snprintf(why, sizeof(why), "the save after the cut ended with status %d", st);
        ok = 0;
    }
    if (ok && grow_read_saved(c, s->store, "retried-", names) != 0) {
        snprintf(why, sizeof(why), "the saved files cannot be read on the host");
        ok = 0;
    }
    for (i = 0; ok && names[i] != NULL; i++) {
        snprintf(first, sizeof(first), "saved-%s", names[i]);
        snprintf(again, sizeof(again), "retried-%s", names[i]);
        {
            char a[1024], b[1024], diff[200];

            if (grow_evidence(c, first, a, sizeof(a)) == NULL ||
                grow_evidence(c, again, b, sizeof(b)) == NULL ||
                !grow_same(a, b, 0, 0, diff, sizeof(diff))) {
                snprintf(why, sizeof(why), "the saved %s differs from the first save's", names[i]);
                ok = 0;
            }
        }
    }
    sums = ok ? evidence_text(c, "retried-SHA256SUMS") : NULL;
    ok = ok && sums != NULL && grow_sums_hold(c, sums, "retried-", why, sizeof(why));
    free(sums);
    grow_assert(c, "save-retried", ok, why);
    ok = grow_raw_same(c, "save-retried", 0, why, sizeof(why));
    grow_assert(c, "save-bytes", ok, why);
}

/**
 * Restore after the cut of `point`, which must finish what the cut left,
 * writing only saved sectors, and leave every byte of the target as it was.
 */
static void restore_retry(struct etude_case *c, struct state *s, const char *point)
{
    const char *said = strcmp(point, "repair") == 0 ? "boot environment repaired from boot.img"
                                                    : "boot environment unchanged";
    char why[300], id[64], name[64], *out = NULL;
    struct drive d;
    int st, ok;

    grow_recovery_boot(&d, c, "");
    st =
        grow_shell(&d, "RESTORED", &out,
                   "sh /tmp/grow-plan/machine_grow.sh restore /dev/disk0 /mnt/grow /tmp/grow-plan");
    ok = st == 0 &&
         strstr(out, "every saved range matches and the restored root checks clean") != NULL;
    free(out);
    grow_run(&d, "umount /mnt/grow");
    drive_stop(&d);
    if (drive_halted())
        return;
    if (boot_clean(&d, why, sizeof(why))) {
        if (!ok)
            snprintf(why, sizeof(why),
                     "restore did not end with status 0 saying every saved "
                     "range matches");
        else if (!grow_console_has(&d, said))
            snprintf(why, sizeof(why), "restore did not say %s", said);
        ok = ok && grow_console_has(&d, said);
    } else {
        ok = 0;
    }
    snprintf(id, sizeof(id), "%s-restored", point);
    grow_assert(c, id, ok, why);
    snprintf(id, sizeof(id), "%s-writes", point);
    snprintf(name, sizeof(name), "%s-retried", point);
    ok = s->plan_ok &&
         grow_writes_inside(c, name, s->target, s->seed, "plan-ranges.txt", why, sizeof(why));
    grow_assert(c, id, ok, s->plan_ok ? why : "the plan cannot be read from the store");
    snprintf(id, sizeof(id), "%s-bytes", point);
    ok = grow_raw_same(c, name, 0, why, sizeof(why));
    grow_assert(c, id, ok, why);
}

/**
 * Boot the finished target alone, which must start recovery and stay as it
 * was.
 */
static void target_boot(struct etude_case *c, const char *point)
{
    char why[300], id[64], name[64];
    int ok;

    ok = grow_target_boots(c, why, sizeof(why));
    if (drive_halted())
        return;
    snprintf(name, sizeof(name), "%s-booted", point);
    ok = ok && grow_raw_same(c, name, 0, why, sizeof(why));
    snprintf(id, sizeof(id), "%s-boots", point);
    grow_assert(c, id, ok, why);
}

/** Run the refusals, then cut each point and finish it. */
int etude_scenario(struct etude_case *c)
{
    struct state s;
    char step[64];
    int i, r;

    memset(&s, 0, sizeof(s));
    s.target = etude_private_copy(c, "target");
    s.store = etude_private_copy(c, "store");
    if (etude_artifact(c, "disk") == NULL || s.target == NULL || s.store == NULL ||
        grow_evidence(c, "empty.qcow2", s.empty, sizeof(s.empty)) == NULL ||
        grow_copy_file(s.target, s.empty) != 0) {
        grow_fail_rest(c, ids, "the target's overlay and the store cannot be made");
        return 0;
    }
    grow_made(s.empty);
    if ((r = rejected(c, &s)) == 0 && (r = save(c, &s)) == 0)
        r = grow(c, &s);
    if (r > 0)
        return 1;
    if (r < 0) {
        grow_fail_rest(c, ids, "not reached: the refusals, the save or the grow did not pass");
        grow_clean();
        return 0;
    }
    for (i = 0; points[i] != NULL; i++) {
        snprintf(step, sizeof(step), "%s-cut", points[i]);
        if (etude_step(c, step) != ETUDE_OK)
            return 1;
        cut(c, &s, points[i]);
        if (drive_halted())
            return 1;
        snprintf(step, sizeof(step), "%s-retry", points[i]);
        if (etude_step(c, step) != ETUDE_OK)
            return 1;
        if (strcmp(points[i], "save") == 0)
            save_retry(c, &s);
        else
            restore_retry(c, &s, points[i]);
        if (drive_halted())
            return 1;
        snprintf(step, sizeof(step), "%s-boot", points[i]);
        if (etude_step(c, step) != ETUDE_OK)
            return 1;
        target_boot(c, points[i]);
        if (drive_halted())
            return 1;
    }
    grow_fail_rest(c, ids, "not reached");
    grow_clean();
    return 0;
}
