/*
 * graphics_scenario.c: the graphics runners' cases (machine.webgl.json,
 * machine.video.json, machine.igt.json, machine.modetest.json,
 * machine.piglit.json, machine.glmark2.json and machine.venus.json). Each
 * boots an overlay of the GNU root with a display, runs one runner of
 * tests/graphics/runners on its case, and judges the result against the
 * Debian reference guest's stored one, with judge.py or, for the loss of
 * Venus's render server, diff, in the guest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The records of the boot the cases wait for: the root mounted and the
 * login prompt. */
#define ROOT_MOUNTED "ext2: root: /dev/disk0p1"
#define LOGIN_PROMPT "rach2 login"

/* Where the case's files go on the machine, and the stored references among
 * them, one directory for each display profile, and the Venus references of
 * the testing PC. */
#define GRAPHICS_DIR "/tmp/graphics"
#define GRAPHICS_REF GRAPHICS_DIR "/ref/debian"
#define VENUS_REF    GRAPHICS_DIR "/ref/venus-wsl/deqp"
#define VENUS_LOST   GRAPHICS_DIR "/ref/venus-wsl/lost/lost.out"

/* The Venus cases' dEQP-VK: the release Mesa 25.0.7's CI pins, Venus's ICD
 * alone, so that the guest's lavapipe is not the device, the lists and the
 * flakes, and the parts the release runs the lavapipe fraction in. */
#define VENUS_DEQP                                       \
    "sh " GRAPHICS_DIR "/runners/deqp.sh -v 1.4.1.1 -i " \
    "/usr/share/vulkan/icd.d/venus_icd.x86_64.json -o " GRAPHICS_DIR "/out"
#define VENUS_SMOKE    GRAPHICS_DIR "/deqp-vk-venus-smoke.txt"
#define VENUS_LIST     GRAPHICS_DIR "/deqp-vk-lavapipe.txt.gz"
#define VENUS_FLAKES   GRAPHICS_DIR "/deqp-vk-lavapipe.flakes"
#define VENUS_PARTS    8
/* The run's counts and wall time, after its result line, for the log. */
#define VENUS_SUMMARY  " && cat " GRAPHICS_DIR "/out/summary.txt"
/* The device loss case's runner, in the guest, as the test account. */
#define VENUS_LOST_RUN "su -s /bin/sh test -c 'sh " GRAPHICS_DIR "/runners/venus-lost.sh "

/* The data disks after the root, in the order of the machine's disks: the
 * files of tests/graphics that every case has, and the clips of a video
 * case. Each is an ext2 volume the guest mounts read-only, since the serial
 * channel carries some 137 bytes a frame and would take over an hour for the
 * 35 MB of clips. */
#define GRAPHICS_DISK "/dev/disk1"
#define CLIPS_DISK    "/dev/disk2"
#define GRAPHICS_DATA "/tmp/graphics-data"

/* The runners, by the family of the case's identity. */
enum runner { WEBGL, VIDEO, IGT, MODETEST, PIGLIT, GLMARK2, VENUS };

/* What one attempt shares among its steps. */
struct graphics_case {
    struct net_case u;
    struct net_boot b;
    enum runner runner;
    char profile[16];   /* the display profile, the references' directory */
    char browser[16];   /* chromium or firefox */
    char version[8];    /* the WebGL suite's version, 1.0.4 or 2.0.1 */
    char chunk[8];      /* the chunk's number, two digits */
    char clip[48];      /* the clip's file name */
    char test[48];      /* IGT's test program, piglit's part or Venus's case */
    int part;           /* the part of the lavapipe fraction, 1 to VENUS_PARTS */
    char password[128]; /* root's, at the console */
};

/**
 * Read a browser case from the last part of its identity, NAME:
 * BROWSER-webglV-NN, a chunk of WebGL V's list, or BROWSER-CODEC-SIZE, a
 * clip. Give 0 for a name of neither form.
 */
