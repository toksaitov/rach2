/*
 * bed_vtest_scenario.c: the cases of the bed-vtest suite
 * (machine.bed-vtest.json). Each boots an overlay of the x86-64 GNU root
 * with the suite's data disk and runs bed/fixture/vtest-rach2.sh there,
 * which draws a triangle with Mesa's virpipe through vtestrelay to the
 * vtest server guest that the root's QEMU boots.
 */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/* The records of the boot the cases wait for: the root mounted and the
 * login prompt. */
#define ROOT_MOUNTED "ext2: root: /dev/disk0p1"
#define LOGIN_PROMPT "rach2 login"

/* The data disk, the second of the machine's disks, and where the guest
 * mounts it read-only; the runner reads the guest's images in place. */
#define BED_DISK "/dev/disk1"
#define BED_DATA "/tmp/bed-vtest"

/* The runner's last line on a pass. */
#define PASSED "vtest-rach2: result: pass"

/* What one attempt shares among its steps. */
struct bed_case {
    struct net_case u;
    struct net_boot b;
    char accel[8];      /* the vtest guest's accelerator inside Rach2 */
    char password[128]; /* root's, at the console */
};

/**
 * Read the case from the last part of its identity: `triangle`, the guest
 * under QEMU's TCG, or `triangle-kvm`, the guest under Rach2's KVM. Give 0
 * for an identity of no such form.
 */
static int parse(struct bed_case *s, const char *id)
{
    const char *name = strrchr(id, '.');

    if (name == NULL)
        return 0;
    name++;
    if (strcmp(name, "triangle") == 0)
        strcpy(s->accel, "tcg");
    else if (strcmp(name, "triangle-kvm") == 0)
        strcpy(s->accel, "kvm");
    else
        return 0;
    return 1;
}

/** Run `command` as root under sh through the guest channel, keeping what
 * it prints as evidence `output`. */
static enum etude_status as_root(struct bed_case *s, const char *command, const char *output,
                                 struct net_run *r)
{
    const char *argv[] = {"/bin/sh", "-c", command, NULL};

    return net_guest(&s->u, s->b.m, argv, NULL, NULL, output, r);
}

/** Boot the machine, log in as root at the console and open the channel. */
static void up(struct bed_case *s)
{
    if (net_step(&s->u, "boot") == ETUDE_OK)
        net_boot(&s->b, &s->u, "gnu", 1, NULL);
    net_seen(&s->b, ROOT_MOUNTED);
    net_seen(&s->b, LOGIN_PROMPT);
    if (net_login(&s->b, "root", s->password, "# ") == ETUDE_OK &&
        etude_guest_open(s->b.m) != ETUDE_OK)
        net_fail(&s->u, "the GNU guest command channel did not open");
}

/** Mount the data disk read-only at BED_DATA. */
static void mount_data(struct bed_case *s)
{
    char line[160];
    struct net_run r;

    if (net_step(&s->u, "mount") != ETUDE_OK)
        return;
    if (as_root(s,
                "rm -rf " BED_DATA " && mkdir -p " BED_DATA " && mount -t ext2 -o ro " BED_DISK
                " " BED_DATA,
                "mount", &r) != ETUDE_OK)
        return;
    if (!net_exited0(&r))
        net_fail(
            &s->u, "the bed-vtest data disk did not mount: %s",
            net_copy_line(r.err != NULL && r.err[0] != '\0' ? r.err : r.out, line, sizeof(line)));
    net_run_free(&r);
}

/**
 * Run vtest-rach2.sh under the case's accelerator, assert its result line,
 * the one assertion of the case, and print what it printed for the run's
 * log.
 */
static void run(struct bed_case *s)
{
    char command[256], line[200];
    const char *result;
    struct net_run r;

    snprintf(command, sizeof(command), "sh " BED_DATA "/vtest-rach2.sh -a %s " BED_DATA, s->accel);
    if (net_step(&s->u, "run") == ETUDE_OK)
        as_root(s, command, "runner", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "pixels", 0, "");
        return;
    }
    result = net_find(r.out, "vtest-rach2: result: ");
    net_assert(&s->u, "pixels", net_exited0(&r) && net_find(r.out, PASSED) != NULL, "%s",
               net_copy_line(result != NULL ? result : (r.err != NULL ? r.err : r.out), line,
                             sizeof(line)));
    if (r.out != NULL)
        fputs(r.out, stdout);
    net_run_free(&r);
}

int etude_scenario(struct etude_case *c)
{
    struct bed_case s;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    if (!parse(&s, etude_case_id(c)))
        net_fail(&s.u, "the case's identity names no bed-vtest case");
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    up(&s);
    mount_data(&s);
    run(&s);
    return net_end(&s.u);
}
