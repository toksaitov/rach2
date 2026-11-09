/* Run a Direct3D row as test from a separate disk on the GNU root. */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/** Run one row and require its exit and complete result token to agree. */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    struct net_run r;
    char password[128], mode[16], exe[32], unit[64];
    char family[16], row[128], command[1024], marker[192];
    const char *id = etude_case_id(c), *tail = strrchr(id, '.');
    const char *argv[] = {"/bin/bash", "-c", command, NULL};
    int matched = 0;

    net_case(&u, c);
    memset(&b, 0, sizeof(b));
    b.u = &u;
    tail = tail != NULL ? tail + 1 : id;
    if (strncmp(id, "machine.d3d-wine.", 17) == 0 &&
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
    snprintf(command, sizeof(command),
             "mkdir -p /tmp/d3d /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix && "
             "mount -t ext2 -o ro /dev/disk1 /tmp/d3d && "
             "su -s /bin/bash test -c 'python3 /tmp/d3d/run.py %s %s /tmp/d3d-results'",
             family, row);
    if (net_step(&u, "row") == ETUDE_OK &&
        net_guest(&u, b.m, argv, NULL, NULL, "direct3d", &r) == ETUDE_OK) {
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