static int parse_browser(struct graphics_case *s, const char *name)
{
    const char *dash = strchr(name, '-');
    char rest[40];
    size_t n = dash != NULL ? (size_t)(dash - name) : 0;

    if (n == 0 || n >= sizeof(s->browser) || strlen(dash + 1) >= sizeof(rest))
        return 0;
    memcpy(s->browser, name, n);
    s->browser[n] = '\0';
    if (strcmp(s->browser, "chromium") != 0 && strcmp(s->browser, "firefox") != 0)
        return 0;
    strcpy(rest, dash + 1);
    strcpy(s->profile, "virtio-vga");
    if (s->runner == VIDEO) {
        snprintf(s->clip, sizeof(s->clip), "%s.%s", rest,
                 strncmp(rest, "h264-", 5) == 0 ? "mp4" : "webm");
        return 1;
    }
    if (strncmp(rest, "webgl1-", 7) == 0)
        strcpy(s->version, "1.0.4");
    else if (strncmp(rest, "webgl2-", 7) == 0)
        strcpy(s->version, "2.0.1");
    else
        return 0;
    if (strlen(rest + 7) != 2)
        return 0;
    strcpy(s->chunk, rest + 7);
    return 1;
}

/**
 * Take PROFILE as the case's display profile, giving 0 unless it is one
 * that the references have: vkms, whose directory is `none`, since the
 * machine has no display device, std-vga or virtio-vga.
 */
static int set_profile(struct graphics_case *s, const char *profile)
{
    if (strcmp(profile, "vkms") == 0)
        profile = "none";
    else if (strcmp(profile, "std-vga") != 0 && strcmp(profile, "virtio-vga") != 0)
        return 0;
    strcpy(s->profile, profile);
    return 1;
}

/**
 * Read a Venus case, NAME: `smoke`, `sample`, `device-lost`, or
 * `fraction-N`, part N of VENUS_PARTS of the lavapipe fraction. Give 0 for
 * another name.
 */
static int parse_venus(struct graphics_case *s, const char *name)
{
    strcpy(s->profile, "venus");
    if (strcmp(name, "smoke") == 0 || strcmp(name, "sample") == 0 ||
        strcmp(name, "device-lost") == 0) {
        strcpy(s->test, name);
        return 1;
    }
    if (strncmp(name, "fraction-", 9) != 0 || name[9] < '1' || name[9] > '0' + VENUS_PARTS ||
        name[10] != '\0')
        return 0;
    strcpy(s->test, "fraction");
    s->part = name[9] - '0';
    return 1;
}

/**
 * Read the case from the last part of its identity, after its family:
 * webgl's and video's as parse_browser says; igt's TEST-PROFILE, IGT's test
 * program TEST, whose name has no dash, on PROFILE; modetest's PROFILE;
 * piglit's PART, on virtio-vga; glmark2's `wayland`, on virtio-vga; and
 * venus's as parse_venus says. Give 0 for an identity of no such form.
 */
static int parse(struct graphics_case *s, const char *id)
{
    static const char *const families[] = {".webgl.",  ".video.",   ".igt.",  ".modetest.",
                                           ".piglit.", ".glmark2.", ".venus."};
    const char *name = strrchr(id, '.'), *dash;
    size_t i, n;

    if (name == NULL)
        return 0;
    name++;
    for (i = 0; i < sizeof(families) / sizeof(families[0]); i++)
        if (strstr(id, families[i]) != NULL)
            break;
    if (i == sizeof(families) / sizeof(families[0]))
        return 0;
    s->runner = (enum runner)i;
    switch (s->runner) {
    case WEBGL:
    case VIDEO:
        return parse_browser(s, name);
    case IGT:
        dash = strchr(name, '-');
        n = dash != NULL ? (size_t)(dash - name) : 0;
        if (n == 0 || n >= sizeof(s->test))
            return 0;
        memcpy(s->test, name, n);
        s->test[n] = '\0';
        return set_profile(s, dash + 1);
    case MODETEST:
        return strcmp(name, "vkms") != 0 && set_profile(s, name);
    case PIGLIT:
        if (strlen(name) >= sizeof(s->test))
            return 0;
        strcpy(s->test, name);
        return set_profile(s, "virtio-vga");
    case GLMARK2:
        return strcmp(name, "wayland") == 0 && set_profile(s, "virtio-vga");
    case VENUS:
        return parse_venus(s, name);
    }
    return 0;
}

