/*
 * grow_ram_full.c: the scenario of machine.grow.ram-full: save, grow and
 * restore of tools/dev/machine_grow.sh run with the recovery system's
 * RAM root full, keeping their work and their logs on the store, and a store
 * that fills while the restore's log grows failing the restore with a
 * message, after which the restore run again finishes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib_grow.h"

/* The assertions of the case. */
static const char *const ids[] = {"fixed-full",        "fixed-save",        "fixed-grown",
                                  "fixed-restored",    "fixed-ram-same",    "fixed-work",
                                  "fixed-bytes",       "fixed-boots",       "full-log-full",
                                  "full-log-save",     "full-log-grown",    "log-full",
                                  "full-log-restored", "full-log-ram-same", "full-log-work",
                                  "full-log-bytes",    "full-log-boots",    NULL};

/* The disks of the case. */
struct state {
    const char *target; /* the target's overlay */
    const char *store;  /* the private copy of the store */
    char empty[1024];   /* the overlay before any boot */
};

/**
 * Report assertion `NAME-WHAT` of round `name`: held when `held`, else
 * failed with the failure of the boot of `d`, or with `why` when the boot
 * did not fail.
 */
static void round_assert(struct etude_case *c, const char *name, const char *what,
                         const struct drive *d, int held, const char *why)
{
    char id[64];

    snprintf(id, sizeof(id), "%s-%s", name, what);
    grow_assert(c, id, !d->failed && held, d->failed ? d->why : why);
}

/** Say whether a line of `text` begins with `first` and has `then` after it. */
static int line_has(const char *text, const char *first, const char *then)
{
    const char *at, *eol, *hit;

    for (at = text; at != NULL && (at = strstr(at, first)) != NULL; at = eol) {
        eol = strchr(at, '\n');
        hit = strstr(at, then);
        if (hit != NULL && (eol == NULL || hit < eol))
            return 1;
        if (eol == NULL)
            break;
    }
    return 0;
}

/**
 * Fill the RAM root of the machine of `d` until its file system refuses a
 * write and has no free block: a file of whole blocks, its last fragments,
 * and the name records that are left. The heap and the image the stick
 * boots are not touched.
 *
 * @return 1 when the root has 10020 to 10240 KiB and is full, 0 otherwise.
 */
static int fill_root(struct drive *d)
{
    int ok;

    ok =
        grow_shell(d, "SIZED", NULL, "%s",
                   "df -k / > /mnt/grow/root.before && "
                   "awk 'NR == 2 {exit !($2 <= 10240 && $2 >= 10020)}' /mnt/grow/root.before") == 0;
    ok = grow_shell(d, "BLOCKS", NULL, "%s",
                    "dd if=/dev/zero of=/tmp/occupied bs=4096 2>/mnt/grow/fill.log; "
                    "test \"$?\" != 0") == 0 &&
         ok;
    ok = grow_shell(d, "TAIL", NULL, "%s",
                    "dd if=/dev/zero bs=512 2>/mnt/grow/fill-tail.log >> /tmp/occupied; "
                    "test \"$?\" != 0") == 0 &&
         ok;
    ok = grow_shell(d, "NAMES", NULL, "%s",
                    "i=0; while [ \"$i\" -lt 1024 ] && "
                    "touch \"/tmp/empty.$i\" 2>/mnt/grow/fill-names.log; do i=$((i+1)); done; "
                    "test \"$i\" -lt 1024") == 0 &&
         ok;
    ok = grow_shell(d, "FULL", NULL, "%s",
                    "df -k / > /mnt/grow/root.full && "
                    "awk 'NR == 2 {exit !($4 == 0)}' /mnt/grow/root.full") == 0 &&
         ok;
    return ok;
}

/**
 * Run round `name` with the RAM root full: save, check the backup on the
 * host, grow and restore, the round `full-log` with a restore that fills
 * the store first. The grow's output is kept in a shell variable until the
 * grow has unmounted the store and mounted it again, since a file held open
 * on the store would keep it from unmounting.
 *
 * @return 0; -1 when the target and the store cannot be put as the round
 *         starts; or 1 when the case was stopped.
 */
