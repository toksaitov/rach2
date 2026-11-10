/* Run the nested assignment cases on private GNU-root overlays. */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/* The records of the boot the cases wait for: the root mounted and the
 * login prompt. */
#define ROOT_MOUNTED "ext2: root:"
#define LOGIN_PROMPT "rach2 login"

/* The suite's files on the machine, copied there from the data disk, the
 * second of the machine's disks, which the guest mounts read-only. */
#define KVM_DIR  "/var/tmp/assign"
#define KVM_DATA "/tmp/assign-data"

/* One assignment case and its console login. */
struct assign_case {
    struct net_case u;
    struct net_boot b;
    char mode[32];
    char password[128];
};

/** Run `command` as root under sh through the guest channel, keeping what
 * it prints as evidence `output`. */
static enum etude_status as_root(struct assign_case *s, const char *command, const char *output,
                                 struct net_run *r)
{
    const char *argv[] = {"/bin/sh", "-c", command, NULL};

    return net_guest(&s->u, s->b.m, argv, NULL, NULL, output, r);
}

/** Boot the machine, log in as root at the console and open the channel. */
static void up(struct assign_case *s)
{
    if (net_step(&s->u, "boot") == ETUDE_OK)
        net_boot(&s->b, &s->u, "gnu", 1, NULL);
    net_seen(&s->b, ROOT_MOUNTED);
    net_seen(&s->b, LOGIN_PROMPT);
    if (net_login(&s->b, "root", s->password, "# ") == ETUDE_OK &&
        etude_guest_open(s->b.m) != ETUDE_OK)
        net_fail(&s->u, "the GNU guest command channel did not open");
}

/** Copy the runners, the lists and the guests from the data disk to
 * KVM_DIR. */
static void stage(struct assign_case *s)
{
    char line[160];
    struct net_run r;

    if (net_step(&s->u, "stage") != ETUDE_OK)
        return;
    if (as_root(s,
                "mkdir -p " KVM_DATA " " KVM_DIR " && "
                "for d in /dev/disk[0-7]; do "
                "mount -t ext2 -o ro $d " KVM_DATA " 2>/dev/null || continue; "
                "if [ -f " KVM_DATA "/assign/run.sh ]; then "
                "cp -R " KVM_DATA "/. " KVM_DIR "; umount " KVM_DATA "; exit 0; fi; "
                "umount " KVM_DATA "; done; exit 1",
                "copy", &r) != ETUDE_OK)
        return;
    if (!net_exited0(&r))
        net_fail(&s->u, "the assignment payload could not be copied: %s",
                 net_copy_line(r.err != NULL ? r.err : r.out, line, sizeof(line)));
    net_run_free(&r);
}

/**
 * Run the case's runner, assert its result line, the one assertion of the
 * case, and print what it printed for the run's log.
 */
static void run(struct assign_case *s)
{
    char command[256];
    struct net_run r;

    snprintf(command, sizeof(command), "cd " KVM_DIR "/assign && TMPDIR=" KVM_DIR " bash run.sh %s",
             s->mode);
    if (net_step(&s->u, "run") != ETUDE_OK || as_root(s, command, "runner", &r) != ETUDE_OK)
        return;
    if (r.out != NULL)
        fputs(r.out, stdout);
    if (r.err != NULL)
        fputs(r.err, stderr);
    net_assert(&s->u, "assignment",
               net_exited0(&r) && net_find(r.out, "assign: result: pass:") != NULL,
               "assignment case %s", s->mode);
    net_run_free(&r);
}

/** Run the assignment case named by its manifest on a private GNU root. */
int etude_scenario(struct etude_case *c)
{
    struct assign_case s;
    const char *name;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    name = strrchr(etude_case_id(c), '.');
    if (name == NULL || strlen(name + 1) >= sizeof(s.mode))
        net_fail(&s.u, "invalid assignment identity");
    else
        strcpy(s.mode, name + 1);
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    up(&s);
    stage(&s);
    run(&s);
    return net_end(&s.u);
}