/** Run `command` as root under bash through the guest channel, keeping
 * what it prints as evidence `output`. */
static enum etude_status as_root(struct graphics_case *s, const char *command, const char *output,
                                 struct net_run *r)
{
    const char *argv[] = {"/bin/bash", "-c", command, NULL};

    return net_guest(&s->u, s->b.m, argv, NULL, NULL, output, r);
}

/** Run `command` as root and fail the attempt with `what` unless it exits
 * 0. */
static void must(struct graphics_case *s, const char *command, const char *output, const char *what)
{
    char line[160];
    struct net_run r;

    if (as_root(s, command, output, &r) != ETUDE_OK)
        return;
    if (!net_exited0(&r))
        net_fail(
            &s->u, "%s: %s", what,
            net_copy_line(r.err != NULL && r.err[0] != '\0' ? r.err : r.out, line, sizeof(line)));
    net_run_free(&r);
}

/** Put artifact `name` at `remote` on the machine with permissions `mode`. */
static void put(struct graphics_case *s, const char *name, const char *remote, const char *mode)
{
    const char *path = etude_artifact(s->u.c, name);

    if (!net_ok(&s->u))
        return;
    if (path == NULL) {
        net_fail(&s->u, "artifact %s is not the case's", name);
        return;
    }
    if (etude_guest_put(s->b.m, path, remote, mode) != ETUDE_OK)
        net_fail(&s->u, "%s could not be transferred to the guest", name);
}

/** Boot the machine, log in as root at the console and open the channel. */
static void up(struct graphics_case *s)
{
    if (net_step(&s->u, "boot") == ETUDE_OK)
        net_boot(&s->b, &s->u, "gnu", 1, NULL);
    net_seen(&s->b, ROOT_MOUNTED);
    net_seen(&s->b, LOGIN_PROMPT);
    if (net_login(&s->b, "root", s->password, "# ") == ETUDE_OK &&
        etude_guest_open(s->b.m) != ETUDE_OK)
        net_fail(&s->u, "the GNU guest command channel did not open");
}

/** Copy the runners and the references from their data disk to the machine,
 * owned by the test account, mount a video case's clips read-only beside
 * them, and put modetest's drmcapture there. */
static void stage(struct graphics_case *s)
{
    if (net_step(&s->u, "stage") == ETUDE_OK)
        must(s,
             "rm -rf " GRAPHICS_DIR " " GRAPHICS_DATA " && mkdir -p " GRAPHICS_DIR " " GRAPHICS_DATA
             " && mount -t ext2 -o ro " GRAPHICS_DISK " " GRAPHICS_DATA " && cp -R " GRAPHICS_DATA
             "/. " GRAPHICS_DIR " && umount " GRAPHICS_DATA " && rmdir " GRAPHICS_DATA
             " " GRAPHICS_DIR "/lost+found",
             "copy", "the graphics data disk could not be copied");
    if (s->runner == MODETEST)
        put(s, "drmcapture", GRAPHICS_DIR "/drmcapture", "0755");
    if (!net_ok(&s->u))
        return;
    must(s, "chown -R test " GRAPHICS_DIR, "chown", "the case's files could not be given to test");
    if (s->runner == VIDEO && net_ok(&s->u))
        must(s,
             "mkdir " GRAPHICS_DIR "/clips && mount -t ext2 -o ro " CLIPS_DISK " " GRAPHICS_DIR
             "/clips",
             "clips", "the clips' data disk could not be mounted");
}

