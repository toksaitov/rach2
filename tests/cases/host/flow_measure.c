/* flow_measure.c: machine identity, measurement windows and SSH cleanup. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include "sha256.h"
#include <dirent.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *ev(const char *n)
{
    const char *v = getenv(n);
    return v ? v : "";
}
static const char *mp(const char *name)
{
    static char b[32][4096];
    static unsigned i;
    char *v = b[i++ % 32];
    snprintf(v, 4096, "%s/%s", ev("CASE"), name);
    return v;
}
static void record(const char *text)
{
    FILE *f = fopen(mp("commands"), "ab");
    if (f) {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}
static void known(int argc, char **argv)
{
    const char *prefix = "UserKnownHostsFile=";
    int i;
    for (i = 1; i < argc; i++)
        if (!strncmp(argv[i], prefix, strlen(prefix)) &&
            strcmp(argv[i] + strlen(prefix), "/dev/null"))
            fx_text(argv[i] + strlen(prefix), "dummy address and public key\n");
}
static int identity_mock(int argc, char **argv, const char *command)
{
    char text[1000], image[4096], *script;
    record("identity");
    known(argc, argv);
    snprintf(text, sizeof(text), "rach2.root=ext2%s%s\n", *ev("MACHINE_SLOT") ? " rach2.slot=" : "",
             ev("MACHINE_SLOT"));
    fx_text(mp("cmdline"), text);
    fx_text(mp("identity-command"), command);
    snprintf(image, sizeof(image), "%s/%s", ev("MACHINE"),
             *ev("MACHINE_IMAGE") ? ev("MACHINE_IMAGE") : "bootpart");
    if (!fx_replace(mp("identity-command"), mp("identity-command"), "/proc/cmdline",
                    mp("cmdline")) ||
        !fx_replace(mp("identity-command"), mp("identity-command"), "/dev/bootpart", image))
        return 1;
    script = fx_read(mp("identity-command"));
    {
        char *a[] = {"/bin/sh", "-c", script, NULL};
        execv(a[0], a);
    }
    free(script);
    return 127;
}
static int ssh_mock(int argc, char **argv)
{
    const char *command = argc > 1 ? argv[argc - 1] : "", *mode = ev("MODE");
    if (!strncmp(command, ": machine-identity", 18))
        return identity_mock(argc, argv, command);
    if (!strcmp(mode, "load") || !strcmp(mode, "stagger")) {
        if (!strncmp(command, "mktemp ", 7)) {
            record("mktemp");
            puts(mp("remote"));
            return 0;
        }
        record("session");
        if (!strcmp(mode, "stagger")) {
            char shell[40], arg[40], dash[40], session[40];
            FILE *f;
            if (sscanf(command, "%39s %39s %39s %39s", shell, arg, dash, session) != 4)
                return 1;
            f = fopen(mp("order"), "ab");
            if (!f)
                return 1;
            fprintf(f, "session %s\n", session);
            fclose(f);
            printf("SESSION %s builds 0 failed 0 before-deadline 0 late 0\n", session);
            return 0;
        }
        {
            char *a[] = {"/bin/sh", "-c", (char *)command, NULL};
            execv(a[0], a);
        }
        return 127;
    }
    if (!strncmp(command, "kill ", 5))
        return 0;
    record("receiver");
    known(argc, argv);
    puts("machine-net: receiver-pid: 1\nmachine-net: listening: port 27158");
    fflush(stdout);
    if (!strcmp(mode, "interrupt"))
        for (;;)
            pause();
    puts("machine-net: result: fixture transfer");
    return 0;
}
static int hash_mock(int argc, char **argv)
{
    struct sha256 hash;
    unsigned char b[8192], digest[32];
    char hex[65];
    FILE *f = stdin;
    size_t n;
    if (argc == 4 && !strcmp(argv[1], "-a") && !strcmp(argv[2], "256")) {
        f = fopen(argv[3], "rb");
        if (!f)
            return 1;
    } else if (argc != 1 && !(argc == 3 && !strcmp(argv[1], "-a") && !strcmp(argv[2], "256")))
        return 2;
    sha256_init(&hash);
    while ((n = fread(b, 1, sizeof(b), f)))
        sha256_update(&hash, b, n);
    if (ferror(f)) {
        if (f != stdin)
            fclose(f);
        return 1;
    }
    if (f != stdin)
        fclose(f);
    sha256_final(&hash, digest);
    sha256_hex(digest, hex);
    printf("%s  %s\n", hex, argc == 4 ? argv[3] : "-");
    return 0;
}
static int pace_mock(int argc, char **argv)
{
    for (;;) {
        char *s = fx_read(mp("order")), *line = s;
        unsigned starts = 0, pauses = 0;
        while (*line) {
            if (!strncmp(line, "session ", 8))
                starts++;
            if (!strncmp(line, "sleep ", 6))
                pauses++;
            line = strchr(line, '\n');
            if (!line)
                break;
            line++;
        }
        free(s);
        if (starts > pauses)
            break;
        fx_pause();
    }
    {
        FILE *f = fopen(mp("order"), "ab");
        int i;
        if (!f)
            return 1;
        fputs("sleep", f);
        for (i = 1; i < argc; i++)
            fprintf(f, " %s", argv[i]);
        fputc('\n', f);
        return fclose(f) ? 1 : 0;
    }
}
static int clock_mock(int argc, char **argv)
{
    char *s, *end, *rest;
    if (argc != 4 || strcmp(argv[1], "-d.") || strcmp(argv[2], "-f1") ||
        strcmp(argv[3], "/proc/uptime"))
        return 2;
    s = fx_read(mp("clock"));
    end = strchr(s, '\n');
    if (!end) {
        free(s);
        return 1;
    }
    *end = 0;
    puts(s);
    rest = strdup(end + 1);
    fx_text(mp("clock"), rest);
    free(rest);
    free(s);
    return 0;
}
static int bootctl_mock(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "status"))
        return 2;
    puts("bootctl: candidate: first sector 8, 8 sectors in 8 clusters, 3000 bytes\nbootctl: "
         "fallback: first sector 16, 8 sectors in 8 clusters, 3000 bytes\nbootctl: grubenv: "
         "first sector 24, 2 sectors in 2 clusters, 1024 bytes\nbootctl: saved_entry: fallback");
    printf("bootctl: next_entry: %s\nbootctl: booted: %s\n", ev("MACHINE_NEXT"),
           *ev("MACHINE_SLOT") ? ev("MACHINE_SLOT")
                               : "not from a slot (no rach2.slot= on /proc/cmdline)");
    return 0;
}
int flow_measure_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "ssh"))
        return ssh_mock(argc, argv);
    if (!strcmp(name, "sha256sum") || !strcmp(name, "shasum"))
        return hash_mock(argc, argv);
    if (!strcmp(name, "bootctl"))
        return bootctl_mock(argc, argv);
    if (!strcmp(name, "sender"))
        return fx_text(mp("sent"), "") ? (!strcmp(ev("MODE"), "failure") ? 3 : 0) : 1;
    if (!strcmp(name, "cp")) {
        if (argc != 4 || strcmp(argv[1], "-r") || strcmp(argv[2], "/src/rach2"))
            return 2;
        return fx_mkdir(argv[3]) ? 0 : 1;
    }
    if (!strcmp(name, "cut"))
        return clock_mock(argc, argv);
    if (!strcmp(name, "make")) {
        char *s, number[50];
        unsigned n;
        if (argc < 3 || strcmp(argv[1], "-f") || strcmp(argv[2], "guest.mk"))
            return 2;
        if (!strcmp(argv[argc - 1], "clean"))
            return 0;
        s = fx_read(mp("made"));
        n = (unsigned)atoi(s) + 1;
        free(s);
        snprintf(number, sizeof(number), "%u", n);
        fx_text(mp("made"), number);
        return n == 2 && !strcmp(ev("FAIL_BUILD"), "1") ? 7 : 0;
    }
    if (!strcmp(name, "sleep"))
        return pace_mock(argc, argv);
    return 97;
}
/*
 * The fixture's boot partition is a FAT32 image of 32 sectors of 512 bytes,
 * one sector a cluster: the boot sector, one FAT of 7 sectors, and the data
 * from sector 8, where cluster C is sector C + 6. The candidate slot is
 * sectors 8 to 15; the fallback slot is split, sectors 16 to 19 and then 28
 * to 31, as `bootctl fallback` can leave it, so that a read of its sectors
 * in a row hashes the zeros between. The grubenv is sectors 24 and 25.
 */
