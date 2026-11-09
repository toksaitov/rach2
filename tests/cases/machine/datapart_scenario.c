/*
 * datapart_scenario.c: the scenario of machine.datapart.keep: the data
 * partition a new install makes, a file on it kept across an update of the
 * root, trial roots booted from it, and the room `rach2-install --data` makes
 * for it on a disk installed without one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib_installer_repair.h"

/* The disks the installs write, and the root's size each one asks for. */
#define DATA_DISK (256 * REPAIR_MIB)
#define DATA_ROOT "96M"
#define DATA_USER "alice"
#define DATA_PASS "alices-own-password"

/* The data partition of a disk installed from the test stick, by the disk
 * GUID its image has (tools/image/mkdisk_grub.sh), and the trial roots on
 * it. */
#define DATA_SPEC  "GPTID=1acef00d-0f1e-4d2c-8b3a-c0ffee000001,6"
#define TRIAL_GOOD "/trial/root.img"
#define TRIAL_BAD  "/trial/bad.img"

/** Report assertion `id` of a boot `d` that ran: held when it completed. */
static void judge_boot(struct etude_case *c, const char *id, const struct drive *d)
{
    repair_assert(c, id, !d->failed, d->why);
}

/**
 * Answer the installer's questions from the stick's volume on /m, onto
 * /dev/disk0, with `size` for the root, and wait for its end.
 */
static void install(struct drive *d, const char *size)
{
    drive_send(d, "rach2-install --source /m");
    drive_expect(d, "disk shall Rach2 be written to");
    drive_send(d, "%s", "/dev/disk0");
    drive_expect(d, "the password for root");
    drive_send(d, "%s", REPAIR_NEWPASS);
    drive_expect(d, "again");
    drive_send(d, "%s", REPAIR_NEWPASS);
    drive_expect(d, "The name of the first account");
    drive_send(d, "%s", DATA_USER);
    drive_expect(d, "the password for " DATA_USER);
    drive_send(d, "%s", DATA_PASS);
    drive_expect(d, "again");
    drive_send(d, "%s", DATA_PASS);
    drive_expect(d, "uthorized keys");
    drive_send(d, "%s", "");
    drive_expect(d, "key for the recovery system");
    drive_send(d, "%s", "");
    drive_expect(d, "row the root");
    drive_send(d, "%s", size);
    drive_expect(d, "to go on, or stop to stop");
    drive_send(d, "%s", "/dev/disk0");
}

/**
 * Install with a data partition, put a file and the trial roots on it,
 * update the root from the stick and read the file back. The entry's line in
 * /proc/disk is printed before and after the update, as DATA= and AFTER=.
 */
static void install_keep(struct drive *d, struct etude_case *c)
{
    repair_recovery(d, c, "recovery");
    install(d, DATA_ROOT);
    drive_expect(d, "Rach2 data, is sectors");
    drive_expect(d, "making the data partition's file system on /dev/disk0p6");
    drive_expect(d, "growing the root to " DATA_ROOT ", up to the data partition");
    drive_expect(d, "installed:");
    drive_send(d, "%s", "grep '^part 0 6 ' /proc/disk | sed s/^/DATA=/; echo LISTED\"\"DATA");
    drive_expect(d, "LISTEDDATA");
    drive_send(d,
               "mkdir -p /d && mount -t ext2 /dev/disk0p6 /d && mkdir -p /d/trial && "
               "echo kept-across-the-update > /d/kept && cp /m/%s /d%s && "
               "dd if=/dev/zero of=/d%s bs=1048576 count=4 2>/dev/null && umount /d && "
               "echo DATA\"\"WRITTEN",
               REPAIR_ROOT_NAME, TRIAL_GOOD, TRIAL_BAD);
    drive_expect(d, "DATAWRITTEN");
    drive_send(d, "rach2-install --update /m/%s /dev/disk0", REPAIR_ROOT_NAME);
    drive_expect(d, "updated:");
    drive_send(d, "%s",
               "grep '^part 0 6 ' /proc/disk | sed s/^/AFTER=/; mount -t ext2 -o ro /dev/disk0p6 "
               "/d && cat /d/kept && umount /d && /sbin/e2fsck -fn /dev/disk0p6 > /dev/null && "
               "echo DATA\"\"CLEAN");
    drive_expect(d, "kept-across-the-update");
    drive_expect(d, "DATACLEAN");
    drive_send(d, "%s", "umount /m; echo INSTALL\"\"DONE");
    drive_expect(d, "INSTALLDONE");
}