/**
 * Write the runner's command line for the case to `runner`, and the line
 * that begins its result to `*pattern`. Give 1 when the runner runs as
 * root, IGT and modetest, which take the card's master, and 0 when it runs
 * as the test account.
 */
static int runner_line(struct graphics_case *s, char *runner, size_t size, const char **pattern)
{
    static const char *const results[] = {
        "webgl: result: ",  "video: result: ",   "igt: result: ", "modetest: result: ",
        "piglit: result: ", "glmark2: result: ", "deqp: result: "};

    *pattern = results[s->runner];
    switch (s->runner) {
    case WEBGL:
        snprintf(runner, size,
                 "sh " GRAPHICS_DIR "/runners/webgl.sh -b %s -v %s -o " GRAPHICS_DIR "/out %s",
                 s->browser, s->version, s->chunk);
        return 0;
    case VIDEO:
        snprintf(runner, size,
                 "sh " GRAPHICS_DIR "/runners/video.sh -b %s -c " GRAPHICS_DIR
                 "/clips -o " GRAPHICS_DIR "/out %s",
                 s->browser, s->clip);
        return 0;
    case IGT:
        snprintf(runner, size, "sh " GRAPHICS_DIR "/runners/igt.sh -o " GRAPHICS_DIR "/out %s",
                 s->test);
        return 1;
    case MODETEST:
        snprintf(runner, size,
                 "sh " GRAPHICS_DIR "/runners/modetest.sh -c " GRAPHICS_DIR
                 "/drmcapture -o " GRAPHICS_DIR "/out");
        return 1;
    case PIGLIT:
        snprintf(runner, size, "sh " GRAPHICS_DIR "/runners/piglit.sh -o " GRAPHICS_DIR "/out %s",
                 s->test);
        return 0;
    case GLMARK2:
        snprintf(runner, size, "sh " GRAPHICS_DIR "/runners/glmark2.sh -o " GRAPHICS_DIR "/out");
        return 0;
    case VENUS:
        if (strcmp(s->test, "smoke") == 0)
            snprintf(runner, size, VENUS_DEQP " -l " VENUS_SMOKE " vk" VENUS_SUMMARY);
        else if (strcmp(s->test, "sample") == 0)
            snprintf(runner, size,
                     VENUS_DEQP " -e 500 -l " VENUS_LIST " -f " VENUS_FLAKES " vk" VENUS_SUMMARY);
        else
            snprintf(runner, size,
                     VENUS_DEQP " -e %d -s %d -l " VENUS_LIST " -f " VENUS_FLAKES
                                " vk" VENUS_SUMMARY,
                     VENUS_PARTS, s->part);
        return 0;
    }
    return 0;
}

/**
 * Run the runner, as root or as the test account, with the environment of
 * a Done's run of its class, performance for glmark2 and conformance for
 * the others, assert that it ran its whole case, and print what it printed.
 */
static void run(struct graphics_case *s)
{
    char runner[512], env[768], command[1024], line[200];
    const char *pattern;
    struct net_run r;
    int root = runner_line(s, runner, sizeof(runner), &pattern);

    snprintf(env, sizeof(env),
             "GRAPHICS_CLASS=%s GRAPHICS_DONE=1; export GRAPHICS_CLASS GRAPHICS_DONE; "
             ". " GRAPHICS_DIR "/guest-env.sh && graphics_env > /tmp/graphics.env && %s",
             s->runner == GLMARK2 ? "performance" : "conformance", runner);
    if (root)
        snprintf(command, sizeof(command), "%s", env);
    else
        snprintf(command, sizeof(command), "su -s /bin/sh test -c '%s'", env);
    if (net_step(&s->u, "run") == ETUDE_OK)
        as_root(s, command, "runner", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "ran", 0, "");
        return;
    }
    net_assert(&s->u, "ran", net_exited0(&r) && net_find(r.out, pattern) != NULL,
               "the runner did not run its whole case: %s",
               net_copy_line(net_find(r.out, pattern) != NULL ? net_find(r.out, pattern)
                                                              : (r.err != NULL ? r.err : r.out),
                             line, sizeof(line)));
    /* The runner's own lines, its result among them, go to the run's log. */
    if (r.out != NULL)
        fputs(r.out, stdout);
    net_run_free(&r);
}

