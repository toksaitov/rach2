/*
 * bed_shm_scenario.c: the case of the bed-shm suite (machine.bed-shm.json).
 * It boots an overlay of the GNU root with the suite's data disk and the test
 * bed's VM disk and runs tests/kvm/bed-shm.sh on it, which attaches a region
 * to /dev/vgpu-shm, boots the VM under Rach2's KVM on it, draws through
 * virtio-gpu over the region, runs piglit's readbacks there and kills the
 * VM, or, for the case `nocopy`,
 * maps the VM's Vulkan buffers into a program of Rach2's and kills the VM
 * with one mapping live, or, for the case `vm-death`, runs the desktop
 * session on the VM's GPU and kills the VM under glmark2, or, for the case
 * `console`, first has the console lose the host's own card, so that it
 * takes the region's card inside the bind, and then draws; each line the
 * runner prints for a check is the case's assertion of that name.
 */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/* The records of the boot the case waits for: the root mounted and the
 * login prompt. */
#define ROOT_MOUNTED "ext2: root: /dev/disk0p1"
#define LOGIN_PROMPT "rach2 login"

/* The suite's files on the machine, copied there from the data disk, the
 * second of the machine's disks, which the guest mounts read-only; the VM's
 * disk is the third. */
#define BED_DIR  "/tmp/bed-shm-run"
#define BED_DISK "/dev/disk1"
#define BED_DATA "/tmp/bed-shm-data"
#define VM_DISK  "/dev/disk2"

/* What one attempt shares among its steps. */
struct bed_case {
    struct net_case u;
    struct net_boot b;
    char password[128]; /* root's, at the console */
};

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

/** Copy the runner, the probe, nocopy.c and piglit's runner and list from
 * the data disk to BED_DIR. */
static void stage(struct bed_case *s)
{
    char line[160];
    struct net_run r;

    if (net_step(&s->u, "stage") != ETUDE_OK)
        return;
    if (as_root(s,
                "rm -rf " BED_DIR " " BED_DATA " && mkdir -p " BED_DIR " " BED_DATA
                " && mount -t ext2 -o ro " BED_DISK " " BED_DATA " && cp -R " BED_DATA "/. " BED_DIR
                " && umount " BED_DATA " && rmdir " BED_DATA " " BED_DIR "/lost+found",
                "copy", &r) != ETUDE_OK)
        return;
    if (!net_exited0(&r))
        net_fail(
            &s->u, "the bed-shm data disk could not be copied: %s",
            net_copy_line(r.err != NULL && r.err[0] != '\0' ? r.err : r.out, line, sizeof(line)));
    net_run_free(&r);
}

/* The runs of bed-shm.sh, by the case's last word. */
enum bed_run { RUN_VIRGL, RUN_NOCOPY, RUN_DESKTOP, RUN_CONSOLE };

/**
 * Run bed-shm.sh and assert each of its checks from the line it printed,
 * `bed-shm: NAME: pass` or `fail`, then print what it printed for the run's
 * log: the checks of the virgl draw and readbacks, with `nocopy` those of
 * -k, with `vm-death` those of -w, or with `console` those of -u.
 */
static void run(struct bed_case *s, enum bed_run which)
{
    static const char *const virgl[] = {"attach", "draw", "doorbell", "readback", "death", NULL};
    static const char *const mapped[] = {"attach", "build",   "pages",  "coherent", "release",
                                         "refuse", "measure", "revoke", "leak",     NULL};
    static const char *const desktop[] = {"attach", "session", "load", "death", "fallback", NULL};
    static const char *const console[] = {"unplug",   "attach", "console", "draw",
                                          "doorbell", "death",  NULL};
    static const char *const commands[] = {"cd " BED_DIR " && sh bed-shm.sh -d " VM_DISK
                                           " -p " BED_DIR "/vgshmprobe -o " BED_DIR "/out",
                                           "cd " BED_DIR " && sh bed-shm.sh -d " VM_DISK
                                           " -p " BED_DIR "/vgshmprobe -k " BED_DIR
                                           "/nocopy.c -o " BED_DIR "/out",
                                           "cd " BED_DIR " && sh bed-shm.sh -d " VM_DISK
                                           " -p " BED_DIR "/vgshmprobe -w -o " BED_DIR "/out",
                                           "cd " BED_DIR " && sh bed-shm.sh -d " VM_DISK
                                           " -p " BED_DIR "/vgshmprobe -u -o " BED_DIR "/out"};
    const char *const *check = which == RUN_NOCOPY    ? mapped
                               : which == RUN_DESKTOP ? desktop
                               : which == RUN_CONSOLE ? console
                                                      : virgl;
    char want[64], line[200];
    const char *found;
    struct net_run r;
    size_t i;
    int ran;

    memset(&r, 0, sizeof(r));
    ran =
        net_step(&s->u, "run") == ETUDE_OK && as_root(s, commands[which], "runner", &r) == ETUDE_OK;
    for (i = 0; check[i] != NULL; i++) {
        if (!ran || !net_ok(&s->u)) {
            net_assert(&s->u, check[i], 0, "");
            continue;
        }
        snprintf(want, sizeof(want), "bed-shm: %s: ", check[i]);
        found = net_find(r.out, want);
        snprintf(want, sizeof(want), "bed-shm: %s: pass", check[i]);
        net_assert(&s->u, check[i], found != NULL && strncmp(found, want, strlen(want)) == 0, "%s",
                   net_copy_line(found != NULL ? found : (r.err != NULL ? r.err : r.out), line,
                                 sizeof(line)));
    }
    if (r.out != NULL)
        fputs(r.out, stdout);
    net_run_free(&r);
}

int etude_scenario(struct etude_case *c)
{
    struct bed_case s;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    up(&s);
    stage(&s);
    if (strcmp(etude_case_id(c), "machine.bed-shm.nocopy") == 0)
        run(&s, RUN_NOCOPY);
    else if (strcmp(etude_case_id(c), "machine.bed-shm.vm-death") == 0)
        run(&s, RUN_DESKTOP);
    else if (strcmp(etude_case_id(c), "machine.bed-shm.console") == 0)
        run(&s, RUN_CONSOLE);
    else
        run(&s, RUN_VIRGL);
    return net_end(&s.u);
}
