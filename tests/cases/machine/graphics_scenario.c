/*
 * graphics_scenario.c: the browser runners' cases (machine.webgl.json and
 * machine.video.json). Each boots an overlay of the GNU root with a display,
 * runs tests/graphics/runners/webgl.sh on one chunk of the WebGL suite or
 * video.sh on one clip as the test account, and judges the result against
 * the Debian reference guest's stored one with judge.py, in the guest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcase.h"

/* The records of the boot the cases wait for: the root mounted and the
 * login prompt. */
#define ROOT_MOUNTED "ext2: root: /dev/disk0p1"
#define LOGIN_PROMPT "rach2 login"

/* Where the case's files go on the machine, and the stored references in
 * the archive of tests/graphics that the case puts there. */
#define GRAPHICS_DIR "/tmp/graphics"
#define GRAPHICS_REF GRAPHICS_DIR "/ref/debian/virtio-vga"

/* What one attempt shares among its steps. */
struct graphics_case {
    struct net_case u;
    struct net_boot b;
    int video;          /* video.sh's case, else webgl.sh's */
    char browser[16];   /* chromium or firefox */
    char version[8];    /* the WebGL suite's version, 1.0.4 or 2.0.1 */
    char chunk[8];      /* the chunk's number, two digits */
    char clip[48];      /* the clip's file name */
    char password[128]; /* root's, at the console */
};

/**
 * Read the case from the last part of its identity: BROWSER-webglV-NN, a
 * chunk of WebGL V's list, or BROWSER-CODEC-SIZE, a clip. Give 0 for an
 * identity of neither form.
 */
static int parse(struct graphics_case *s, const char *id)
{
    const char *name = strrchr(id, '.'), *dash;
    char rest[40];
    size_t n;

    if (name == NULL)
        return 0;
    name++;
    dash = strchr(name, '-');
    n = dash != NULL ? (size_t)(dash - name) : 0;
    if (n == 0 || n >= sizeof(s->browser) || strlen(dash + 1) >= sizeof(rest))
        return 0;
    memcpy(s->browser, name, n);
    s->browser[n] = '\0';
    if (strcmp(s->browser, "chromium") != 0 && strcmp(s->browser, "firefox") != 0)
        return 0;
    strcpy(rest, dash + 1);
    s->video = strstr(id, ".video.") != NULL;
    if (!s->video) {
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
    snprintf(s->clip, sizeof(s->clip), "%s.%s", rest,
             strncmp(rest, "h264-", 5) == 0 ? "mp4" : "webm");
    return 1;
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

/** Put artifact `name` at `remote` on the machine. */
static void put(struct graphics_case *s, const char *name, const char *remote)
{
    const char *path = etude_artifact(s->u.c, name);

    if (!net_ok(&s->u))
        return;
    if (path == NULL) {
        net_fail(&s->u, "artifact %s is not the case's", name);
        return;
    }
    if (etude_guest_put(s->b.m, path, remote, "0644") != ETUDE_OK)
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

/** Put the runners, the references and, for a clip, the clips on the
 * machine, owned by the test account. */
static void stage(struct graphics_case *s)
{
    if (net_step(&s->u, "stage") == ETUDE_OK)
        must(s, "rm -rf " GRAPHICS_DIR " && mkdir -p " GRAPHICS_DIR, "mkdir",
             "mkdir " GRAPHICS_DIR);
    put(s, "graphics", GRAPHICS_DIR "/graphics.tar");
    if (s->video)
        put(s, "clips", GRAPHICS_DIR "/clips.tar");
    if (!net_ok(&s->u))
        return;
    must(s,
         "cd " GRAPHICS_DIR " && tar -xf graphics.tar && rm graphics.tar && "
         "if [ -f clips.tar ]; then mkdir clips && tar -xf clips.tar -C clips && rm clips.tar; fi "
         "&& "
         "chown -R test " GRAPHICS_DIR,
         "unpack", "the case's files could not be unpacked");
}

/**
 * Run the runner as the test account, with the environment of a Done's
 * conformance run, and assert that it ran its whole chunk or every play.
 */
static void run(struct graphics_case *s)
{
    char runner[512], command[1024], line[200];
    const char *pattern = s->video ? "video: result: " : "webgl: result: ";
    struct net_run r;

    if (s->video)
        snprintf(runner, sizeof(runner),
                 "sh " GRAPHICS_DIR "/runners/video.sh -b %s -c " GRAPHICS_DIR
                 "/clips -o " GRAPHICS_DIR "/out %s",
                 s->browser, s->clip);
    else
        snprintf(runner, sizeof(runner),
                 "sh " GRAPHICS_DIR "/runners/webgl.sh -b %s -v %s -o " GRAPHICS_DIR "/out %s",
                 s->browser, s->version, s->chunk);
    snprintf(command, sizeof(command),
             "su test -s /bin/sh -c 'GRAPHICS_CLASS=conformance GRAPHICS_DONE=1; "
             "export GRAPHICS_CLASS GRAPHICS_DONE; . " GRAPHICS_DIR "/guest-env.sh && "
             "graphics_env > /tmp/graphics.env && %s'",
             runner);
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
    net_run_free(&r);
}

/** Judge the result against the stored Debian one, in the guest. */
static void judge(struct graphics_case *s)
{
    char command[1024], line[200];
    struct net_run r;

    if (s->video)
        snprintf(command, sizeof(command),
                 "python3 -I " GRAPHICS_DIR "/runners/judge.py video " GRAPHICS_REF
                 "/video-%s/%s-%s.plays " GRAPHICS_DIR "/out/%s-%s.plays " GRAPHICS_REF
                 "/video-%s/%s-%s.decoders " GRAPHICS_DIR "/out/%s-%s.decoders",
                 s->browser, s->browser, s->clip, s->browser, s->clip, s->browser, s->browser,
                 s->clip, s->browser, s->clip);
    else
        snprintf(command, sizeof(command),
                 "python3 -I " GRAPHICS_DIR "/runners/judge.py webgl " GRAPHICS_REF
                 "/webgl-%s-%s/%s-%s-%s.pages " GRAPHICS_DIR "/out/%s-%s-%s.pages",
                 s->browser, s->version, s->browser, s->version, s->chunk, s->browser, s->version,
                 s->chunk);
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
    char *argv[] = {"sh", NULL, "host", NULL};
    struct net_run r;

    if (net_step(&s->u, "collect") != ETUDE_OK)
        return;
    must(s, "tar -czf " GRAPHICS_DIR "/out.tar.gz -C " GRAPHICS_DIR "/out .", "pack",
         "the output could not be packed");
    if (net_ok(&s->u) &&
        etude_guest_get(s->b.m, GRAPHICS_DIR "/out.tar.gz", "out.tar.gz") != ETUDE_OK)
        net_fail(&s->u, "the output could not be fetched");
    argv[1] = (char *)etude_artifact(s->u.c, "common");
    if (net_ok(&s->u) && argv[1] != NULL && net_host(&s->u, argv, "host-record", &r) == ETUDE_OK)
        net_run_free(&r);
}

int etude_scenario(struct etude_case *c)
{
    struct graphics_case s;

    memset(&s, 0, sizeof(s));
    net_case(&s.u, c);
    if (!parse(&s, etude_case_id(c)))
        net_fail(&s.u, "the case's identity names no chunk or clip");
    net_secret(&s.u, "demo-password", s.password, sizeof(s.password));
    up(&s);
    stage(&s);
    run(&s);
    judge(&s);
    collect(&s);
    return net_end(&s.u);
}
