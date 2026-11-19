/*
 * etude_nativedeploy_hosttest.c: the setup that `native-deploy run` sends
 * before the native collection (tests/native/deploy_remote.c), run by the
 * host's sh on a state directory of the test's own.
 *
 * host-deps: tests/native/deploy_remote.c tests/host/compat/fixdir_host.c
 * host-cflags: -I tests/native -I tools/etude/include -Wall -Wextra -Werror
 *
 * nd_ssh is replaced by one that keeps the command; the test cuts out the
 * machine's identity guard and the collection's exec, and points the kernel's
 * boot_id, the etude boot token and /proc/$$/stat at files of its own. A
 * controller claim or a collection lock that an older deployment recorded
 * with the etude boot token of this boot must be kept, and the lock refused
 * by name; a claim or a lock of another boot must be cleared with a line that
 * names it, leaving no name under STATE/gone.; a setup killed just after it
 * renamed a stale lock must leave no lock at STATE/collection.lock, and the
 * next setup must pass and remove what it left; names under STATE/gone. that
 * the collection left must be removed; and a lock with no owner record must
 * still be refused.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "deploy.h"
#include "fixdir_host.h"

#define BOOT_ID  "6c3e0f4a-1b2d-4e5f-8a9b-0c1d2e3f4a5b"
#define OTHER_ID "11111111-2222-4333-8444-555555555555"
#define TOKEN    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define ARCHIVE  "a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0"

static int failures;
static char sent[32768]; /* the command nd_ssh was given last */

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "etude-nativedeploy-test: failed: %s\n", what); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/** As tests/native/deploy_process.c formats: 1, or 0 when `out` is short. */
int nd_format(char *out, size_t size, const char *format, ...)
{
    va_list arguments;
    int n;
    va_start(arguments, format);
    n = vsnprintf(out, size, format, arguments);
    va_end(arguments);
    return n >= 0 && (size_t)n < size;
}

/** Keep `command` in `sent` in place of running it over SSH; 0. */
int nd_ssh(struct nd_deploy *d, const char *name, const char *command, const char *input,
           struct nd_process **process)
{
    (void)d;
    (void)name;
    (void)input;
    *process = NULL;
    snprintf(sent, sizeof(sent), "%s", command);
    return 0;
}

/** Write `text` to `path`, ending the test when it cannot. */
static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f == NULL || fputs(text, f) < 0 || fclose(f)) {
        fprintf(stderr, "etude-nativedeploy-test: cannot write %s\n", path);
        exit(2);
    }
}

/** Make the directory `path`, ending the test when it cannot. */
static void dir(const char *path)
{
    if (mkdir(path, 0700)) {
        fprintf(stderr, "etude-nativedeploy-test: cannot make %s\n", path);
        exit(2);
    }
}

/** Whether `path` names anything, a dangling link included. */
static int exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

/** `text` with every `from` replaced by `to`, in a buffer the caller frees. */
static char *replaced(const char *text, const char *from, const char *to)
{
    size_t n = 0, flen = strlen(from), tlen = strlen(to);
    const char *at;
    char *out, *w;
    for (at = strstr(text, from); at; at = strstr(at + flen, from))
        n++;
    out = malloc(strlen(text) + n * tlen + 1);
    if (out == NULL)
        exit(2);
    for (w = out; (at = strstr(text, from)) != NULL; text = at + flen) {
        memcpy(w, text, (size_t)(at - text));
        w += at - text;
        memcpy(w, to, tlen);
        w += tlen;
    }
    strcpy(w, text);
    return out;
}

/** Make the tree of row `row`, TARGET with its staged archive and STATE,
 * and return TARGET. */
static const char *tree(const char *row)
{
    char path[1024];
    const char *target = fix_path(row);
    dir(target);
    fix_format(path, sizeof(path), "%s/p-%.16s", target, ARCHIVE);
    dir(path);
    fix_format(path, sizeof(path), "%s/p-%.16s/archive.sha256", target, ARCHIVE);
    put(path, ARCHIVE "\n");
    fix_format(path, sizeof(path), "%s/state", target);
    dir(path);
    return target;
}

/** Run the setup of a deployment to `target` with `bin` first on PATH, its
 * diagnostics in `out`; the status as system(3) gives it. */
