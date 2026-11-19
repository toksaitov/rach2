/*
 * etude_nativedeploy_hosttest.c: the setup that `native-deploy run` sends
 * before the native collection (tests/native/deploy_remote.c), run by the
 * host's sh on a state directory of the test's own, and the collection's
 * lock (tests/native/collection_lock.c) that it clears.
 *
 * host-deps: tests/native/deploy_remote.c tests/native/collection_lock.c
 * host-deps: tests/host/compat/fixdir_host.c
 * host-cflags: -I tests/native -I tools/etude/include -Wall -Wextra -Werror
 * host-cflags: -Dmkdir=nd_test_mkdir -Dunlink=nd_test_unlink
 *
 * mkdir and unlink in the linked sources call this program's, which can end
 * the process just after a named call, so that the collection's lock is
 * stopped between its steps. nd_ssh is replaced by one that keeps the
 * command; the test cuts out the machine's identity guard and the
 * collection's exec, and points the kernel's boot_id, the etude boot token
 * and /proc/$$/stat at files of its own.
 *
 * A controller claim or a collection lock of this boot, recorded with the
 * kernel's boot_id or, by an older deployment, with the etude boot token,
 * must be kept, and the lock refused by name; a claim or a lock of another
 * boot must be cleared with a line that names it, leaving no
 * STATE/collection.journal; a claim recorded with a token of hexadecimal
 * digits other than this boot's must be cleared while the token can be read,
 * and kept while it cannot; the new claim's receipt must record the boot_id.
 * A setup killed, as waitpid sees, once it has removed a stale lock's owner
 * record must leave its journal, and the next setup must pass and remove the
 * lock and the journal. A journal must have its lock removed when the lock
 * has no owner record, the one the journal names, even of this boot, or the
 * start of it, and
 * kept when the lock has another owner; a journal with no lock must be
 * removed alone; and a lock with no owner record and no journal must still
 * be refused. The collection's lock must be taken with this process's owner
 * record and no journal left, refused to a contender with no journal written
 * and the owner unchanged, refused while a journal is there, which it keeps,
 * and released with no lock or journal left; and the setup must clear a lock
 * that the collection took when a journal names its owner record. Where
 * procfs is, a claim of another boot whose recorded PID runs with the
 * recorded start must be kept when its boot is an etude token and cleared
 * when it is another boot_id, and a journal's lock kept while a process whose
 * arguments are `etude run native` runs, but not for a shell whose script
 * only names them. A journal's lock whose owner file is empty is cleared.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "collection.h"
#include "deploy.h"
#include "fixdir_host.h"

/* The sources call nd_test_mkdir and nd_test_unlink; this file, the C
 * library's. */
#undef mkdir
#undef unlink
int mkdir(const char *path, mode_t mode);
int unlink(const char *path);

#define BOOT_ID  "6c3e0f4a-1b2d-4e5f-8a9b-0c1d2e3f4a5b"
#define OTHER_ID "11111111-2222-4333-8444-555555555555"
#define TOKEN    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define OLDTOKEN "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
#define ARCHIVE  "a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0"
#define RECEIPT  " 4321 " BOOT_ID /* how the new receipt ends */

static int failures;
static const char *stop_mkdir;  /* a path ending that ends the process at its mkdir */
static const char *stop_unlink; /* likewise at its unlink */
static char sent[32768];        /* the command nd_ssh was given last */

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

/** Whether `path` ends with `end`, which may be NULL. */
static int ends(const char *path, const char *end)
{
    size_t n = strlen(path);
    return end && n >= strlen(end) && !strcmp(path + n - strlen(end), end);
}

/** mkdir(2), then the end of the process with status 9 when `path` ends
 * with stop_mkdir. */
int nd_test_mkdir(const char *path, mode_t mode)
{
    int status = mkdir(path, mode);
    if (ends(path, stop_mkdir))
        _exit(9);
    return status;
}

/** unlink(2), then the end of the process with status 9 when `path` ends
 * with stop_unlink. */