static int round_trip(struct etude_case *c, struct state *s, const char *name)
{
    int full_log = strcmp(name, "full-log") == 0;
    char why[300], prefix[64], ev[96], *out = NULL, *sums = NULL;
    struct drive d;
    int st, ok;

    if (etude_step(c, name) != ETUDE_OK)
        return 1;
    if (grow_copy_file(s->empty, s->target) != 0 ||
        grow_copy_file(etude_artifact(c, "store"), s->store) != 0)
        return -1;
    grow_recovery_boot(&d, c, "-n");
    round_assert(c, name, "full", &d, fill_root(&d),
                 "the RAM root is not of 10020 to 10240 KiB, or a file system with blocks free");

    st = grow_shell(&d, "SAVED", &out, "%s",
                    "sh /tmp/grow-plan/machine_grow.sh save /dev/disk0 /mnt/grow /tmp/grow-plan "
                    "> /mnt/grow/save.capture 2>&1");
    ok = st == 0 && strstr(out, "machine-grow: failed:") == NULL;
    free(out);
    snprintf(why, sizeof(why), "save ended with status %d", st);
    grow_run(&d, "umount -n /mnt/grow");
    snprintf(prefix, sizeof(prefix), "%s-saved-", name);
    snprintf(ev, sizeof(ev), "%sSHA256SUMS", prefix);
    if (ok && !d.failed && grow_read_saved(c, s->store, prefix, NULL) != 0) {
        snprintf(why, sizeof(why), "the saved files cannot be read on the host");
        ok = 0;
    }
    sums = ok && !d.failed ? evidence_text(c, ev) : NULL;
    ok = ok && sums != NULL && grow_sums_hold(c, sums, prefix, why, sizeof(why));
    round_assert(c, name, "save", &d, ok, why);

    grow_run(&d, "mount -n -t exfat /dev/disk1p1 /mnt/grow");
    grow_type_sums(&d, sums != NULL ? sums : "");
    free(sums);
    st = grow_shell(&d, "GROWN", &out, "%s",
                    "report=$(sh /tmp/grow-plan/machine_grow.sh grow /dev/disk0 /mnt/grow "
                    "/tmp/grow-plan 2>&1); rc=$?; "
                    "printf '%s\\n' \"$report\" > /mnt/grow/grow.capture && test \"$rc\" = 0");
    ok = st == 0 && strstr(out, "machine-grow: failed:") == NULL;
    free(out);
    snprintf(why, sizeof(why), "grow ended with status %d", st);
    round_assert(c, name, "grown", &d, ok, why);

    if (full_log) {
        st = grow_shell(&d, "LOGFULL", &out, "%s",
                        "sh /tmp/grow-plan/full-log.sh restore /dev/disk0 /mnt/grow "
                        "/tmp/grow-plan > /mnt/grow/failed.capture 2>&1");
        ok = st > 0 && line_has(out, "machine-grow: failed: writing ", "restore.log");
        free(out);
        grow_assert(c, "log-full", !d.failed && ok,
                    d.failed ? d.why
                             : "the restore with the store full did not fail saying that "
                               "writing restore.log failed");
        grow_run(&d, "rm /mnt/grow/full.fixture");
    }
    st = grow_shell(&d, "RESTORED", &out, "%s",
                    "sh /tmp/grow-plan/machine_grow.sh restore /dev/disk0 /mnt/grow "
                    "/tmp/grow-plan > /mnt/grow/restore.capture 2>&1 && "
                    "grep -q 'every saved range matches and the restored root checks clean' "
                    "/mnt/grow/restore.capture");
    ok = st == 0 && strstr(out, "machine-grow: failed:") == NULL;
    free(out);
    snprintf(why, sizeof(why),
             "restore ended with status %d, or did not say every saved range matches", st);
    round_assert(c, name, "restored", &d, ok, why);
    st = grow_shell(&d, "SAME", NULL, "%s",
                    "df -k / > /mnt/grow/root.after && "
                    "cmp /mnt/grow/root.full /mnt/grow/root.after");
    round_assert(c, name, "ram-same", &d, st == 0,
                 "the RAM root is not as full after the restore as before the save");
    st = grow_shell(&d, "WORK", NULL, "%s",
                    "test ! -e /tmp/machine-grow.* && "
                    "test \"$(ls -d /mnt/grow/machine-grow.* | wc -l)\" -ge 3 && "
                    "umount -n /mnt/grow");
    round_assert(c, name, "work", &d, st == 0,
                 "a work directory is in /tmp, or fewer than three are on the store");
    drive_stop(&d);
    return drive_halted() ? 1 : 0;
}

/**
 * Compare the target after round `name` with the disk as it was, and boot
 * it alone, which must start recovery.
 *
 * @return 0, or 1 when the case was stopped.
 */
static int round_boot(struct etude_case *c, const char *name)
{
    char step[64], id[64], why[300];
    int ok;

    snprintf(step, sizeof(step), "%s-boot", name);
    if (etude_step(c, step) != ETUDE_OK)
        return 1;
    snprintf(id, sizeof(id), "%s-bytes", name);
    ok = grow_raw_same(c, id, 0, why, sizeof(why));
    grow_assert(c, id, ok, why);
    ok = grow_target_boots(c, why, sizeof(why));
    if (drive_halted())
        return 1;
    snprintf(id, sizeof(id), "%s-boots", name);
    grow_assert(c, id, ok, why);
    return 0;
}

/**
 * Run the round with the script as it is, then the round that fills the
 * store.
 */
int etude_scenario(struct etude_case *c)
{
    static const char *const rounds[] = {"fixed", "full-log", NULL};
    struct state s;
    int i, r = 0;

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
    for (i = 0; rounds[i] != NULL && r == 0; i++) {
        if ((r = round_trip(c, &s, rounds[i])) == 0)
            r = round_boot(c, rounds[i]);
    }
    if (r > 0)
        return 1;
    grow_fail_rest(c, ids, "not reached: the target and the store cannot be put as a round starts");
    grow_clean();
    return 0;
}