/**
 * Judge that the data partition's entry is the same before and after the
 * update, from the DATA= and AFTER= lines of the console of boot `d`.
 */
static void judge_entry(struct etude_case *c, const struct drive *d)
{
    char before[200] = "", after[200] = "", detail[440];
    char *text = drive_console(d);
    int held = 0;

    if (text != NULL) {
        held = text_field(text, "DATA=part 0 6 ", 1, before, sizeof(before)) == 0 &&
               text_field(text, "AFTER=part 0 6 ", 1, after, sizeof(after)) == 0 &&
               strstr(before, "Rach2 data") != NULL && strcmp(before, after) == 0;
        free(text);
    }
    snprintf(detail, sizeof(detail), "before the update '%s', after it '%s'", before, after);
    repair_assert(c, "entry-kept", held, detail);
}

/**
 * Log in to the installed root as the install left it and arm trial root
 * `path`, after committing the boot when `commit` is set, then power off.
 */
static void arm(struct drive *d, struct etude_case *c, const char *path, int commit)
{
    repair_login(d, c, "installed", REPAIR_NEWPASS);
    if (commit) {
        drive_send(d, "%s", "bootctl commit");
        drive_expect(d, "committed: the candidate is the saved default");
    }
    drive_send(d, "bootctl rootimage %s:%s", DATA_SPEC, path);
    drive_expect(d, "bootctl: armed: the next boot starts the candidate's kernel");
    drive_send(d, "%s", "poweroff");
    drive_wait_end(d);
}

/** Boot the broken trial root: the kernel refuses it and restarts. */
static void boot_bad(struct drive *d, struct etude_case *c)
{
    drive_boot(d, c, "installed", NULL);
    drive_expect(d, "root: image: " TRIAL_BAD " on ");
    drive_expect(d, "refused: the root image did not come up; restarting into the saved default");
    drive_expect(d, "shutdown: Stopping the system");
    drive_wait_end(d);
}

/** Boot the installed root and see that the candidate's line started it. */
static void boot_installed(struct drive *d, struct etude_case *c)
{
    repair_login(d, c, "installed", REPAIR_NEWPASS);
    drive_send(d, "%s", "grep -q rach2.slot=candidate /proc/cmdline && echo CANDI\"\"DATE");
    drive_expect(d, "CANDIDATE");
}

/**
 * Boot the good trial root, which has the image's own password, and read
 * its line, its mounts and what bootctl says of the boot.
 */
static void boot_good(struct drive *d, struct etude_case *c, const char *password)
{
    repair_login(d, c, "installed", password);
    drive_send(d, "%s", "grep -q rach2.slot=trialroot /proc/cmdline && echo TRIAL\"\"LINE");
    drive_expect(d, "TRIALLINE");
    drive_send(d, "%s",
               "echo MOUNTS=$(grep -c -e '^/dev/loop0 / ext2' -e '^/dev/disk0p6 /data ext2' "
               "/proc/mounts)");
    drive_expect(d, "MOUNTS=2");
    drive_send(d, "%s", "test -f /data/kept && bootctl status | grep booted:");
    drive_expect(d, "bootctl: booted: trialroot");
    drive_send(d, "%s", "poweroff");
    drive_wait_end(d);
}

/**
 * Install onto the spare disk with the root grown whole, then make room for
 * the data partition with `rach2-install --data`, twice, the second run
 * finding each step done.
 */
