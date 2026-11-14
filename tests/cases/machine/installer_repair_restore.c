/*
 * installer_repair_restore.c: the scenario of machine.installer-repair.restore:
 * a destroyed disk put back through the recovery menu and booted, then the
 * machine changed, updated from the stick and booted, and its root damaged and
 * updated from the archive on its recovery partition and booted.
 */
#include <stdio.h>
#include <string.h>

#include "lib_installer_repair.h"

/* The disk under repair, and the account whose home the rows make. */
#define RESTORE_DISK (256 * REPAIR_MIB)
#define RESTORE_USER "alice"

/** Report assertion `id` of a boot `d` that ran: held when it completed. */
static void judge_boot(struct etude_case *c, const char *id, const struct drive *d)
{
    repair_assert(c, id, !d->failed, d->why);
}

/** Restore the disk through `rach2-recover`'s menu, row 9. */
static void restore(struct drive *d, struct etude_case *c)
{
    drive_boot(d, c, "recovery", NULL);
    drive_expect(d, "recovery image");
    drive_expect(d, "# ");
    drive_send(d, "%s", "rach2-recover");
    drive_expect(d, "which?");
    drive_send(d, "%s", "1");
    drive_expect(d, "/dev/disk0:");
    drive_expect(d, "which?");
    drive_send(d, "%s", "3");
    drive_expect(d, "which disk shall be restored");
    drive_send(d, "%s", "/dev/disk0");
    drive_expect(d, "the image to put on it");
    drive_send(d, "%s", "");
    drive_expect(d, "to go on, anything else to stop");
    drive_send(d, "%s", "/dev/disk0");
    drive_expect(d, "reading the image to check it against its hash");
    drive_expect(d, "reflash: 16777216 of ");
    drive_expect(d, "restored and read back");
    drive_expect(d, "which?");
    drive_send(d, "%s", "6");
    drive_expect(d, "leaving");
    drive_send(d, "%s", "echo RECOVER\"\"DONE");
    drive_expect(d, "RECOVERDONE");
}

/**
 * Change the installed machine, row 11: root's password, its keys removed,
 * an account's home with a file of 8 MiB, a marked motd, all synced, and an
 * archive in the old layout whose group file has a line the root's lacks.
 */
static void change(struct drive *d, struct etude_case *c, const char *password)
{
    repair_login(d, c, "installed", password);
    drive_send(d, "echo root:%s | chpasswd && echo PASSWORD\"\"SET", REPAIR_NEWPASS);
    drive_expect(d, "PASSWORDSET");
    drive_send(d, "rm -rf /root/.ssh && mkdir -p /home/%s && chown 1000:1000 /home/%s",
               RESTORE_USER, RESTORE_USER);
    drive_send(d, "echo this-machines-own > /home/%s/mine && chown 1000:1000 /home/%s/mine",
               RESTORE_USER, RESTORE_USER);
    drive_send(d, "dd if=/dev/zero of=/home/%s/big bs=1048576 count=8 2>/dev/null", RESTORE_USER);
    drive_send(d, "%s", "echo a-marked-motd > /etc/motd && sync && echo CHANGED\"\"DONE");
    drive_expect(d, "CHANGEDDONE");
    drive_send(d, "%s",
               "mkdir -p /r && mount -t vfat /dev/disk0p2 /r && mkdir -p /r/rach2-config/etc && "
               "cp /etc/passwd /etc/shadow /r/rach2-config/etc/ && { cat /etc/group; echo "
               "tampered:x:99:; } > /r/rach2-config/etc/group && umount /r && echo "
               "ARCHIVE\"\"TAMPERED");
    drive_expect(d, "ARCHIVETAMPERED");
}

/**
 * Run `rach2-install ARGS` at the recovery shell, with no input, and wait for
 * the line typed after it, which ends the wait whatever rach2-install did, so
 * that a run that stops early fails its row at once and not at the case's
 * gap bound. It must exit with `status` and print each text of `seen`, a list
 * ended by NULL, in that order.
 */