static const unsigned candidate_slot[] = {8, 9, 10, 11, 12, 13, 14, 15},
                      fallback_slot[] = {16, 17, 18, 19, 28, 29, 30, 31};
static void put32(unsigned char *p, unsigned long v)
{
    p[0] = v & 255;
    p[1] = v >> 8 & 255;
    p[2] = v >> 16 & 255;
    p[3] = v >> 24 & 255;
}
/** Link the clusters of the N sectors in AT, in order, in the image's FAT. */
static void chain(unsigned char *disk, const unsigned *at, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        put32(disk + 512 + 4 * (at[i] - 6), i + 1 < n ? at[i + 1] - 6 : 0x0FFFFFFFUL);
}
/** Write N copies of TEXT into the sectors in AT, in order, from the first. */
static void lay(unsigned char *disk, const unsigned *at, const char *text, unsigned n)
{
    size_t len = strlen(text), i;
    for (i = 0; i < n * len; i++)
        disk[at[i / 512] * 512 + i % 512] = text[i % len];
}
/**
 * Make the image with NC copies of C in the candidate slot and NF of F in the
 * fallback.
 */
static void image(unsigned char *disk, const char *c, unsigned nc, const char *f, unsigned nf)
{
    static const unsigned grubenv[] = {24, 25};
    memset(disk, 0, 16384);
    disk[12] = 2; /* 512 bytes a sector */
    disk[13] = 1; /* one sector a cluster */
    disk[14] = 1; /* one reserved sector */
    disk[16] = 1; /* one FAT */
    disk[36] = 7; /* 7 sectors a FAT */
    put32(disk + 512, 0x0FFFFFF8UL);
    put32(disk + 516, 0x0FFFFFFFUL);
    chain(disk, candidate_slot, 8);
    chain(disk, fallback_slot, 8);
    chain(disk, grubenv, 2);
    lay(disk, candidate_slot, c, nc);
    lay(disk, fallback_slot, f, nf);
}
struct measure {
    struct fixture *f;
    char source[4096], machine[4096], key[4096], case_dir[4096];
};
static int setup(struct measure *m, struct fixture *f)
{
    const char *files[] = {"machine_net_in.sh", "machine_load.sh", "machine_load_session.sh",
                           "machine_identity.sh"};
    const char *mocks[] = {"ssh", "sender", "cp", "cut", "make", "bootctl", "sha256sum", "shasum"};
    char source[4096], target[4096];
    unsigned i, j;
    unsigned char disk[16384];
    const char *candidate = "the candidate kernel\n", *fallback = "the fallback kernel\n";
    memset(m, 0, sizeof(*m));
    m->f = f;
    snprintf(m->source, sizeof(m->source), "%s/source", f->work);
    snprintf(m->machine, sizeof(m->machine), "%s/machine", f->work);
    snprintf(m->key, sizeof(m->key), "%s/dummy-key", f->work);
    for (i = 0; i < 4; i++) {
        snprintf(source, sizeof(source), "tools/dev/%s", files[i]);
        snprintf(target, sizeof(target), "%s/%s", m->source, files[i]);
        if (!fx_copy(flow_source(f, source), target))
            return 0;
    }
    for (i = 0; i < 8; i++)
        if (!fx_mock(f, mocks[i]))
            return 0;
    image(disk, candidate, 150, fallback, 160);
    if (!fx_write(flow_path(f, "machine/bootpart"), disk, sizeof(disk), 0600))
        return 0;
    image(disk, candidate, 150, candidate, 150);
    if (!fx_write(flow_path(f, "machine/bootpart-same"), disk, sizeof(disk), 0600))
        return 0;
    for (j = 0; j < 150; j++)
        memcpy(disk + j * strlen(candidate), candidate, strlen(candidate));
    if (!fx_write(flow_path(f, "machine/candidate.kernel"), disk, 150 * strlen(candidate), 0600))
        return 0;
    for (j = 0; j < 160; j++)
        memcpy(disk + j * strlen(fallback), fallback, strlen(fallback));
    if (!fx_write(flow_path(f, "machine/fallback.kernel"), disk, 160 * strlen(fallback), 0600) ||
        !fx_text(m->key, "fixture; no credential\n"))
        return 0;
    fx_env("FLOW_MEASURE", "1");
    return 1;
}
static int environment(struct measure *m, const char *name, const char *mode, int external)
{
    char path[9000];
    if (external) {
        if (!fx_external("/tmp/rach2-load.XXXXXX", m->case_dir, sizeof(m->case_dir)))
            return 0;
        printf("measurement fixture: %s\n", m->case_dir);
    } else {
        snprintf(m->case_dir, sizeof(m->case_dir), "%s/%s", m->f->work, name);
        if (!fx_mkdir(m->case_dir))
            return 0;
    }
    fx_env("CASE", m->case_dir);
    fx_env("MODE", mode);
    fx_env("MACHINE", m->machine);
    fx_env("MACHINE_SLOT", "candidate");
    fx_env("MACHINE_NEXT", "empty (a trial was consumed)");
    fx_env("MACHINE_IMAGE", NULL);
    fx_env("FAIL_BUILD", NULL);
    fx_env("RACH2_MEASURE_SLOT", "candidate");
    fx_env("RACH2_MEASURE_KERNEL", flow_path(m->f, "machine/candidate.kernel"));
    fx_env("RACH2_NET_KNOWN", NULL);
    fx_env("RACH2_NET_USER", NULL);
    fx_env("RACH2_LOAD_USER", NULL);
    fx_env("RACH2_MEASURE_USER", NULL);
    snprintf(path, sizeof(path), "%s:/usr/bin:/bin:/usr/sbin:/sbin", m->f->bin);
    fx_env("PATH", path);
    return 1;
}
static int run(struct measure *m, const char *const args[], int interrupt)
{
    struct fx_child p;
    if (!fx_start(m->f, &p, args, m->case_dir, NULL, NULL))
        return 255;
    if (interrupt) {
        if (!fx_until(&p, mp("sent")))
            return 255;
        kill(p.pid, SIGTERM);
    }
    fx_wait(m->f, &p);
    fx_stop(&p, SIGKILL);
    return m->f->status;
}
static unsigned logs(struct measure *m, const char *pattern, const char *leaf, const char *want)
{
    DIR *d = opendir(m->case_dir);
    struct dirent *e;
    unsigned n = 0;
    char path[4096];
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (!fnmatch(pattern, e->d_name, 0)) {
            snprintf(path, sizeof(path), "%s/%s%s%s", m->case_dir, e->d_name, leaf ? "/" : "",
                     leaf ? leaf : "");
            if (fx_exists(path) && (!want || fx_has(path, want)))
                n++;
        }
    closedir(d);
    return n;
}
static int identity_rows(struct measure *m)
{
    static const char *names[] = {"match",   "committed", "fallback",     "other-slot",
                                  "no-slot", "armed",     "other-kernel", "unnamed"};
    static const char *want[] = {
        "machine-identity: result: the machine runs the named kernel: slot candidate",
        "machine-identity: result:",
        "machine-identity: result: the machine runs the named kernel: slot fallback",
        "the machine booted fallback, not the named candidate",
        "the machine booted no slot, not the named candidate",
        "a trial is armed (next_entry candidate)",
        "the candidate slot holds sha256",
        "RACH2_MEASURE_SLOT must name the slot"};
    struct fixture *f = m->f;
    unsigned i;
    char script[4096];
    int ok = 1;
    snprintf(script, sizeof(script), "%s/machine_identity.sh", m->source);
    for (i = 0; i < 8; i++) {
        if (!environment(m, names[i], "identity", 0))
            return 0;
        if (i == 1)
            fx_env("MACHINE_NEXT", "absent");
        if (i == 2) {
            fx_env("MACHINE_SLOT", "fallback");
            fx_env("RACH2_MEASURE_SLOT", "fallback");
            fx_env("RACH2_MEASURE_KERNEL", flow_path(f, "machine/fallback.kernel"));
        }
        if (i == 3) {
            fx_env("MACHINE_SLOT", "fallback");
            fx_env("MACHINE_IMAGE", "bootpart-same");
        }
        if (i == 4)
            fx_env("MACHINE_SLOT", "");
        if (i == 5)
            fx_env("MACHINE_NEXT", "candidate");
        if (i == 6)
            fx_env("RACH2_MEASURE_KERNEL", flow_path(f, "machine/fallback.kernel"));
        if (i == 7)
            fx_env("RACH2_MEASURE_SLOT", "");
        {
            const char *a[] = {"/bin/sh", script,          mp("identity.log"), "ssh",
                               "-o",      "BatchMode=yes", "root@127.0.0.1",   NULL};
            run(m, a, 0);
        }
        ok = fx_check(f,
                      f->status == (i < 3    ? 0
                                    : i == 7 ? 2
                                             : 1) &&
                          (strstr(f->out, want[i]) || strstr(f->err, want[i]) ||
                           fx_has(mp("identity.log"), want[i])),
                      names[i]) &&
             ok;
        if (i < 3)
            ok = fx_check(f,
                          fx_has(mp("identity.log"), "machine-identity: named: slot ") &&
                              fx_has(mp("identity.log"), "machine-identity: sha256: "),
                          "accepted machine has identity evidence") &&
                 ok;
    }
    return ok;
}
static int inbound(struct measure *m, int interrupt)
{
    char script[4096], sender[4096];
    snprintf(script, sizeof(script), "%s/machine_net_in.sh", m->source);
    snprintf(sender, sizeof(sender), "%s/sender", m->f->bin);
    const char *a[] = {"/bin/sh", script, m->key,      "127.0.0.1", "/dummy/receiver", sender, "1",
                       "4096",    "0",    m->case_dir, NULL};
    return run(m, a, interrupt);
}
static int load(struct measure *m, const char *sessions)
{
    char script[4096];
    snprintf(script, sizeof(script), "%s/machine_load.sh", m->source);
    const char *a[] = {"/bin/sh", script, m->key, "127.0.0.1", sessions, "3600", m->case_dir, NULL};
    return run(m, a, 0);
}
static int refusal_rows(struct measure *m)
{
    struct fixture *f = m->f;
    int ok = 1;
    if (!environment(m, "refused-net", "success", 0))
        return 0;
    fx_env("MACHINE_SLOT", "fallback");
    inbound(m, 0);
    ok = fx_check(f,
                  f->status == 1 && !fx_exists(mp("sent")) &&
                      fx_equal(mp("commands"), "identity\n") &&
                      !logs(m, "net-in.*", "known_hosts", NULL) &&
                      logs(m, "net-in.*", "identity.log", "machine-identity: refused: ") == 1,
                  "inbound refuses before transfer") &&
         ok;
    if (!environment(m, "refused-load", "load", 1))
        return 0;
    fx_env("MACHINE_SLOT", "fallback");
    load(m, "2");
    return fx_check(f,
                    f->status == 1 && fx_equal(mp("commands"), "identity\n") &&
                        !logs(m, "load-*-session*.log", NULL, NULL) &&
                        logs(m, "load-*-identity.log", NULL, "machine-identity: refused: ") == 1,
                    "load refuses before sessions") &&
           ok;
}
static int inbound_rows(struct measure *m)
{
    const char *modes[] = {"success", "failure", "interrupt", "success", "interrupt"};
    unsigned i;
    int ok = 1;
    for (i = 0; i < 5; i++) {
        char name[100];
        snprintf(name, sizeof(name), "inbound-%u", i);
        if (!environment(m, name, modes[i], 0))
            return 0;
        if (i >= 3)
            fx_env("RACH2_NET_KNOWN", mp("caller-known"));
        inbound(m, i == 2 || i == 4);
        ok = fx_check(m->f,
                      (m->f->status == 0) == !strcmp(modes[i], "success") &&
                          fx_has(mp("commands"), "identity\n") &&
                          !logs(m, "net-in.*", "known_hosts", NULL) &&
                          logs(m, "net-in.*", "receive.log", NULL) > 0,
                      "inbound cleanup keeps measurement log") &&
             ok;
        {
            char *s = fx_read(mp("commands"));
            ok = fx_check(m->f, !strncmp(s, "identity\n", 9), "identity checked first") && ok;
            free(s);
        }
        if (i >= 3)
            ok = fx_check(m->f, fx_equal(mp("caller-known"), "dummy address and public key\n"),
                          "caller known-hosts retained") &&
                 ok;
    }
    return ok;
}
static int load_rows(struct measure *m)
{
    unsigned i;
    int ok = 1;
    const int finishes[] = {3799, 3700, 3799};
    for (i = 0; i < 3; i++) {
        char clock[200], expected[300], log[300];
        if (!environment(m, "load-window", "load", 1))
            return 0;
        snprintf(clock, sizeof(clock), "100\n101\n103\n3699\n%d\n%d\n", finishes[i],
                 finishes[i] + 1);
        fx_text(mp("clock"), clock);
        fx_env("FAIL_BUILD", i == 2 ? "1" : "0");
        load(m, "1");
        snprintf(expected, sizeof(expected),
                 "SESSION 1 builds %d failed %d before-deadline 1 late %d", i == 2 ? 1 : 2,
                 i == 2 ? 1 : 0, i == 2 ? 0 : 1);
        snprintf(log, sizeof(log), "start 3699 finish %d status %d", finishes[i], i == 2 ? 7 : 0);
        ok = fx_check(m->f,
                      (m->f->status == 0) == (i != 2) && strstr(m->f->out, expected) &&
                          logs(m, "load-*-session1.log", NULL,
                               "start 100 deadline 3700 clock guest-tick-seconds") == 1 &&
                          logs(m, "load-*-session1.log", NULL, log) == 1,
                      "late completions and failures counted separately") &&
             ok;
        {
            char *s = fx_read(mp("commands"));
            ok = fx_check(m->f, !strncmp(s, "identity\n", 9), "load identity checked first") && ok;
            free(s);
        }
    }
    return ok;
}
static int stagger_row(struct measure *m)
{
    char path[9000], *order, *line, *save = NULL;
    unsigned starts = 0;
    int pause_between = 0, ok = 1;
    if (!environment(m, "stagger", "stagger", 1) ||
        !fx_copy(m->f->self, flow_path(m->f, "pace/sleep")))
        return 0;
    snprintf(path, sizeof(path), "%s/pace:%s:/usr/bin:/bin:/usr/sbin:/sbin", m->f->work, m->f->bin);
    fx_env("PATH", path);
    load(m, "3");
    if (m->f->status)
        return 0;
    order = fx_read(mp("order"));
    for (line = strtok_r(order, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "session ", 8)) {
            if (starts && !pause_between)
                ok = 0;
            starts++;
            pause_between = 0;
        } else if (!strcmp(line, "sleep 1"))
            pause_between = 1;
    }
    free(order);
    return fx_check(m->f, ok && starts == 3, "session starts have a one-second pause between them");
}
int flow_measure(struct fixture *f)
{
    struct measure m;
    int ok = 1;
    if (!setup(&m, f))
        return 0;
    ok = identity_rows(&m) && ok;
    ok = refusal_rows(&m) && ok;
    ok = inbound_rows(&m) && ok;
    ok = load_rows(&m) && ok;
    ok = stagger_row(&m) && ok;
    fx_env("FLOW_MEASURE", NULL);
    return ok;
}
