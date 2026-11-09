/* Boot the desktop session and measure each client's OSS loopback tone. */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/**
 * Run the headless desktop case on a private GNU root and retain its output.
 */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    struct net_run r;
    const char *diagnose[] = {"/bin/bash", "-c", "cat /var/log/services; ps -eo pid,ppid,user,args",
                              NULL};
    const char *argv[] = {"/usr/bin/python3", "-", NULL};
    const char *clients[] = {"session", "chromium", "firefox", "alsa"};
    char password[128], token[96];
    unsigned i;

    memset(&r, 0, sizeof(r));
    net_case(&u, c);
    net_secret(&u, "demo-password", password, sizeof(password));
    if (net_step(&u, "boot") == ETUDE_OK)
        net_boot(&b, &u, "desktop", 1, NULL);
    net_seen(&b, "ext2: root: /dev/disk0p1");
    if (net_login(&b, "root", password, "# ") == ETUDE_OK && etude_guest_open(b.m) != ETUDE_OK)
        net_fail(&u, "the desktop guest command channel did not open");
    if (net_step(&u, "tones") == ETUDE_OK) {
        net_guest(&u, b.m, diagnose, NULL, NULL, "services", &r);
        net_run_free(&r);
    }
    if (net_ok(&u))
        net_guest(&u, b.m, argv, NULL, etude_artifact(c, "desktop-probe"), "tones", &r);
    for (i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
        snprintf(token, sizeof(token), "desktop: result: %s", clients[i]);
        net_assert(&u, clients[i], net_ok(&u) && net_exited0(&r) && strstr(r.out, token) != NULL,
                   "%s did not complete; see tones and tones.err", clients[i]);
    }
    net_run_free(&r);
    return net_end(&u);
}