static void install(struct drive *d, const char *args, int status, const char *const seen[])
{
    char end[64];
    uint64_t from = d->at, at;

    drive_send(d, "rach2-install %s < /dev/null; echo INSTALL\"\"STATUS=$?=INSTALL\"\"ENDED", args);
    drive_expect(d, "=INSTALLENDED");
    if (d->failed)
        return;
    snprintf(end, sizeof(end), "INSTALLSTATUS=%d=INSTALLENDED", status);
    if (etude_console_find(d->m, end, from, &at) != ETUDE_OK) {
        d->failed = 1;
        snprintf(d->why, sizeof(d->why), "rach2-install %.80s did not exit with status %d", args,
                 status);
        return;
    }
    for (; *seen != NULL; seen++)
        if (etude_console_find(d->m, *seen, from, &from) != ETUDE_OK) {
            d->failed = 1;
            snprintf(d->why, sizeof(d->why), "rach2-install %.50s did not print \"%.90s\"", args,
                     *seen);
            return;
        }
}

/**
 * Update the installed machine from the stick, row 12. The disk has no data
 * partition, and its recovery partition of 16 MiB keeps under 2 MiB free, so
 * the update refuses before it writes anything for want of room for the copy
 * of the homes, the 8 MiB file among them; `rach2-install --data` then makes
 * the data partition after the root, which keeps its 64 MiB, and the update
 * run again keeps its copy there.
 */
static void update(struct drive *d, struct etude_case *c)
{
    static const char *const refused[] = {
        "neither the data partition nor the recovery partition of /dev/disk0 has the",
        "a copy of the root's homes takes", "nothing was written", NULL};
    static const char *const room[] = {
        "made room: /dev/disk0p3 is 64M, and /dev/disk0p6 is the data partition", NULL};
    static const char *const updated[] = {"copying this machine's accounts",
                                          "differs from the root's etc/group; the root's is kept",
                                          "keeping a copy of the root's homes and configuration "
                                          "in /dev/disk0p6",
                                          "what reached the root is the image",
                                          "writing the accounts, the policy and the keys back",
                                          "the configuration is archived on",
                                          "armed for one trial boot",
                                          "updated:",
                                          NULL};
    char args[128];

    repair_recovery(d, c, "recovery");
    snprintf(args, sizeof(args), "--update /m/%s /dev/disk0", REPAIR_ROOT_NAME);
    install(d, args, 1, refused);
    install(d, "--data /dev/disk0 64M", 0, room);
    install(d, args, 0, updated);
    drive_send(d, "%s", "umount /m; echo UPDATE\"\"DONE");
    drive_expect(d, "UPDATEDONE");
}

/**
 * Boot the updated machine, row 13: the image's password is refused and the
 * machine's own taken, the keys stay absent, the home keeps its file, its
 * owner and its file of 8 MiB, and the motd is the image's again.
 */
static void updated(struct drive *d, struct etude_case *c, const char *password)
{
    drive_boot(d, c, "installed", NULL);
    drive_expect(d, "rach2 login:");
    drive_send(d, "%s", "root");
    drive_expect(d, "Password:");
    drive_send(d, "%s", password);
    drive_expect(d, "incorrect");
    drive_expect(d, "rach2 login:");
    drive_send(d, "%s", "root");
    drive_expect(d, "Password:");
    drive_send(d, "%s", REPAIR_NEWPASS);
    drive_expect(d, "# ");
    drive_send(d, "%s", "cat /etc/motd");
    drive_expect(d, "The log of this boot is in /dev/bootlog");
    drive_send(d, "%s", "test ! -d /root/.ssh && echo KEYS\"\"GONE");
    drive_expect(d, "KEYSGONE");
    drive_send(d, "cat /home/%s/mine; stat -c %%u /home/%s/mine", RESTORE_USER, RESTORE_USER);
    drive_expect(d, "this-machines-own");
    drive_expect(d, "1000");
    drive_send(d, "stat -c BIG=%%s /home/%s/big", RESTORE_USER);
    drive_expect(d, "BIG=8388608");
    drive_send(d, "%s", "echo UPDATED\"\"UP");
    drive_expect(d, "UPDATEDUP");
}

