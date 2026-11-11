/*
 * saveupdate_scenario.c: the scenario of machine.save-update: a Wine prefix
 * and a save on a data partition kept across a cold restart into a new root.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The guest's disks, on AHCI in the profile's order: the root, the payload
 * that holds save.py and the data partition. */
#define MOUNT_PAYLOAD                                                           \
    "mkdir -p /tmp/games /tmp/.X11-unix /data && chmod 1777 /tmp/.X11-unix && " \
    "mount -t ext2 -o ro /dev/disk1 /tmp/games && "

/* The first boot makes the data partition's file system as rach2-install
 * --data does, gives the account its directory, as /data/crysis is given,
 * and runs the save row; the second runs the load row and checks the volume
 * once it is unmounted. Each unmounts /data before the scenario cuts the
 * power, so that the restart is cold and the volume clean. */
static const char *const COMMANDS[] = {
    MOUNT_PAYLOAD
    "mke2fs -q -t ext2 -T huge -L 'Rach2 data' /dev/disk2 && "
    "mount -t ext2 /dev/disk2 /data && install -d -o desktop -g desktop /data/games && "
    "{ su -s /bin/bash desktop -c 'python3 /tmp/games/save.py save /tmp/save-results'; "
    "s=$?; umount /data || s=98; exit $s; }",
    MOUNT_PAYLOAD
    "mount -t ext2 /dev/disk2 /data && "
    "{ su -s /bin/bash desktop -c 'python3 /tmp/games/save.py load /tmp/save-results'; "
    "s=$?; umount /data || s=98; /sbin/e2fsck -fn /dev/disk2 || s=97; exit $s; }",
};

/**
 * Boot `machine` as step `step`, run its row through the guest channel and
 * require its exit 0 and its token, then end the boot by cutting the power.
 * @return 1 when the row passed.
 */
static int boot_row(struct net_case *u, const char *step, const char *machine, const char *row,
                    const char *command)
{
    struct net_boot b;
    struct net_run r;
    struct etude_termination end;
    char password[128], marker[64], *reply = NULL;
    const char *argv[] = {"/bin/bash", "-c", command, NULL};
    int matched = 0;

    memset(&b, 0, sizeof(b));
    b.u = u;
    net_secret(u, "demo-password", password, sizeof(password));
    if (net_step(u, step) == ETUDE_OK)
        net_boot(&b, u, machine, 1, NULL);
    net_seen(&b, "ext2: root: /dev/disk0p1");
    net_seen(&b, "rach2 login");
    if (net_login(&b, "root", password, "# ") == ETUDE_OK && etude_guest_open(b.m) != ETUDE_OK)
        net_fail(u, "the GNU guest command channel did not open");
    if (net_ok(u) && net_guest(u, b.m, argv, NULL, NULL, row, &r) == ETUDE_OK) {
        fputs(r.out, stdout);
        fputs(r.err, stderr);
        snprintf(marker, sizeof(marker), "SAVEROW OK %s", row);
        matched = net_exited0(&r) && net_find(r.out, marker) != NULL;
        net_run_free(&r);
    }
    if (b.m != NULL &&
        (etude_qmp(b.m, "quit", NULL, &reply) != ETUDE_OK || etude_wait_end(b.m, &end) != ETUDE_OK))
        net_fail(u, "the %s boot did not end", machine);
    free(reply);
    return matched;
}

/**
 * Write the save and the prefix in the first root, then load both in a new
 * one from the same data disk, which the attempt keeps between the boots;
 * a failed save leaves the load not run.
 */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    int saved;

    net_case(&u, c);
    saved = boot_row(&u, "save", "before", "save", COMMANDS[0]);
    net_assert(&u, "save", saved, "the save row failed; see save evidence");
    if (!saved)
        net_fail(&u, "no save to load");
    net_assert(&u, "load", boot_row(&u, "load", "after", "load", COMMANDS[1]),
               "the load row failed or the volume is not clean; see load evidence");
    return net_end(&u);
}