static int setup(const char *target, const char *bin, const char *out)
{
    static char *cases[] = {NULL};
    struct nd_deploy d;
    char guard[4096], command[2048], *end, *a, *b, *c;
    const char *script;
    FILE *f;
    memset(&d, 0, sizeof(d));
    d.c.mode = "full";
    d.c.host = "machine";
    d.c.mac = "02:00:00:00:00:01";
    d.c.slot = "a";
    d.c.target = target;
    d.c.run = "r1";
    d.c.peer = d.c.network = d.c.interface = d.c.gateway = d.c.far = d.c.empty = d.c.silent = "x";
    d.c.architecture = "i686";
    d.c.purpose = "test";
    d.c.cases = cases;
    d.c.uid = 1000;
    snprintf(d.c.nonce, sizeof(d.c.nonce), "%s", TOKEN);
    snprintf(d.c.archive_sha, sizeof(d.c.archive_sha), "%s", ARCHIVE);
    snprintf(d.c.kernel_sha, sizeof(d.c.kernel_sha), "%s", ARCHIVE);
    sent[0] = '\0';
    if (nd_run(&d) != 0 || !nd_identity(&d, guard, sizeof(guard)) ||
        strncmp(sent, guard, strlen(guard)) || (end = strstr(sent, "exec \"$dest/tree/")) == NULL) {
        fprintf(stderr, "etude-nativedeploy-test: the run's command has no setup\n");
        exit(2);
    }
    *end = '\0';
    a = replaced(sent + strlen(guard), "/proc/sys/kernel/random/boot_id", fix_path("boot_id"));
    b = replaced(a, "/dev/shm/", fix_path("shm/"));
    c = replaced(b, "/proc/$$/stat", fix_path("stat"));
    script = fix_path("setup.sh");
    f = fopen(script, "w");
    if (f == NULL ||
        fprintf(f, "set -eu; %sprintf 'native-deploy-test: setup passed\\n' >&2\n", c) < 0 ||
        fclose(f))
        exit(2);
    free(a);
    free(b);
    free(c);
    fix_format(command, sizeof(command), "exec 2> /dev/null; PATH=%s:%s:$PATH /bin/sh %s > %s 2>&1",
               bin, fix_path("bin"), script, out);
    return system(command);
}