/**
 * Run the device loss case: four deqp-vk processes on Venus as the test
 * account, the host's kill of the render server of this case's QEMU, which
 * the artifact venus-lost finds by the workspace its own path is in, and
 * then the processes' ends and a new vulkaninfo; assert that each part ran
 * and print the guest's lines.
 */
static void run_lost(struct graphics_case *s)
{
    char *kill[] = {"venus-lost", "kill", NULL};
    char line[200];
    struct net_run r;
    const char *why = NULL;

    if (net_step(&s->u, "run") == ETUDE_OK)
        as_root(s, VENUS_LOST_RUN "start'", "start", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "ran", 0, "");
        return;
    }
    if (!net_exited0(&r) || net_find(r.out, "venus-lost: result: ") == NULL)
        why = "the four processes did not start";
    net_copy_line(r.err != NULL && *r.err != '\0' ? r.err : r.out, line, sizeof(line));
    net_run_free(&r);
    if (why == NULL && net_host(&s->u, kill, "kill", &r) == ETUDE_OK) {
        if (!net_exited0(&r) || net_find(r.out, "venus-lost: result: killed") == NULL)
            why = "the host did not kill the render server";
        net_copy_line(r.err != NULL && *r.err != '\0' ? r.err : r.out, line, sizeof(line));
        if (r.out != NULL)
            fputs(r.out, stdout);
        net_run_free(&r);
    }
    if (why == NULL && net_ok(&s->u) &&
        as_root(s, VENUS_LOST_RUN "after'", "runner", &r) == ETUDE_OK) {
        if (!net_exited0(&r) || net_find(r.out, "vulkaninfo exit ") == NULL)
            why = "the processes' ends were not read";
        net_copy_line(r.err != NULL && *r.err != '\0' ? r.err : r.out, line, sizeof(line));
        if (r.out != NULL)
            fputs(r.out, stdout);
        net_run_free(&r);
    }
    if (why == NULL && !net_ok(&s->u))
        why = "the guest channel failed";
    net_assert(&s->u, "ran", why == NULL, "%s: %s", why != NULL ? why : "", line);
}