int nd_test_unlink(const char *path)
{
    int status = unlink(path);
    if (ends(path, stop_unlink))
        _exit(9);
    return status;
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

/** Read the first line of `path` into `line` of `size` bytes; 1, or 0. */
static int first(const char *path, char *line, size_t size)
{
    FILE *f = fopen(path, "r");
    int ok = f && fgets(line, (int)size, f) != NULL;
    if (f)
        fclose(f);
    if (ok)
        line[strcspn(line, "\n")] = '\0';
    return ok;
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

/** Write the setup of a deployment to `target` as the file `script`. */
static void script_write(const char *target, const char *script)
{
    static char *cases[] = {NULL};
    struct nd_deploy d;
    char guard[4096], *end, *a, *b, *c, *a2;
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
    if (system("stat -c %u / > /dev/null 2>&1")) {
        /* BSD stat, as macOS has, names the same fields otherwise. */
        a2 = replaced(c, "stat -c '%u:%a:%s'", "stat -f '%u:%Lp:%z'");
        free(c);
        c = a2;
    }
    f = fopen(script, "w");
    if (f == NULL ||
        fprintf(f, "set -eu; %sprintf 'native-deploy-test: setup passed\\n' >&2\n", c) < 0 ||
        fclose(f))
        exit(2);
    free(a);
    free(b);
    free(c);
}

/** Run the setup of a deployment to `target` in /bin/sh, with `bin` first on
 * PATH and its output in `out`; the status waitpid gives. */
static int setup(const char *target, const char *bin, const char *out)
{
    char path[4096];
    const char *script = fix_path("setup.sh");
    pid_t pid;
    int status;
    script_write(target, script);
    fix_format(path, sizeof(path), "%s:%s:%s", bin, fix_path("bin"), getenv("PATH"));
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        exit(2);
    if (pid == 0) {
        if (!freopen(out, "w", stdout) || dup2(fileno(stdout), 2) < 0 || setenv("PATH", path, 1))
            _exit(127);
        execl("/bin/sh", "sh", script, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0)
        ;
    return status;
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

/** Whether STATE of `target` holds no journal. */
static int no_journal(const char *target)
{
    char path[1024];
    fix_format(path, sizeof(path), "%s/state/collection.journal", target);
    return !exists(path);
}

/** Write the journal of STATE of `target` as `text`. */
static void journal(const char *target, const char *text)
{
    char path[1024];
    fix_format(path, sizeof(path), "%s/state/collection.journal", target);
    put(path, text);
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

/** Whether the lock of `target` is there. */
static int locked(const char *target)
{
    char path[1024];
    fix_format(path, sizeof(path), "%s/state/collection.lock", target);
    return exists(path);
}

/** Whether the claim of run `run` in `target` is there. */
static int claimed(const char *target, const char *run)
{
    char path[1024];
    fix_format(path, sizeof(path), "%s/controller-%s", target, run);
    return exists(path);
}

/** Whether the receipt of run `run` in `target` ends with RECEIPT. */
static int receipt_boot(const char *target, const char *run)
{
    char path[1024], line[256];
    size_t n = strlen(RECEIPT);
    fix_format(path, sizeof(path), "%s/controller-%s/identity", target, run);
    return first(path, line, sizeof(line)) && strlen(line) > n &&
           !strcmp(line + strlen(line) - n, RECEIPT);
}

/** Write the etude boot token as `text`, of mode 0644 whatever the umask. */
static void token(const char *text)
{
    put(fix_path("shm/etude-guest-boot"), text);
    if (chmod(fix_path("shm/etude-guest-boot"), 0644)) {
        fprintf(stderr, "etude-nativedeploy-test: cannot set the token's mode\n");
        exit(2);
    }
}

/** Format this process's start, field 22 of /proc/self/stat, into `out` of
 * `size` bytes; 1, or 0 where there is no procfs. */
static int self_start(char *out, size_t size)
{
    char line[1024], *at;
    int field = 2;
    if (!first("/proc/self/stat", line, sizeof(line)) || (at = strrchr(line, ')')) == NULL)
        return 0;
    for (; at && field < 22; field++)
        at = strchr(at + 1, ' ');
    return at && sscanf(at + 1, "%31[0-9]", out) == 1 && size > strlen(out);
}

/** Start `program` with the arguments `a0`, `a1` and `a2` in the fixture
 * directory, its standard input the read end of the pipe `hold`, and return
 * once its exec has succeeded, its PID in `pid`. */
static void held(const char *program, const char *a0, const char *a1, const char *a2, pid_t *pid,
                 int hold[2])
{
    int gate[2];
    char byte;
    fflush(NULL);
    if (pipe(gate) || pipe(hold))
        exit(2);
    fcntl(hold[1], F_SETFD, FD_CLOEXEC);
    *pid = fork();
    if (*pid < 0)
        exit(2);
    if (*pid == 0) {
        close(gate[0]);
        close(hold[1]);
        fcntl(gate[1], F_SETFD, FD_CLOEXEC);
        if (dup2(hold[0], 0) < 0 || chdir(fix_dir()))
            _exit(127);
        execl(program, a0, a1, a2, (char *)NULL);
        _exit(127);
    }
    /* The gate's end closes at the exec, once procfs shows the arguments. */
    close(hold[0]);
    close(gate[1]);
    while (read(gate[0], &byte, 1) > 0)
        ;
    close(gate[0]);
}

/** End the process of held, which reads to the end of its pipe, and reap it;
 * whether it exited 0, as only a fixture that ran does. */
static int unheld(pid_t pid, int hold[2])
{
    int status;
    close(hold[1]);
    while (waitpid(pid, &status, 0) < 0)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/** Run the setup where procfs shows a live controller and a live
 * collection, as on Linux and Rach2. */
static void live_rows(const char *plain)
{
    char out[1024], start[32], identity[256];
    const char *t;
    pid_t pid;
    int hold[2];

    if (!self_start(start, sizeof(start)))
        return;
    t = tree("liveclaim");
    fix_format(identity, sizeof(identity), "%ld %s " OLDTOKEN "\n", (long)getpid(), start);
    claim(t, "old", identity);
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a live claim of an old token fails the setup");
    CHECK(claimed(t, "old") && !says(out, "cleared"), "the setup clears a live claim of a token");

    t = tree("liveother");
    fix_format(identity, sizeof(identity), "%ld %s " OTHER_ID "\n", (long)getpid(), start);
    claim(t, "old", identity);
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a claim of another boot_id fails the setup");
    CHECK(!claimed(t, "old"), "the setup keeps a claim of another boot_id for a reused PID");

    t = tree("livelock");
    fix_format(identity, sizeof(identity), "%ld %s " OLDTOKEN "\n", (long)getpid(), start);
    lock(t, identity);
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) != 0, "a live lock of an old token passes the setup");
    CHECK(locked(t), "the setup clears a live lock of a token");

    t = tree("lockother");
    fix_format(identity, sizeof(identity), "%ld %s " OTHER_ID "\n", (long)getpid(), start);
    lock(t, identity);
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a lock of another boot_id fails the setup");
    CHECK(!locked(t), "the setup keeps a lock of another boot_id for a reused PID");

    /* A collection's arguments, and a shell that only names them. */
    t = tree("running");
    lock(t, NULL);
    journal(t, "release 1 2 " OTHER_ID "\n");
    held(fix_path("etude"), "etude", "run", "native", &pid, hold);
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) != 0, "a journal beside a running collection passes the setup");
    CHECK(locked(t) && !no_journal(t) && says(out, "a collection runs: pid "),
          "the setup clears a lock beside a running collection");
    CHECK(unheld(pid, hold), "the collection fixture did not run");

    t = tree("named");
    lock(t, NULL);
    journal(t, "release 1 2 " OTHER_ID "\n");
    held("/bin/sh", "sh", "-c", "read x; : etude run native", &pid, hold);
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a shell naming a collection fails the setup");
    CHECK(!locked(t) && no_journal(t), "a shell naming a collection keeps a journal's lock");
    CHECK(unheld(pid, hold), "the naming shell did not run");

    t = tree("namedpath");
    lock(t, NULL);
    journal(t, "release 1 2 " OTHER_ID "\n");
    held("/bin/sh", "sh", "-c", "read x; : /tmp/etude run native", &pid, hold);
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a shell naming a collection's path fails the setup");
    CHECK(!locked(t) && no_journal(t), "a shell naming a collection's path keeps a journal's lock");
    CHECK(unheld(pid, hold), "the shell naming a path did not run");
}

/** Make the files that stand for the machine, the PATH of each setup and
 * the `rm` that kills a setup. */
static void machine(const char *plain, const char *killer)
{
    char path[1024];

    dir(fix_path("shm"));
    token(TOKEN);
    put(fix_path("boot_id"), BOOT_ID "\n");
    put(fix_path("stat"), "1 (sh) S 1 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 4321 0 0\n");
    dir(fix_path("bin"));
    /* `etude run native`: sh, named etude, reading the script `run`. */
    if (symlink("/bin/sh", fix_path("etude")))
        exit(2);
    put(fix_path("run"), "read x; exit 0\n");
    dir(plain);
    dir(killer);
    if (system("command -v flock > /dev/null 2>&1")) {
        put(fix_path("bin/flock"), "#!/bin/sh\nexit 0\n");
        chmod(fix_path("bin/flock"), 0755);
    }
    /* Kill the setup once it has removed a lock's owner record. */
    fix_format(path, sizeof(path), "%s/rm", killer);
    put(path, "#!/bin/sh\ncase \"$*\" in *collection.lock/owner*) /bin/rm \"$@\" && "
              "kill -KILL $PPID; exit 1;; esac\nexec /bin/rm \"$@\"\n");
    chmod(path, 0755);
}

/** Run the setup on controller claims. */
static void claim_rows(const char *plain)
{
    char out[1024];
    const char *t;

    t = tree("thisboot");
    claim(t, "old", "4241 76 " BOOT_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a claim of this boot fails the setup");
    CHECK(claimed(t, "old") && !says(out, "cleared"), "the setup clears a claim of this boot");
    CHECK(receipt_boot(t, "r1"), "the new receipt does not record the boot_id");

    t = tree("older");
    claim(t, "old", "4242 77 " TOKEN "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a claim of this boot's etude token fails the setup");
    CHECK(claimed(t, "old") && !says(out, "cleared"),
          "the setup clears a claim of this boot's etude token");

    t = tree("oldtoken");
    claim(t, "old", "4243 78 " OLDTOKEN "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a claim of another boot's etude token fails the setup");
    CHECK(!claimed(t, "old") && says(out, "controller-old: pid 4243 of boot " OLDTOKEN),
          "the setup keeps a claim of another boot's etude token");

    /* With the token unreadable, a token of another boot may be this one's. */
    token("short");
    t = tree("notoken");
    claim(t, "old", "4244 79 " OLDTOKEN "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a setup with no token fails");
    CHECK(claimed(t, "old") && !says(out, "cleared"),
          "the setup with no token clears a claim of a token");
    token(TOKEN);

    t = tree("otherclaim");
    claim(t, "old", "4245 80 " OTHER_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a claim of another boot fails the setup");
    CHECK(!claimed(t, "old") && says(out, "controller-old: pid 4245 of boot " OTHER_ID),
          "the setup keeps a claim of another boot or clears it without naming it");
    CHECK(no_journal(t), "the setup leaves a journal after a cleared claim");
}

/** Run the setup on collection locks, `killer` first on the PATH of a setup
 * that is killed. */
static void lock_rows(const char *plain, const char *killer)
{
    char out[1024], path[1024];
    const char *t;
    int status;

    t = tree("thislock");
    lock(t, "4250 85 " BOOT_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) != 0, "a lock of this boot passes the setup");
    CHECK(locked(t) && says(out, "refused: ") && says(out, "held by pid 4250 of boot " BOOT_ID),
          "the setup does not refuse a lock of this boot by name");

    t = tree("olderlock");
    lock(t, "4251 86 " TOKEN "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) != 0, "a lock of this boot's etude token passes the setup");
    CHECK(locked(t) && says(out, "held by pid 4251 of boot " TOKEN),
          "the setup does not refuse a lock of this boot's etude token by name");

    t = tree("killed");
    lock(t, "4252 87 " OTHER_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    status = setup(t, killer, out);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "the setup was not killed");
    CHECK(locked(t) && !no_journal(t), "a setup killed while it clears a lock leaves no journal");
    fix_format(path, sizeof(path), "%s/state/collection.lock/owner", t);
    CHECK(!exists(path), "the setup was killed before it removed the lock's owner record");
    fix_format(out, sizeof(out), "%s/out2", t);
    CHECK(setup(t, plain, out) == 0, "a setup after a killed one fails");
    CHECK(!locked(t) && no_journal(t), "a setup after a killed one keeps the lock or journal");
    CHECK(says(out, "collection.lock: left by a stopped change: clear 4252 87 " OTHER_ID),
          "a setup after a killed one clears its lock without naming it");

    t = tree("otherlock");
    lock(t, "4253 88 " OTHER_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a lock of another boot fails the setup");
    CHECK(!locked(t) && says(out, "collection.lock: held by pid 4253 of boot " OTHER_ID),
          "the setup keeps a lock of another boot or clears it without naming it");
    CHECK(no_journal(t), "the setup leaves a journal after a cleared lock");
}

/** Run the setup on journals a stopped process left, and on a lock with no
 * owner record. */
static void journal_rows(const char *plain)
{
    char out[1024];
    const char *t;

    t = tree("release");
    lock(t, "4260 90 " BOOT_ID "\n");
    journal(t, "release 4260 90 " BOOT_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a journal of a stopped release fails the setup");
    CHECK(!locked(t) && no_journal(t), "the setup keeps the lock or journal of a stopped release");
    CHECK(says(out, "collection.lock: left by a stopped change: release 4260 90 " BOOT_ID),
          "the setup clears the lock of a stopped release without naming it");

    t = tree("made");
    lock(t, NULL);
    journal(t, "make 4261 91 " BOOT_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a journal of a stopped make fails the setup");
    CHECK(!locked(t) && no_journal(t), "the setup keeps the ownerless lock of a stopped make");

    t = tree("partial");
    lock(t, "4265 95");
    journal(t, "make 4265 95 " BOOT_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a journal of a stopped owner write fails the setup");
    CHECK(!locked(t) && no_journal(t), "the setup keeps a lock its journal's owner began");

    t = tree("emptyowner");
    lock(t, "");
    journal(t, "make 4266 96 " BOOT_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a journal beside an empty owner record fails the setup");
    CHECK(!locked(t) && no_journal(t), "the setup keeps a lock with an empty owner record");

    t = tree("contender");
    lock(t, "4262 92 " BOOT_ID "\n");
    journal(t, "make 4263 93 " BOOT_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) != 0, "a lock of another owner than its journal's passes");
    CHECK(locked(t) && no_journal(t) && says(out, "held by pid 4262 of boot " BOOT_ID),
          "the setup removes a lock of another owner than its journal's");

    t = tree("journal");
    journal(t, "make 4264 94 " BOOT_ID "\n");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a journal with no lock fails the setup");
    CHECK(!says(out, "cleared") && no_journal(t), "the setup mishandles a journal with no lock");

    t = tree("ownerless");
    lock(t, NULL);
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) != 0, "a lock with no owner record passes the setup");
    CHECK(says(out, "collection.lock: held with no owner record"),
          "the setup refuses a lock with no owner record without naming it");
}

/** Run `take`, then `release` when it is set, on the lock of STATE `state` in
 * a child, which ends just after the mkdir or unlink of a path ending with
 * `at_mkdir` or `at_unlink`; whether it ended so. */
static int stopped(const char *state, const char *lockdir, int release, const char *at_mkdir,
                   const char *at_unlink)
{
    pid_t pid;
    int status;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        exit(2);
    if (pid == 0) {
        stop_mkdir = at_mkdir;
        stop_unlink = release ? NULL : at_unlink;
        if (!nc_lock_take(state, lockdir))
            _exit(1);
        stop_unlink = at_unlink;
        if (release)
            nc_lock_release(state, lockdir);
        _exit(0);
    }
    while (waitpid(pid, &status, 0) < 0)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 9;
}

/** Stop the collection's lock between its steps and clear what it left. */
static void stop_rows(const char *plain)
{
    char state[1024], lockdir[1024], owner[1024], line[256], out[1024];
    const char *t = tree("stopmake");

    fix_format(state, sizeof(state), "%s/state", t);
    fix_format(lockdir, sizeof(lockdir), "%s/collection.lock", state);
    fix_format(owner, sizeof(owner), "%s/owner", lockdir);
    CHECK(stopped(state, lockdir, 0, "/collection.lock", NULL), "the take did not stop");
    fix_format(line, sizeof(line), "%s/collection.journal", state);
    CHECK(locked(t) && !exists(owner) && first(line, line, sizeof(line)) &&
              !strncmp(line, "make ", 5),
          "a take stopped after its mkdir leaves no make journal beside an ownerless lock");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a setup after a stopped take fails");
    CHECK(!locked(t) && no_journal(t) && says(out, "left by a stopped change: make "),
          "a setup after a stopped take does not clear its lock by name");

    t = tree("stoprelease");
    fix_format(state, sizeof(state), "%s/state", t);
    fix_format(lockdir, sizeof(lockdir), "%s/collection.lock", state);
    fix_format(owner, sizeof(owner), "%s/owner", lockdir);
    CHECK(stopped(state, lockdir, 1, NULL, "/collection.lock/owner"), "the release did not stop");
    fix_format(line, sizeof(line), "%s/collection.journal", state);
    CHECK(locked(t) && !exists(owner) && first(line, line, sizeof(line)) &&
              !strncmp(line, "release ", 8),
          "a release stopped after its unlink leaves no release journal beside an ownerless lock");
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a setup after a stopped release fails");
    CHECK(!locked(t) && no_journal(t) && says(out, "left by a stopped change: release "),
          "a setup after a stopped release does not clear its lock by name");
}

/** Take and release the collection's lock, and clear one it took. */
static void collection_rows(const char *plain)
{
    char state[1024], lockdir[1024], owner[1024], record[256], again[256], text[1024];
    char out[1024], pid[32];
    const char *t = tree("writer");

    fix_format(state, sizeof(state), "%s/state", t);
    fix_format(lockdir, sizeof(lockdir), "%s/collection.lock", state);
    fix_format(owner, sizeof(owner), "%s/owner", lockdir);
    fix_format(pid, sizeof(pid), "%ld ", (long)getpid());
    CHECK(nc_lock_take(state, lockdir), "the collection cannot take its lock");
    CHECK(first(owner, record, sizeof(record)) && !strncmp(record, pid, strlen(pid)),
          "the collection's lock does not record its PID");
    CHECK(no_journal(t), "the collection leaves a journal after it takes its lock");
    CHECK(!nc_lock_take(state, lockdir), "a contender takes a held lock");
    CHECK(no_journal(t), "a contender writes a journal");
    CHECK(first(owner, again, sizeof(again)) && !strcmp(record, again),
          "a contender changes the lock's owner record");
    CHECK(nc_lock_release(state, lockdir) == 0, "the collection cannot release its lock");
    CHECK(!locked(t) && no_journal(t), "the release leaves its lock or journal");

    journal(t, "release 1 1 -\n");
    CHECK(!nc_lock_take(state, lockdir), "the collection takes its lock beside a journal");
    CHECK(!locked(t) && !no_journal(t), "the collection removes a journal it finds");
    fix_format(text, sizeof(text), "%s/collection.journal", state);
    unlink(text);

    CHECK(nc_lock_take(state, lockdir), "the collection cannot take its lock again");
    CHECK(first(owner, record, sizeof(record)), "the collection's lock has no owner record");
    fix_format(text, sizeof(text), "release %s\n", record);
    journal(t, text);
    fix_format(out, sizeof(out), "%s/out", t);
    CHECK(setup(t, plain, out) == 0, "a journal of the collection's own lock fails the setup");
    CHECK(!locked(t) && no_journal(t), "the setup keeps a lock its journal names");
}

int main(void)
{
    const char *plain = fix_path("plain"), *killer = fix_path("kill");

    machine(plain, killer);
    claim_rows(plain);
    lock_rows(plain, killer);
    journal_rows(plain);
    collection_rows(plain);
    stop_rows(plain);
    live_rows(plain);
    if (failures)
        return failures > 255 ? 255 : failures;
    fix_passed();
    printf("etude-nativedeploy-test: passed\n");
    return 0;
}