/** Whether the file `path` holds `text`. */
static int says(const char *path, const char *text)
{
    char buf[8192];
    size_t n;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

/** Whether STATE of `target` holds no name under gone. */
static int no_gone(const char *target)
{
    char command[1024];
    fix_format(command, sizeof(command), "ls -a %s/state | grep -q '^gone\\.'", target);
    return system(command) != 0;
}

/** Write a claim of run `run` in `target`, its receipt `identity`. */
static void claim(const char *target, const char *run, const char *identity)
{
    char path[1024];
    fix_format(path, sizeof(path), "%s/controller-%s", target, run);
    dir(path);
    fix_format(path, sizeof(path), "%s/controller-%s/owner", target, run);
    put(path, "feedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedfacefeedface\n");
    fix_format(path, sizeof(path), "%s/controller-%s/identity", target, run);
    put(path, identity);
}

/** Write a collection lock in `target`, with the owner record `owner`
 * unless it is NULL. */
static void lock(const char *target, const char *owner)
{
    char path[1024];
    fix_format(path, sizeof(path), "%s/state/collection.lock", target);
    dir(path);
    if (owner) {
        fix_format(path, sizeof(path), "%s/state/collection.lock/owner", target);
        put(path, owner);
    }
}

/** Make the files that stand for the machine, the PATH of each setup and
 * the `rm` that kills a setup. */
static void machine(const char *plain, const char *killer)
{
    char path[1024];

    dir(fix_path("shm"));
    put(fix_path("shm/etude-guest-boot"), TOKEN);
    put(fix_path("boot_id"), BOOT_ID "\n");
    put(fix_path("stat"), "1 (sh) S 1 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 4321 0 0\n");
    dir(fix_path("bin"));
    dir(plain);
    dir(killer);
    if (system("command -v flock > /dev/null 2>&1")) {
        put(fix_path("bin/flock"), "#!/bin/sh\nexit 0\n");
        chmod(fix_path("bin/flock"), 0755);
    }
    /* Kill the setup as it starts to remove a lock it has renamed, or once it
     * has removed the owner record of a lock it has not. */
    fix_format(path, sizeof(path), "%s/rm", killer);
    put(path, "#!/bin/sh\ncase \"$*\" in *gone.collection.lock*) kill -KILL $PPID; exit 1;; "
              "*collection.lock/owner*) /bin/rm \"$@\"; kill -KILL $PPID; exit 1;; "
              "esac\nexec /bin/rm \"$@\"\n");
    chmod(path, 0755);
}

/** Run the setup on controller claims. */
static void claim_rows(const char *plain)
{
    char path[1024], out[1024];
    const char *t;
    int status;

    /* An older controller of this boot keeps its claim. */
    t = tree("older");
    claim(t, "old", "4242 77 " TOKEN "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    status = setup(t, plain, out);
    CHECK(status == 0, "a claim of this boot's etude token fails the setup");
    fix_format(path, sizeof(path), "%s/controller-old/identity", t);
    CHECK(exists(path), "the setup removes a claim of this boot's etude token");
    CHECK(!says(out, "cleared"), "the setup clears a claim of this boot's etude token");
    fix_format(path, sizeof(path), "%s/controller-r1/identity", t);
    CHECK(exists(path), "the setup makes no claim beside an older one of this boot");

    /* A controller of another boot is cleared by name. */
    t = tree("otherclaim");
    claim(t, "old", "4243 78 " OTHER_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    status = setup(t, plain, out);
    CHECK(status == 0, "a claim of another boot fails the setup");
    fix_format(path, sizeof(path), "%s/controller-old", t);
    CHECK(!exists(path), "the setup keeps a claim of another boot");
    CHECK(says(out, "cleared: ") && says(out, "controller-old: pid 4243 of boot " OTHER_ID),
          "the setup clears a claim of another boot without naming it");
    CHECK(no_gone(t), "the setup leaves a cleared claim under gone.");
}

/** Run the setup on collection locks, `killer` first on the PATH of a setup
 * that is killed. */
static void lock_rows(const char *plain, const char *killer)
{
    char path[1024], out[1024];
    const char *t;
    int status;

    /* An older collection of this boot holds its lock. */
    t = tree("olderlock");
    lock(t, "4244 79 " TOKEN "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    status = setup(t, plain, out);
    CHECK(status != 0, "a lock of this boot's etude token passes the setup");
    fix_format(path, sizeof(path), "%s/state/collection.lock/owner", t);
    CHECK(exists(path), "the setup removes a lock of this boot's etude token");
    CHECK(says(out, "refused: ") && says(out, "held by pid 4244 of boot " TOKEN),
          "the setup refuses a lock of this boot's etude token without naming it");

    /* A lock of another boot, cleared by a setup killed after its rename. */
    t = tree("killed");
    lock(t, "4245 80 " OTHER_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    status = setup(t, killer, out);
    CHECK(status != 0, "the setup outlives its kill");
    fix_format(path, sizeof(path), "%s/state/collection.lock", t);
    CHECK(!exists(path), "a setup killed while it clears a lock leaves it at its name");
    fix_format(path, sizeof(path), "%s/state/gone.collection.lock/owner", t);
    CHECK(exists(path), "the killed setup did not stop where it removes the lock");
    fix_format(out, sizeof(out), "%s/out2", t);
    status = setup(t, plain, out);
    CHECK(status == 0, "a setup after a killed one fails");
    CHECK(no_gone(t), "the setup keeps what a killed setup left under gone.");

    /* A lock of another boot is cleared by name. */
    t = tree("otherlock");
    lock(t, "4246 81 " OTHER_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    status = setup(t, plain, out);
    CHECK(status == 0, "a lock of another boot fails the setup");
    fix_format(path, sizeof(path), "%s/state/collection.lock", t);
    CHECK(!exists(path), "the setup keeps a lock of another boot");
    CHECK(says(out, "collection.lock: held by pid 4246 of boot " OTHER_ID),
          "the setup clears a lock of another boot without naming it");
    CHECK(no_gone(t), "the setup leaves a cleared lock under gone.");
}

/** Run the setup on names under STATE/gone. and on a lock with no owner. */
static void left_rows(const char *plain)
{
    char path[1024], out[1024];
    const char *t;
    int status;

    /* What a stopped collection left under gone. is removed. */
    t = tree("leftover");
    fix_format(path, sizeof(path), "%s/state/gone.make-9", t);
    dir(path);
    fix_format(path, sizeof(path), "%s/state/gone.make-9/owner", t);
    put(path, "9 1 " BOOT_ID "\n");
    fix_format(path, sizeof(path), "%s/state/gone.lock-9", t);
    dir(path);
    fix_format(out, sizeof(out), "%s/out", t);
    status = setup(t, plain, out);
    CHECK(status == 0, "names under gone. fail the setup");
    CHECK(no_gone(t), "the setup keeps names under gone.");

    /* A lock with no owner record is still refused. */
    t = tree("ownerless");
    lock(t, NULL);
    fix_format(out, sizeof(out), "%s/out", t);
    status = setup(t, plain, out);
    CHECK(status != 0, "a lock with no owner record passes the setup");
    CHECK(says(out, "collection.lock: held with no owner record"),
          "the setup refuses a lock with no owner record without naming it");
}

int main(void)
{
    const char *plain = fix_path("plain"), *killer = fix_path("kill");

    machine(plain, killer);
    claim_rows(plain);
    lock_rows(plain, killer);
    left_rows(plain);
    if (failures)
        return failures > 255 ? 255 : failures;
    fix_passed();
    printf("etude-nativedeploy-test: passed\n");
    return 0;
}