/** Damage the root's superblock and update from the archive, row 14. */
static void damage(struct drive *d, struct etude_case *c)
{
    static const char *const archived[] = {
        "the root does not read; taking the configuration saved on the recovery partition",
        "what reached the root is the image",
        "writing the accounts, the policy and the keys back",
        "the configuration is archived on",
        "updated:",
        NULL};
    char args[128];

    drive_boot(d, c, "recovery", NULL);
    drive_expect(d, "recovery image");
    drive_expect(d, "# ");
    drive_send(d, "%s",
               "dd if=/dev/zero of=/dev/disk0p3 bs=1024 seek=1 count=1 && echo DAMA\"\"GED");
    drive_expect(d, "DAMAGED");
    drive_send(d, "%s", "mkdir -p /m && mount -t exfat /dev/disk1p2 /m && echo MOUNT\"\"ED");
    drive_expect(d, "MOUNTED");
    snprintf(args, sizeof(args), "--update /m/%s /dev/disk0", REPAIR_ROOT_NAME);
    install(d, args, 0, archived);
    drive_send(d, "%s", "umount /m; echo ARCHIVE\"\"DONE");
    drive_expect(d, "ARCHIVEDONE");
}

/**
 * Boot the machine updated from the archive, row 15: the archive's password,
 * the group file the last update archived and not the tampered one, the
 * ordinary home file gone and the test account's keys kept with their modes.
 */
static void archived(struct drive *d, struct etude_case *c)
{
    repair_login(d, c, "installed", REPAIR_NEWPASS);
    drive_send(d,
               "grep -c ^tampered: /etc/group | sed s/^/TAMPERED=/; test ! -e /home/%s/mine && "
               "echo HOME\"\"GONE",
               RESTORE_USER);
    drive_expect(d, "TAMPERED=0");
    drive_expect(d, "HOMEGONE");
    drive_send(d, "%s", "stat -c %u:%g:%a /home/test/.ssh /home/test/.ssh/authorized_keys");
    drive_expect(d, "1000:1000:700");
    drive_expect(d, "1000:1000:600");
    drive_send(d, "%s", "bootctl commit && echo ARCHIVE\"\"UP");
    drive_expect(d, "ARCHIVEUP");
}

/**
 * Fill the disk with the pattern, so that it has no boot code and no table,
 * and run the rows on it in order, each a boot of its own.
 *
 * Every row runs and is judged whatever the one before it did, as the rows
 * of the suite did; a row after a failed one fails by itself.
 */
int etude_scenario(struct etude_case *c)
{
    const char *image = etude_artifact(c, "disk"), *p;
    char target[4096] = "", password[128] = "", bootimg[65] = "", boot9[65] = "";
    char detail[200];
    struct drive d;
    int ready;

    if (etude_step(c, "fill") != ETUDE_OK)
        return 1;
    p = etude_private_copy(c, "target");
    if (p != NULL)
        snprintf(target, sizeof(target), "%s", p);
    ready = p != NULL && fill_pattern(target, RESTORE_DISK) == 0 &&
            file_digest(image, 0, 440, bootimg) == 0 &&
            repair_password(c, password, sizeof(password)) == 0;
    repair_assert(c, "setup", ready,
                  "the disk could not be filled, or the image or the password read");

    if (etude_step(c, "restore") != ETUDE_OK)
        return 1;
    restore(&d, c);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, "restored", &d);
    if (file_digest(target, 0, 440, boot9) != 0)
        snprintf(boot9, sizeof(boot9), "unreadable");
    snprintf(detail, sizeof(detail), "the restored disk begins %s and the image %s", boot9,
             bootimg);
    repair_assert(c, "boot-code", ready && strcmp(boot9, bootimg) == 0, detail);

    if (etude_step(c, "recovered") != ETUDE_OK)
        return 1;
    repair_login(&d, c, "installed", password);
    drive_send(&d, "%s", "echo RECOVERED\"\"UP");
    drive_expect(&d, "RECOVEREDUP");
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, "recovered-boots", &d);

    if (etude_step(c, "change") != ETUDE_OK)
        return 1;
    change(&d, c, password);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, "changed", &d);

    if (etude_step(c, "update") != ETUDE_OK)
        return 1;
    update(&d, c);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, "updated", &d);

    if (etude_step(c, "updated") != ETUDE_OK)
        return 1;
    updated(&d, c, password);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, "updated-boots", &d);

    if (etude_step(c, "damage") != ETUDE_OK)
        return 1;
    damage(&d, c);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, "archive-update", &d);

    if (etude_step(c, "archived") != ETUDE_OK)
        return 1;
    archived(&d, c);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    judge_boot(c, "archive-boots", &d);
    repair_finish(c);
    return 0;
}