/** Write judge.py's command line for the case to `command`. */
static void judge_line(struct graphics_case *s, char *command, size_t size)
{
    int n = snprintf(command, size, "python3 -I " GRAPHICS_DIR "/runners/judge.py ");

    command += n;
    size -= (size_t)n;
    switch (s->runner) {
    case WEBGL:
        snprintf(command, size,
                 "webgl " GRAPHICS_REF "/%s/webgl-%s-%s/%s-%s-%s.pages " GRAPHICS_DIR
                 "/out/%s-%s-%s.pages",
                 s->profile, s->browser, s->version, s->browser, s->version, s->chunk, s->browser,
                 s->version, s->chunk);
        break;
    case VIDEO:
        snprintf(command, size,
                 "video " GRAPHICS_REF "/%s/video-%s/%s-%s.plays " GRAPHICS_DIR
                 "/out/%s-%s.plays " GRAPHICS_REF "/%s/video-%s/%s-%s.decoders " GRAPHICS_DIR
                 "/out/%s-%s.decoders",
                 s->profile, s->browser, s->browser, s->clip, s->browser, s->clip, s->profile,
                 s->browser, s->browser, s->clip, s->browser, s->clip);
        break;
    case IGT:
    case PIGLIT:
        snprintf(command, size,
                 "results " GRAPHICS_REF "/%s/%s/%s.results " GRAPHICS_DIR "/out/%s.results",
                 s->profile, s->runner == IGT ? "igt" : "piglit", s->test, s->test);
        break;
    case MODETEST:
        snprintf(command, size,
                 "modes " GRAPHICS_REF "/%s/modetest/modes " GRAPHICS_DIR "/out/modes", s->profile);
        break;
    case GLMARK2:
        snprintf(command, size,
                 "glmark2 " GRAPHICS_REF "/%s/glmark2/validations " GRAPHICS_DIR "/out/validations",
                 s->profile);
        break;
    case VENUS:
        if (strcmp(s->test, "device-lost") == 0) {
            /* What the guest saw of the loss is the Debian guest's. */
            snprintf(command - n, size + (size_t)n,
                     "if diff " VENUS_LOST " " GRAPHICS_DIR "/out/lost.txt; then "
                     "echo 'judge: result: pass'; else echo 'judge: result: fail'; fi");
            break;
        }
        /* The smoke case's device, as vulkaninfo describes it, is the
         * Debian guest's: Venus over the same host lavapipe. */
        snprintf(command, size,
                 "deqp " VENUS_REF "/%s/results.csv.gz " GRAPHICS_DIR
                 "/out/results.csv.gz " GRAPHICS_DIR "/out/flakes%s",
                 strcmp(s->test, "smoke") == 0 ? "smoke" : "fraction",
                 strcmp(s->test, "smoke") != 0
                     ? ""
                     : " && d() { grep -E '^[[:space:]]+(apiVersion|driverVersion|vendorID|"
                       "deviceID|deviceType|deviceName|driverID|driverName|driverInfo|"
                       "conformanceVersion)[[:space:]]*=' \"$1\"; } && "
                       "d " VENUS_REF "/smoke/vulkaninfo.out > /tmp/vk.ref && "
                       "d " GRAPHICS_DIR "/out/vulkaninfo.out > /tmp/vk.out && "
                       "grep -q 'Virtio-GPU Venus' /tmp/vk.out && diff /tmp/vk.ref /tmp/vk.out");
        break;
    }
}

/** Judge the result against the stored Debian one, in the guest. */
static void judge(struct graphics_case *s)
{
    char command[1024], line[200];
    struct net_run r;

    judge_line(s, command, sizeof(command));
    if (net_step(&s->u, "judge") == ETUDE_OK)
        as_root(s, command, "judge", &r);
    if (!net_ok(&s->u)) {
        net_assert(&s->u, "as-debian", 0, "");
        return;
    }
    net_assert(&s->u, "as-debian",
               net_exited0(&r) && net_find(r.out, "judge: result: pass") != NULL, "%s",
               net_copy_line(net_find(r.out, "judge: result: ") != NULL
                                 ? net_find(r.out, "judge: result: ")
                                 : (r.err != NULL ? r.err : r.out),
                             line, sizeof(line)));
    net_run_free(&r);
}

/** Keep the runner's output and the host's half of the record as
 * evidence. */
static void collect(struct graphics_case *s)
{
    char *argv[] = {"common", "host", NULL};
    struct net_run r;

    if (net_step(&s->u, "collect") != ETUDE_OK)
        return;
    must(s, "tar -czf " GRAPHICS_DIR "/out.tar.gz -C " GRAPHICS_DIR "/out .", "pack",
         "the output could not be packed");
    if (net_ok(&s->u) &&
        etude_guest_get(s->b.m, GRAPHICS_DIR "/out.tar.gz", "out.tar.gz") != ETUDE_OK)
        net_fail(&s->u, "the output could not be fetched");
    /* common.sh runs as the artifact itself, by its #! line, since sh is
     * no tool of the cases' manifests. */
    if (net_ok(&s->u) && net_host(&s->u, argv, "host-record", &r) == ETUDE_OK)
        net_run_free(&r);
}

int etude_scenario(struct etude_case *c)
{
    struct graphics_case s;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    if (!parse(&s, etude_case_id(c)))
        net_fail(&s.u, "the case's identity names no runner's case");
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    up(&s);
    stage(&s);
    if (s.runner == VENUS && strcmp(s.test, "device-lost") == 0)
        run_lost(&s);
    else
        run(&s);
    judge(&s);
    collect(&s);
    return net_end(&s.u);
}
