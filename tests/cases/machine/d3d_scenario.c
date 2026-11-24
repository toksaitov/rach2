/* Run a Direct3D row as test from a separate disk on the GNU root, on the
 * guest's lavapipe or, for the d3dvenus family, on Venus. */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/**
 * Run `argv` through the guest channel as a d3dvenus vkd3d part, and report
 * the witness `unit` as each of its units ends, on run.py's line
 * `d3d: done: N of M`, so that the gap's watchdog bounds the time between
 * two units' ends and not the whole part.
 */
static enum etude_status vkd3d_part(struct net_case *u, struct etude_case_machine *m,
                                    const char *const argv[], struct net_run *r)
{
    char text[48];
    unsigned long n;

    if (net_guest_start(u, m, argv, NULL, "d3d: units: ") == ETUDE_OK) {
        for (n = 1;; n++) {
            snprintf(text, sizeof(text), "d3d: done: %lu of ", n);
            if (etude_guest_output_wait(m, text) != ETUDE_OK)
                break;
            etude_progress(u->c, "unit", n);
        }
    }
    return net_guest_finish(u, m, 0, "direct3d", r);
}

/**
 * Run one row and require its exit and complete result token to agree.
 *
 * A d3dvenus case is probe-PATH-N, wine-PATH-EXE-UNIT or vkd3d-PART, run
 * with run.py's -a venus against the Linux guest's reference on Venus. A
 * probe's N may end in -wow64 or -i386, its i386 build in that Proton mode.
 */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    struct net_run r;
    char password[128], mode[16], exe[32], unit[64];
    char family[16], row[128], command[2048], marker[192], path[16], n[16];
    const char *id = etude_case_id(c), *tail = strrchr(id, '.');
    const char *argv[] = {"/bin/bash", "-c", command, NULL};
    const char *adapter = "lavapipe";
    int matched = 0;

    net_case(&u, c);
    memset(&b, 0, sizeof(b));
    b.u = &u;
    tail = tail != NULL ? tail + 1 : id;
    if (strncmp(id, "machine.d3dvenus.", 17) == 0) {
        adapter = "venus";
        if (sscanf(tail, "probe-%15[^-]-%15s", path, n) == 2) {
            snprintf(family, sizeof(family), "probe");
            snprintf(row, sizeof(row), "%s/%s", path, n);
        } else if (sscanf(tail, "wine-%15[^-]-%31[^-]-%63s", mode, exe, unit) == 3) {
            snprintf(family, sizeof(family), "wine");
            snprintf(row, sizeof(row), "%s/%s:%s", mode, exe, unit);
        } else if (sscanf(tail, "vkd3d-%15s", n) == 1) {
            snprintf(family, sizeof(family), "vkd3d");
            snprintf(row, sizeof(row), "%s", n);
        } else {
            net_fail(&u, "unknown Direct3D case %s", id);
            return net_end(&u);
        }
    } else if (strncmp(id, "machine.d3d-wine.", 17) == 0 &&
               sscanf(tail, "%15[^-]-%31[^-]-%63s", mode, exe, unit) == 3) {
        snprintf(family, sizeof(family), "wine");
        snprintf(row, sizeof(row), "%s/%s:%s", mode, exe, unit);
    } else if (strncmp(id, "machine.d3d-vkd3d.", 18) == 0) {
        snprintf(family, sizeof(family), "vkd3d");
        snprintf(row, sizeof(row), "%s", tail);
    } else if (strcmp(id, "machine.d3d.private") == 0) {
        snprintf(family, sizeof(family), "private");
        snprintf(row, sizeof(row), "files");
    } else if (strncmp(id, "machine.d3d.", 12) == 0) {
        snprintf(family, sizeof(family), "probe");
        snprintf(row, sizeof(row), "%s", tail);
    } else {
        net_fail(&u, "unknown Direct3D case %s", id);
        return net_end(&u);
    }
    net_secret(&u, "demo-password", password, sizeof(password));
    if (net_step(&u, "boot") == ETUDE_OK)
        net_boot(&b, &u, "gnu", 1, NULL);
    net_seen(&b, "ext2: root: /dev/disk0p1");
    net_seen(&b, "rach2 login");
    if (net_login(&b, "root", password, "# ") == ETUDE_OK && etude_guest_open(b.m) != ETUDE_OK)
        net_fail(&u, "the GNU guest command channel did not open");
    /* The row runs as test in video and render alone, the groups of the
     * Debian account that took the references. test is also in kvm, whose
     * /dev/udmabuf gives lavapipe sync files and so Proton the keyed mutex
     * the references were taken without; BusyBox su would give every group. */
    snprintf(command, sizeof(command),
             "mkdir -p /tmp/d3d /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix && "
             "mount -t ext2 -o ro /dev/disk1 /tmp/d3d && "
             "python3 -c 'import grp, os, pwd, sys; u = pwd.getpwnam(\"test\"); "
             "os.setgroups([grp.getgrnam(g).gr_gid for g in (\"video\", \"render\")]); "
             "os.setgid(u.pw_gid); os.setuid(u.pw_uid); "
             "os.environ.update(HOME=u.pw_dir, SHELL=u.pw_shell, USER=u.pw_name, "
             "LOGNAME=u.pw_name); os.execvp(sys.argv[1], sys.argv[1:])' "
             "python3 /tmp/d3d/run.py -a %s %s %s /tmp/d3d-results",
             adapter, family, row);
    if (net_step(&u, "row") == ETUDE_OK &&
        (strcmp(adapter, "venus") == 0 && strcmp(family, "vkd3d") == 0
             ? vkd3d_part(&u, b.m, argv, &r)
             : net_guest(&u, b.m, argv, NULL, NULL, "direct3d", &r)) == ETUDE_OK) {
        fputs(r.out, stdout);
        fputs(r.err, stderr);
        snprintf(marker, sizeof(marker), "D3DROW OK %s %s", family, row);
        matched = net_exited0(&r) && net_find(r.out, marker) != NULL;
        net_assert(&u, "row", matched, "Direct3D row did not match Linux; see direct3d evidence");
        net_run_free(&r);
    } else
        net_assert(&u, "row", 0, "Direct3D row could not run");
    return net_end(&u);
}
