/*
 * kvm_scenario.c: the cases of the KVM suite (machine.kvm.json). Each boots
 * an overlay of the GNU root with the suite's data disk and runs one runner
 * of tests/kvm on it: kvm-boot.sh, which boots a guest in the root's QEMU to
 * its token, or corpus.sh, which runs kvm-unit-tests or the KVM selftests
 * under Rach2's KVM as their lists say.
 */
#include <stdio.h>
#include <string.h>

#include "netcase.h"

/* The records of the boot the cases wait for: the root mounted and the
 * login prompt. */
#define ROOT_MOUNTED "ext2: root: /dev/disk0p1"
#define LOGIN_PROMPT "rach2 login"

/* The suite's files on the machine, copied there from the data disk, the
 * second of the machine's disks, which the guest mounts read-only. */
#define KVM_DIR  "/tmp/kvm"
#define KVM_DISK "/dev/disk1"
#define KVM_DATA "/tmp/kvm-data"

/* The runners, by the last part of the case's identity. */
enum runner { BOOT, UNIT, SELFTESTS, ENTRY, SLOTS, LEAK };

/* What one attempt shares among its steps. */
struct kvm_case {
    struct net_case u;
    struct net_boot b;
    enum runner runner;
    int rach2;          /* boot the i686 Rach2 guest instead of Debian */
    char accel[8];      /* a boot case's accelerator inside Rach2 */
    char password[128]; /* root's, at the console */
};

/**
 * Read the case from the last part of its identity: `boot-debian-ACCEL`,
 * the Debian guest under ACCEL, `kvm` or `tcg`, or `unit-tests` and
 * `selftests`, each with an optional `-nested`, the corpus on the processor
 * profile of its machine. Give 0 for an identity of no such form.
 */
static int parse(struct kvm_case *s, const char *id)
{
    const char *name = strrchr(id, '.');

    if (name == NULL)
        return 0;
    name++;
    if (strcmp(name, "leak") == 0) {
        s->runner = LEAK;
        return 1;
    }
    if (strcmp(name, "entry") == 0 || strcmp(name, "slots") == 0) {
        s->runner = strcmp(name, "slots") == 0 ? SLOTS : ENTRY;
        return 1;
    }
    if (strcmp(name, "boot-debian-kvm") == 0 || strcmp(name, "boot-debian-tcg") == 0 ||
        strcmp(name, "boot-debian-nested") == 0 || strcmp(name, "boot-rach2-kvm") == 0 ||
        strcmp(name, "boot-rach2-nested") == 0) {
        s->runner = BOOT;
        s->rach2 = strncmp(name, "boot-rach2-", strlen("boot-rach2-")) == 0;
        strcpy(s->accel, strcmp(name, "boot-debian-tcg") == 0 ? "tcg" : "kvm");
        return 1;
    }
    if (strcmp(name, "unit-tests") == 0 || strcmp(name, "unit-tests-nested") == 0) {
        s->runner = UNIT;
        return 1;
    }
    if (strcmp(name, "selftests") == 0 || strcmp(name, "selftests-nested") == 0) {
        s->runner = SELFTESTS;
        return 1;
    }
    return 0;
}

/** Run `command` as root under sh through the guest channel, keeping what
 * it prints as evidence `output`. */
static enum etude_status as_root(struct kvm_case *s, const char *command, const char *output,
                                 struct net_run *r)
{
    const char *argv[] = {"/bin/sh", "-c", command, NULL};

    return net_guest(&s->u, s->b.m, argv, NULL, NULL, output, r);
}

/** Boot the machine, log in as root at the console and open the channel. */
static void up(struct kvm_case *s)
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
static void stage(struct kvm_case *s)
{
    char line[160];
    struct net_run r;

    if (net_step(&s->u, "stage") != ETUDE_OK)
        return;
    if (as_root(s,
                "rm -rf " KVM_DIR " " KVM_DATA " && mkdir -p " KVM_DIR " " KVM_DATA
                " && mount -t ext2 -o ro " KVM_DISK " " KVM_DATA " && cp -R " KVM_DATA "/. " KVM_DIR
                " && umount " KVM_DATA " && rmdir " KVM_DATA " " KVM_DIR "/lost+found",
                "copy", &r) != ETUDE_OK)
        return;
    if (!net_exited0(&r))
        net_fail(
            &s->u, "the KVM data disk could not be copied: %s",
            net_copy_line(r.err != NULL && r.err[0] != '\0' ? r.err : r.out, line, sizeof(line)));
    net_run_free(&r);
}

/**
 * Run the case's runner, assert its result line, the one assertion of the
 * case, and print what it printed for the run's log.
 */
static void run(struct kvm_case *s)
{
    static const char *const assertion[] = {"token", "as-listed", "as-listed",
                                            "entry", "slots",     "leak"};
    static const char *const pattern[] = {"kvm-boot: result: pass",
                                          "kvm-corpus: result: pass",
                                          "kvm-corpus: result: pass",
                                          "SVMENTRY OK",
                                          "KVMOBJECT OK",
                                          "kvm-leak: result: pass"};
    char command[512], line[200];
    const char *result;
    struct net_run r;

    if (s->runner == LEAK)
        snprintf(command, sizeof(command),
                 "cd " KVM_DIR " && sh leak.sh"
                 " -k guests/debian/vmlinuz -i guests/debian/initrd");
    else if (s->runner == ENTRY || s->runner == SLOTS)
        snprintf(command, sizeof(command), KVM_DIR "/svm-entry%s",
                 s->runner == SLOTS ? " slots" : "");
    else if (s->runner == BOOT)
        snprintf(command, sizeof(command),
                 "cd " KVM_DIR " && sh kvm-boot.sh -a %s -o " KVM_DIR "/boot.log"
                 " -k %s -i %s%s -t '%s'",
                 s->accel, s->rach2 ? "guests/rach2/kernel" : "guests/debian/vmlinuz",
                 s->rach2 ? "guests/rach2/ram.krffs" : "guests/debian/initrd",
                 s->rach2 ? " -l rach2.tscreliable" : "", s->rach2 ? "KVMRACH2 OK" : "KVMGUEST OK");
    else
        snprintf(command, sizeof(command), "cd " KVM_DIR " && sh corpus.sh %s %s",
                 s->runner == UNIT ? "unit" : "selftests",
                 s->runner == UNIT ? "unit-tests.list" : "selftests.list");
    if (net_step(&s->u, "run") == ETUDE_OK)
        as_root(s, command, "runner", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, assertion[s->runner], 0, "");
        return;
    }
    result = net_find(r.out, s->runner >= ENTRY  ? pattern[s->runner]
                             : s->runner == BOOT ? "kvm-boot: result: "
                                                 : "kvm-corpus: result: ");
    net_assert(&s->u, assertion[s->runner],
               net_exited0(&r) && net_find(r.out, pattern[s->runner]) != NULL, "%s",
               net_copy_line(result != NULL ? result : (r.err != NULL ? r.err : r.out), line,
                             sizeof(line)));
    if (r.out != NULL)
        fputs(r.out, stdout);
    net_run_free(&r);
}

int etude_scenario(struct etude_case *c)
{
    struct kvm_case s;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    if (!parse(&s, etude_case_id(c)))
        net_fail(&s.u, "the case's identity names no runner's case");
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    up(&s);
    stage(&s);
    run(&s);
    return net_end(&s.u);
}