static void make_room(struct drive *d, struct etude_case *c)
{
    repair_recovery(d, c, "room");
    install(d, "whole");
    drive_expect(d, "growing the root into the rest of the disk");
    drive_expect(d, "installed:");
    drive_send(d, "%s", "grep -c '^part 0 6 ' /proc/disk | sed s/^/ENTRIES=/");
    drive_expect(d, "ENTRIES=0");
    drive_send(d, "rach2-install --data /dev/disk0 %s; echo ROOM=$?", DATA_ROOT);
    drive_expect(d, "shrinking the file system on /dev/disk0p3 to " DATA_ROOT);
    drive_expect(d, "Rach2 data, is sectors");
    drive_expect(d, "making the data partition's file system on /dev/disk0p6");
    drive_expect(d, "made room:");
    drive_expect(d, "ROOM=0");
    drive_send(d, "rach2-install --data /dev/disk0 %s; echo AGAIN=$?", DATA_ROOT);
    drive_expect(d, "entry 6 is the data partition already");
    drive_expect(d, "has its file system already");
    drive_expect(d, "AGAIN=0");
    drive_send(d, "%s",
               "/sbin/e2fsck -fn /dev/disk0p3 > /dev/null && /sbin/e2fsck -fn /dev/disk0p6 > "
               "/dev/null && echo BOTH\"\"CLEAN");
    drive_expect(d, "BOTHCLEAN");
    drive_send(d, "%s", "umount /m; echo ROOM\"\"DONE");
    drive_expect(d, "ROOMDONE");
}

/** Boot the disk given room, log in, and mount its data partition. */
static void room_boots(struct drive *d, struct etude_case *c)
{
    repair_login(d, c, "room-installed", REPAIR_NEWPASS);
    drive_send(d, "%s",
               "mkdir -p /d && mount -t ext2 /dev/disk0p6 /d && echo here > /d/f && umount /d && "
               "echo ROOM\"\"MOUNTED");
    drive_expect(d, "ROOMMOUNTED");
}

/**
 * Run one boot of the scenario with `run` and judge it as `id`.
 * @return 0, or 1 when the executor has stopped the case.
 */
static int boot_step(struct etude_case *c, const char *step, const char *id,
                     void (*run)(struct drive *, struct etude_case *))
{
    struct drive d;

    if (etude_step(c, step) != ETUDE_OK)
        return 1;
    run(&d, c);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, id, &d);
    return 0;
}

/** Arm the broken trial root, committing the updated root first. */
static void arm_bad(struct drive *d, struct etude_case *c)
{
    arm(d, c, TRIAL_BAD, 1);
}

/** Arm the good trial root. */
static void arm_good(struct drive *d, struct etude_case *c)
{
    arm(d, c, TRIAL_GOOD, 0);
}

/**
 * Fill both disks with the pattern, so that they have no table, and run the
 * boots in order. Every boot runs and is judged whatever the one before it
 * did; a boot after a failed one fails by itself.
 */
int etude_scenario(struct etude_case *c)
{
    char password[128] = "";
    const char *names[] = {"target", "spare"};
    struct drive d;
    int ready = repair_password(c, password, sizeof(password)) == 0, i;
    const char *p;

    if (etude_step(c, "fill") != ETUDE_OK)
        return 1;
    for (i = 0; i < 2; i++) {
        p = etude_private_copy(c, names[i]);
        ready = ready && p != NULL && fill_pattern(p, DATA_DISK) == 0;
    }
    repair_assert(c, "setup", ready, "a disk could not be filled, or the password read");

    if (etude_step(c, "install") != ETUDE_OK)
        return 1;
    install_keep(&d, c);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, "data-kept", &d);
    judge_entry(c, &d);

    if (boot_step(c, "arm-bad", "armed-bad", arm_bad) ||
        boot_step(c, "bad", "bad-refused", boot_bad) ||
        boot_step(c, "fell-back", "fell-back", boot_installed) ||
        boot_step(c, "arm-good", "armed-good", arm_good))
        return 1;

    if (etude_step(c, "good") != ETUDE_OK)
        return 1;
    boot_good(&d, c, password);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, "trial-boots", &d);

    if (boot_step(c, "once", "trial-once", boot_installed) ||
        boot_step(c, "room", "room-made", make_room) ||
        boot_step(c, "room-boots", "room-boots", room_boots))
        return 1;
    repair_finish(c);
    return 0;
}
