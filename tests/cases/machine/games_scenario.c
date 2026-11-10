/* Run a row of the Wine probes as the desktop account on the GNU root. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/**
 * Print once each call the kernel lacks that the boot's console names, as
 * `rach2.trace=enosys` makes the kernel log it, with the program that asked;
 * the console of a passed case is not kept, and the case's output is.
 */
static void print_missing(const struct net_boot *b)
{
    char *text = net_file(b->console), *seen = NULL, *grown, line[256];
    const char *p = text, *call;
    size_t used = 0, len;

    while ((p = net_find(p, "syscall: *not implemented*asked for by")) != NULL) {
        net_copy_line(p, line, sizeof(line));
        p += strcspn(p, "\r\n");
        call = strstr(line, "syscall: ");
        len = strlen(call);
        if (seen != NULL && strstr(seen, call) != NULL)
            continue;
        grown = realloc(seen, used + len + 2);
        if (grown == NULL)
            break;
        seen = grown;
        memcpy(seen + used, call, len);
        used += len;
        seen[used++] = '\n';
        seen[used] = '\0';
        printf("games: enosys: %s\n", call);
    }
    free(seen);
    free(text);
}

/**
 * Boot the GNU root on virgl, mount the probes' disk and run the row the
 * case's identity names; require its exit and its complete result token.
 */
int etude_scenario(struct etude_case *c)
{
    struct net_case u;
    struct net_boot b;
    struct net_run r;
    char password[128], command[1024], marker[96];
    const char *id = etude_case_id(c), *row = strrchr(id, '.');
    const char *argv[] = {"/bin/bash", "-c", command, NULL};
    int matched = 0;

    net_case(&u, c);
    memset(&b, 0, sizeof(b));
    b.u = &u;
    row = row != NULL ? row + 1 : id;
    net_secret(&u, "demo-password", password, sizeof(password));
    if (net_step(&u, "boot") == ETUDE_OK)
        net_boot(&b, &u, "gnu", 1, NULL);
    net_seen(&b, "ext2: root: /dev/disk0p1");
    net_seen(&b, "rach2 login");
    if (net_login(&b, "root", password, "# ") == ETUDE_OK && etude_guest_open(b.m) != ETUDE_OK)
        net_fail(&u, "the GNU guest command channel did not open");
    /* The desktop account is in the render group the render node needs. */
    snprintf(command, sizeof(command),
             "mkdir -p /tmp/games /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix && "
             "mount -t ext2 -o ro /dev/disk1 /tmp/games && "
             "su -s /bin/bash desktop -c 'python3 /tmp/games/run.py %s /tmp/games-results'",
             row);
    if (net_step(&u, "row") == ETUDE_OK &&
        net_guest(&u, b.m, argv, NULL, NULL, "games", &r) == ETUDE_OK) {
        fputs(r.out, stdout);
        fputs(r.err, stderr);
        snprintf(marker, sizeof(marker), "GAMESROW OK %s", row);
        matched = net_exited0(&r) && net_find(r.out, marker) != NULL;
        net_assert(&u, "row", matched, "the Wine row did not match Linux; see games evidence");
        net_run_free(&r);
        print_missing(&b);
    } else
        net_assert(&u, "row", 0, "the Wine row could not run");
    return net_end(&u);
}
