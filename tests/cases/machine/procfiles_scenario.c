/*
 * procfiles_scenario.c: installed /proc readers with a held connection from
 * the host.
 */
#include <stdio.h>
#include <sys/stat.h>

#include "netcase.h"
#include "console_report.h"

/**
 * Log in, establish the peer and run the probe that reports each applet's
 * rows.
 */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    struct etude_case_service *peer;
    struct etude_termination end;
    char password[128], port[16], probe[48];
    char *args[] = {"flood", port, "1", NULL};
    const char *image = etude_artifact(c, "real");
    struct stat st;
    uint64_t at;

    net_case(&u, c);
    net_secret(&u, "password", password, sizeof(password));
    snprintf(port, sizeof(port), "%u", etude_port(c, 0));
    net_step(&u, "applets");
    net_boot(&b, &u, "fixture", 1, NULL);
    net_login(&b, "root", password, "# ");
    net_line(&b, "dropbearmulti dropbear -R -g -p 22; echo SSH\"\"D-UP");
    net_wait(&b, "SSHD-UP");
    if (net_ok(&u) && (etude_service(c, "peer", args, &peer, &end) != ETUDE_OK ||
                       etude_service_wait(peer, "sshpeer: answered 1", 0, &at) != ETUDE_OK))
        net_fail(&u, "the guest did not accept the host connection");
    net_assert(&u, "peer-ready", 1, "the host's peer did not reach the SSH server");
    /* The image's size is the root's OS_REAL_MIB from the Makefile. */
    if (image == NULL || stat(image, &st) != 0) {
        net_fail(&u, "the root image's size cannot be read");
        st.st_size = 0;
    }
    snprintf(probe, sizeof(probe), "/procfilesmusl %lu", (unsigned long)(st.st_size / 1024));
    net_line(&b, probe);
    if (net_ok(&u) && console_report(c, b.m, "fixture", 1, b.at, &b.at) != ETUDE_OK)
        net_fail(&u, "the applet probe did not finish its report");
    return net_end(&u);
}
