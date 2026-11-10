/* Run one browser acceptance case in a private desktop root. */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/** Stage the acceptance files and any clips through the root guest channel. */
static void stage(struct net_case *u, struct net_boot *b, const char *mode)
{
    struct net_run r;
    const char *argv[] = {"/bin/bash", "-c", NULL, NULL};
    const char *command = "mkdir -p /tmp/graphics /tmp/browser-out && "
                          "mount -t ext2 -o ro /dev/disk1 /tmp/graphics && "
                          "chown desktop /tmp/browser-out";

    memset(&r, 0, sizeof(r));
    if (strncmp(mode, "video-", 6) == 0)
        command = "mkdir -p /tmp/graphics /tmp/browser-out && "
                  "mount -t ext2 -o ro /dev/disk1 /tmp/graphics && "
                  "mount -t ext2 -o ro /dev/disk2 /tmp/graphics/clips && "
                  "chown desktop /tmp/browser-out";
    argv[2] = command;
    if (net_step(u, "stage") == ETUDE_OK &&
        net_guest(u, b->m, argv, NULL, NULL, "stage", &r) == ETUDE_OK && !net_exited0(&r))
        net_fail(u, "the browser data disk could not be mounted");
    net_run_free(&r);
}

/** Retain the host load and live admission ledger before the browser runs. */
static void host_record(struct net_case *u, struct net_boot *b)
{
    /* common.sh runs as the artifact itself, by its #! line, since sh is
     * no tool of the cases' manifests. */
    char *host[] = {"common", "host", NULL};
    const char *guest[] = {"/bin/sh", "-c", "cat > /tmp/graphics.host", NULL};
    struct net_run h, r;

    memset(&h, 0, sizeof(h));
    memset(&r, 0, sizeof(r));
    if (net_host(u, host, "host-record", &h) == ETUDE_OK) {
        if (!net_exited0(&h))
            net_fail(u, "the host record could not be read");
        else if (net_guest(u, b->m, guest, NULL, etude_evidence(u->c, "host-record"), "host-stage",
                           &r) == ETUDE_OK &&
                 !net_exited0(&r))
            net_fail(u, "the host record could not be staged");
    }
    net_run_free(&h);
    net_run_free(&r);
}

/**
 * Boot the case's root, run its ordinary browser and retain the result.
 * The guest channel stages files as root; the browser keeps its sandbox.
 */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    struct net_run r;
    const char *id = etude_case_id(c), *tail = strrchr(id, '.');
    const char *argv[] = {"/bin/bash", "-c", NULL, NULL};
    char browser[16], mode[32], password[128], command[2048], token[128];
    int ran = 0, judged = 0;

    memset(&b, 0, sizeof(b));
    memset(&r, 0, sizeof(r));
    net_case(&u, c);
    b.u = &u;
    if (tail == NULL || sscanf(tail + 1, "%15[^-]-%31s", browser, mode) != 2 ||
        (strcmp(browser, "chromium") != 0 && strcmp(browser, "firefox") != 0)) {
        net_fail(&u, "unknown browser case %s", id);
        return net_end(&u);
    }
    net_secret(&u, "demo-password", password, sizeof(password));
    if (net_step(&u, "boot") == ETUDE_OK)
        net_boot(&b, &u, "gnu", 1, NULL);
    net_seen(&b, "ext2: root: /dev/disk0p1");
    net_seen(&b, "rach2 login");
    if (net_login(&b, "root", password, "# ") == ETUDE_OK && etude_guest_open(b.m) != ETUDE_OK)
        net_fail(&u, "the browser guest command channel did not open");
    stage(&u, &b, mode);
    host_record(&u, &b);
    snprintf(command, sizeof(command),
             "su -s /bin/sh desktop -c 'GRAPHICS_CLASS=conformance GRAPHICS_DONE=1; "
             "export GRAPHICS_CLASS GRAPHICS_DONE; . /tmp/graphics/guest-env.sh && "
             "graphics_env > /tmp/graphics.env && sh /tmp/graphics/browser/run.sh %s %s "
             "/tmp/browser-out'",
             browser, mode);
    if (strcmp(mode, "sound") == 0)
        snprintf(command, sizeof(command),
                 "python3 /tmp/graphics/browser/sound.py oss %s /tmp/browser-out", browser);
    else if (strcmp(mode, "sandbox") == 0 && strcmp(browser, "chromium") == 0)
        snprintf(command, sizeof(command),
                 "python3 /tmp/graphics/browser/sandbox.py desktop %s /tmp/browser-out", browser);
    argv[2] = command;
    if (net_step(&u, "run") == ETUDE_OK &&
        net_guest(&u, b.m, argv, NULL, NULL, "browser", &r) == ETUDE_OK) {
        fputs(r.out, stdout);
        fputs(r.err, stderr);
        snprintf(token, sizeof(token), "browser: result: %s %s", browser, mode);
        if (strcmp(mode, "webgl") == 0)
            snprintf(token, sizeof(token), "webgl: result: %s", browser);
        else if (strncmp(mode, "video-", 6) == 0)
            snprintf(token, sizeof(token), "video: result: %s", browser);
        ran = net_exited0(&r) && net_find(r.out, token) != NULL;
        net_run_free(&r);
    }
    net_assert(&u, "ran", ran,
               "the browser acceptance case did not complete; see browser evidence");
    snprintf(command, sizeof(command),
             "python3 /tmp/graphics/browser/judge.py %s %s /tmp/graphics/browser/ref "
             "/tmp/browser-out",
             browser, mode);
    if (net_step(&u, "judge") == ETUDE_OK &&
        net_guest(&u, b.m, argv, NULL, NULL, "judge", &r) == ETUDE_OK) {
        judged = net_exited0(&r) && net_find(r.out, "browser-judge: result: pass") != NULL;
        fputs(r.out, stdout);
        fputs(r.err, stderr);
        net_run_free(&r);
    }
    net_assert(&u, "as-debian", judged,
               "the browser acceptance differs from Debian; see judge evidence");
    if (net_step(&u, "collect") == ETUDE_OK) {
        argv[2] =
            "cd /tmp/browser-out && "
            "for f in result.json switches.json sound.json record *.pages *.plays *.decoders; "
            "do if [ -f \"$f\" ]; then echo \"browser-evidence: $f\"; cat \"$f\"; fi; done; "
            "tar -czf /tmp/browser-out.tar.gz -C /tmp/browser-out .";
        if (net_guest(&u, b.m, argv, NULL, NULL, "pack", &r) == ETUDE_OK && net_exited0(&r)) {
            fputs(r.out, stdout);
            if (etude_guest_get(b.m, "/tmp/browser-out.tar.gz", "browser-out.tar.gz") != ETUDE_OK)
                net_fail(&u, "the browser output could not be fetched");
        }
        net_run_free(&r);
    }
    return net_end(&u);
}
