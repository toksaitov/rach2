/* Run the netguest cases: the GNU root gives its card to a gateway guest. */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/* The records of the boot the cases wait for: the root mounted and the
 * login prompt. */
#define ROOT_MOUNTED "ext2: root:"
#define LOGIN_PROMPT "rach2 login"

/* Where the guest mounts the payload's read-only data disk, the second of
 * the machine's disks, and keeps each case's scratch. */
#define PAYLOAD "/mnt/netguest"
#define SCRATCH "/var/tmp/netguest"

/* The card the cases give away, behind the PCIe root port at 00:06.0, the
 * address its interface takes, and the peer's address on its network: the
 * host, where the peer service listens on 127.0.0.1. */
#define CARD     "0000:01:00.0"
#define CARD_OWN "10.0.3.15/24"
#define PEER     "10.0.3.2"

/* One netguest case and its console login. */
struct netguest_case {
    struct net_case u;
    struct net_boot b;
    struct etude_case_service *peer;
    char name[32];
    char port[12];
    char password[128];
};

/** Run `command` as root under sh through the guest channel, keeping what
 * it prints as evidence `output`. */
static enum etude_status as_root(struct netguest_case *s, const char *command, const char *output,
                                 struct net_run *r)
{
    const char *argv[] = {"/bin/sh", "-c", command, NULL};

    return net_guest(&s->u, s->b.m, argv, NULL, NULL, output, r);
}

/** Start the peer on the case's port, boot the machine, log in as root at
 * the console and open the channel. */
static void up(struct netguest_case *s)
{
    struct etude_termination end;
    char *args[] = {"-s", "-p", s->port, NULL};

    if (net_step(&s->u, "boot") != ETUDE_OK)
        return;
    snprintf(s->port, sizeof(s->port), "%u", etude_port(s->u.c, 0));
    if (etude_service(s->u.c, "peer", args, &s->peer, &end) != ETUDE_OK) {
        net_fail(&s->u, "the nettput peer did not become ready on port %s", s->port);
        return;
    }
    net_boot(&s->b, &s->u, "gnu", 1, NULL);
    net_seen(&s->b, ROOT_MOUNTED);
    net_seen(&s->b, LOGIN_PROMPT);
    if (net_login(&s->b, "root", s->password, "# ") == ETUDE_OK &&
        etude_guest_open(s->b.m) != ETUDE_OK)
        net_fail(&s->u, "the GNU guest command channel did not open");
}

/** Mount the data disk that holds run.sh at PAYLOAD, read-only. */
static void stage(struct netguest_case *s)
{
    char line[160];
    struct net_run r;

    if (net_step(&s->u, "stage") != ETUDE_OK)
        return;
    if (as_root(s,
                "mkdir -p " PAYLOAD " " SCRATCH " && "
                "for d in /dev/disk[0-7]; do "
                "mount -t ext2 -o ro $d " PAYLOAD " 2>/dev/null || continue; "
                "[ -f " PAYLOAD "/run.sh ] && exit 0; "
                "umount " PAYLOAD "; done; exit 1",
                "mount", &r) != ETUDE_OK)
        return;
    if (!net_exited0(&r))
        net_fail(&s->u, "the netguest payload could not be mounted: %s",
                 net_copy_line(r.err != NULL ? r.err : r.out, line, sizeof(line)));
    net_run_free(&r);
}

/**
 * Run the case with run.sh, assert its result line, the one assertion of the
 * case, and print what it printed for the run's log.
 */
static void run(struct netguest_case *s)
{
    char command[384];
    struct net_run r;

    snprintf(command, sizeof(command),
             "TMPDIR=" SCRATCH " sh " PAYLOAD "/run.sh -c " CARD " -a " CARD_OWN " " PEER " %s %s",
             s->port, s->name);
    if (net_step(&s->u, "run") != ETUDE_OK || as_root(s, command, "runner", &r) != ETUDE_OK)
        return;
    if (r.out != NULL)
        fputs(r.out, stdout);
    if (r.err != NULL)
        fputs(r.err, stderr);
    net_assert(&s->u, "netguest",
               net_exited0(&r) && net_find(r.out, "netguest: result: pass:") != NULL,
               "netguest case %s", s->name);
    net_run_free(&r);
}

/** Run the netguest case named by its manifest on a private GNU root. */
int etude_scenario(struct etude_case *c)
{
    struct netguest_case s;
    struct etude_termination end;
    const char *name;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    name = strrchr(etude_case_id(c), '.');
    if (name == NULL || strlen(name + 1) >= sizeof(s.name))
        net_fail(&s.u, "invalid netguest identity");
    else
        strcpy(s.name, name + 1);
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    up(&s);
    stage(&s);
    run(&s);
    if (s.peer != NULL)
        (void)etude_service_stop(s.peer, &end);
    return net_end(&s.u);
}
