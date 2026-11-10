/*
 * bedforward_scenario.c: the cases of the forward direction
 * (machine.bed-forward.json). Each boots an overlay of the GNU root on a
 * display that gives Rach2 a GPU, with the family's data disk and the Debian
 * guest's disk, and runs bed/forward/forward.sh, which boots that guest in
 * the root's QEMU and draws a scene in it through QEMU's virtio-gpu on
 * Rach2's render node.
 */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/* The records of the boot the cases wait for: the root mounted and the
 * login prompt. */
#define ROOT_MOUNTED "ext2: root: /dev/disk0p1"
#define LOGIN_PROMPT "rach2 login"

/* The family's files on the machine, copied there from the data disk, the
 * second of the machine's disks, which the guest mounts read-only; the
 * Debian guest's disk is the third, which QEMU on Rach2 opens whole. */
#define BED_DIR    "/tmp/bed"
#define BED_DISK   "/dev/disk1"
#define BED_DATA   "/tmp/bed-data"
#define GUEST_DISK "/dev/disk2"

/* What one attempt shares among its steps. */
struct forward_case {
    struct net_case u;
    struct net_boot b;
    const char *profile; /* scene.sh's, the last part of the identity */
    char password[128];  /* root's, at the console */
};

/** Run `command` as root under sh through the guest channel, keeping what
 * it prints as evidence `output`. */
static enum etude_status as_root(struct forward_case *s, const char *command, const char *output,
                                 struct net_run *r)
{
    const char *argv[] = {"/bin/sh", "-c", command, NULL};

    return net_guest(&s->u, s->b.m, argv, NULL, NULL, output, r);
}

/** Boot the machine, log in as root at the console and open the channel. */
static void up(struct forward_case *s)
{
    if (net_step(&s->u, "boot") == ETUDE_OK)
        net_boot(&s->b, &s->u, "gnu", 1, NULL);
    net_seen(&s->b, ROOT_MOUNTED);
    net_seen(&s->b, LOGIN_PROMPT);
    if (net_login(&s->b, "root", s->password, "# ") == ETUDE_OK &&
        etude_guest_open(s->b.m) != ETUDE_OK)
        net_fail(&s->u, "the GNU guest command channel did not open");
}

/** Copy the runner, the guest's key and the scenes from the data disk to
 * BED_DIR, the key readable by root alone, as ssh asks. */
static void stage(struct forward_case *s)
{
    char line[160];
    struct net_run r;

    if (net_step(&s->u, "stage") != ETUDE_OK)
        return;
    if (as_root(s,
                "rm -rf " BED_DIR " " BED_DATA " && mkdir -p " BED_DIR " " BED_DATA
                " && mount -t ext2 -o ro " BED_DISK " " BED_DATA " && cp -R " BED_DATA "/. " BED_DIR
                " && umount " BED_DATA " && rmdir " BED_DATA " " BED_DIR "/lost+found"
                " && chmod 600 " BED_DIR "/id_ed25519",
                "copy", &r) != ETUDE_OK)
        return;
    if (!net_exited0(&r))
        net_fail(
            &s->u, "the forward data disk could not be copied: %s",
            net_copy_line(r.err != NULL && r.err[0] != '\0' ? r.err : r.out, line, sizeof(line)));
    net_run_free(&r);
}

/**
 * Run forward.sh on the case's profile, assert its result line, the one
 * assertion of the case, and print what it printed for the run's log. The
 * Debian guest runs in QEMU's emulator, `-a tcg`: the cases' machine is
 * itself a guest of the testing host's KVM, and a guest of Rach2's KVM there
 * is one of the third level, which does not reach Debian's login
 * (bed/forward/Readme.md).
 */
static void run(struct forward_case *s)
{
    char command[512], line[240];
    const char *result;
    struct net_run r;

    snprintf(command, sizeof(command),
             "cd " BED_DIR " && sh forward.sh -a tcg -d " GUEST_DISK " -k id_ed25519 -g guest.tar"
             " -o " BED_DIR "/out %s",
             s->profile);
    if (net_step(&s->u, "run") == ETUDE_OK)
        as_root(s, command, "runner", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "drawn", 0, "");
        return;
    }
    result = net_find(r.out, "bed-forward: result: ");
    net_assert(&s->u, "drawn",
               net_exited0(&r) && net_find(r.out, "bed-forward: result: pass: ") != NULL, "%s",
               net_copy_line(result != NULL ? result : (r.err != NULL ? r.err : r.out), line,
                             sizeof(line)));
    if (r.out != NULL)
        fputs(r.out, stdout);
    net_run_free(&r);
}

/** Fetch the runner's output, the consoles and the scene's logs, as the
 * evidence out.tar.gz, whether or not the scene passed. */
static void collect(struct forward_case *s)
{
    struct net_run r;

    if (net_step(&s->u, "collect") != ETUDE_OK)
        return;
    if (as_root(s, "tar -czf " BED_DIR "/out.tar.gz -C " BED_DIR " out", "pack", &r) != ETUDE_OK)
        return;
    if (!net_exited0(&r) ||
        etude_guest_get(s->b.m, BED_DIR "/out.tar.gz", "out.tar.gz") != ETUDE_OK)
        net_fail(&s->u, "the runner's output could not be fetched");
    net_run_free(&r);
}

int etude_scenario(struct etude_case *c)
{
    struct forward_case s;
    const char *name = strrchr(etude_case_id(c), '.');

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    s.profile = name != NULL ? name + 1 : "";
    if (strcmp(s.profile, "virgl") != 0 && strcmp(s.profile, "venus") != 0)
        net_fail(&s.u, "the case's identity names no profile of forward.sh");
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    up(&s);
    stage(&s);
    run(&s);
    collect(&s);
    return net_end(&s.u);
}
