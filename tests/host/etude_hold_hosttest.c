/**
 * Check `etude lock tree`: the hold of a tree while a command runs, the hold
 * of an ancestor, the refusals, both takeovers, the stop passed to the
 * command and the hold a killed command leaves. Each row runs the command's
 * entry in a child of the test, in a private tree.
 * host-deps: tools/etude/posix/hold.c
 * host-cflags: -I tools/etude -Wall -Wextra -Werror
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "compat/fixdir_host.c"

#include "verbs.h"
#include "../../tools/etude/lock_tree.c"

static int fails;

#define CHECK(c, row)                                                         \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "etude-lock-tree-test: fail: %s: %s\n", row, #c); \
            fails++;                                                          \
        }                                                                     \
    } while (0)

/* What one run of the helper gave. */
struct ran {
    int status; /* as a shell gives it, or -1 when it did not end in time */
    char err[8192];
};

/* The process of a helper that runs beside the test, and its command's
 * process group, which exit ends. */
static pid_t beside;
static long beside_job;

/** End a helper left running beside the test, and its command's group. */
static void stop_beside(void)
{
    if (beside_job > 0)
        kill(-(pid_t)beside_job, SIGKILL);
    beside_job = 0;
    if (beside > 0) {
        kill(beside, SIGKILL);
        while (waitpid(beside, NULL, 0) < 0 && errno == EINTR)
            ;
        beside = 0;
    }
}

/** End the test at a signal, after stop_beside has run from exit. */
static void interrupted(int sig)
{
    exit(128 + sig);
}

/** Give the text of `path`, or "" when it cannot be read, in `buf`. */
static char *get(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n = 0;

    if (f != NULL) {
        n = fread(buf, 1, size - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

/** Write `text` to `path`. */
static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f == NULL || fputs(text, f) == EOF || fclose(f) != 0) {
        fprintf(stderr, "etude-lock-tree-test: cannot write %s\n", path);
        exit(1);
    }
}

/** Say whether `path` exists. */
static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/** Make an empty tree `name` in the fixture directory; its path in `tree`. */
static void make_tree(const char *name, char *tree, size_t size)
{
    char path[1200];

    fix_format(tree, size, "%s/%s", fix_dir(), name);
    mkdir(tree, 0700);
    fix_format(path, sizeof path, "%s/build", tree);
    mkdir(path, 0700);
}

/** Leave a lock directory in `tree` whose owner record is `record`, or none
 * when it is NULL. */
static void leave(const char *tree, const char *record)
{
    char path[1200];

    fix_format(path, sizeof path, "%s/build/.run.lock", tree);
    mkdir(path, 0700);
    if (record != NULL) {
        fix_format(path, sizeof path, "%s/build/.run.lock/owner", tree);
        put(path, record);
    }
}

/** Give the path of the lock directory of `tree`, or of `file` in it. */
static char *lock_path(const char *tree, const char *file, char *buf, size_t size)
{
    if (file == NULL)
        fix_format(buf, size, "%s/build/.run.lock", tree);
    else
        fix_format(buf, size, "%s/build/.run.lock/%s", tree, file);
    return buf;
}

/**
 * Start the helper's main in a child with the arguments `argv`, its standard
 * error into `err`. The child runs in the test's process group, as a command
 * a shell runs.
 */
static pid_t start(char **argv, const char *err)
{
    pid_t pid = fork();
    int argc, fd;

    if (pid != 0)
        return pid;
    fd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        dup2(fd, 2);
        close(fd);
    }
    for (argc = 0; argv[argc] != NULL; argc++)
        ;
    _exit(etude_lock_tree_main(argc, argv));
}

/** Wait for `pid` to end; its status as a shell gives it, or -1 when it
 * cannot be waited for. */
static int finish(pid_t pid)
{
    int status;

    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
}

/** Run the helper with `argv` to its end. */
static void run(char **argv, struct ran *r)
{
    const char *err = fix_path("hold.err");

    r->status = finish(start(argv, err));
    get(err, r->err, sizeof r->err);
}

/** Say whether the child `pid` has not ended, leaving it to be reaped. */
static int running(pid_t pid)
{
    siginfo_t info;

    memset(&info, 0, sizeof info);
    return waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == 0;
}

/** Wait until `path` exists or the child `pid` has ended; 1 when the path
 * exists. */
static int appears(const char *path, pid_t pid)
{
    while (!exists(path) && running(pid))
        usleep(10000); /* interval: the path is looked at again every 10 ms */
    return exists(path);
}

/** Give the value of the line `key` of the record `text` in `buf`, or "". */
static char *line_of(const char *text, const char *key, char *buf, size_t size)
{
    size_t n = strlen(key);
    const char *p;

    buf[0] = '\0';
    for (p = text; *p != '\0'; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p))
        if (strncmp(p, key, n) == 0 && p[n] == ' ') {
            snprintf(buf, size, "%.*s", (int)strcspn(p + n + 1, "\n"), p + n + 1);
            break;
        }
    return buf;
}

/** Give the `job` of the owner record of `tree`, or 0. */
static long job_of(const char *tree)
{
    char path[1200], text[4096], value[64];

    get(lock_path(tree, "owner", path, sizeof path), text, sizeof text);
    return strtol(line_of(text, "job", value, sizeof value), NULL, 10);
}

/** A pid that runs no process now: a child that has been reaped. */
static long ended_pid(void)
{
    pid_t pid = fork();

    if (pid == 0)
        _exit(0);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
    return (long)pid;
}

/**
 * The hold: the command runs with RACH2_TREE_HELD naming the helper, which
 * the owner record names whole, with the command's own group as its job;
 * the status is the command's, and the directory goes when it ends.
 */
static void test_hold(int run_hold)
{
    const char *row = run_hold ? "run hold" : "script hold";
    char tree[1024], cmd[4096], owner[1200], seen[1200], rec[4096], field_buf[256], want[64];
    char *argv[8];
    struct ran r;
    int n = 0;

    make_tree(run_hold ? "hold-run" : "hold-script", tree, sizeof tree);
    fix_format(seen, sizeof seen, "%s/seen", tree);
    lock_path(tree, "owner", owner, sizeof owner);
    fix_format(cmd, sizeof cmd, "{ echo \"held $RACH2_TREE_HELD\"; cat %s; } > %s; exit 3", owner,
               seen);
    argv[n++] = "etude";
    if (run_hold)
        argv[n++] = "-r";
    argv[n++] = tree;
    argv[n++] = "script";
    argv[n++] = "sh";
    argv[n++] = "-c";
    argv[n++] = cmd;
    argv[n] = NULL;
    run(argv, &r);
    get(seen, rec, sizeof rec);
    CHECK(r.status == 3, row);
    line_of(rec, "held", field_buf, sizeof field_buf);
    CHECK(field_buf[0] != '\0', row);
    fix_format(want, sizeof want, "%s", field_buf);
    CHECK(strcmp(line_of(rec, "pid", field_buf, sizeof field_buf), want) == 0, row);
    CHECK(strncmp(line_of(rec, "identity", field_buf, sizeof field_buf), want, strlen(want)) == 0 &&
              field_buf[strlen(want)] == ' ',
          row);
    CHECK(strncmp(line_of(rec, "args", field_buf, sizeof field_buf), "script sh -c ", 13) == 0,
          row);
    CHECK(line_of(rec, "since", field_buf, sizeof field_buf)[0] != '\0', row);
    CHECK(strtol(line_of(rec, "job", field_buf, sizeof field_buf), NULL, 10) > 0 &&
              strcmp(field_buf, want) != 0,
          row);
    CHECK(line_of(rec, "pgid", field_buf, sizeof field_buf)[0] != '\0', row);
    CHECK((strstr(rec, "\nheld script\n") != NULL) == !run_hold, row);
    CHECK(!exists(lock_path(tree, NULL, cmd, sizeof cmd)), row);
    fix_format(cmd, sizeof cmd, "%s/build/.run.guard", tree);
    CHECK(exists(cmd), row);
}

/**
 * The hold of an ancestor: with the tree held for the test, the helper runs
 * the command and takes nothing; a hold named for a process that is not an
 * ancestor is not the helper's.
 */
static void test_ancestor(void)
{
    char tree[1024], record[256], before[512], after[512], path[1200], self[32];
    char *argv[] = {"etude", tree, "script", "sh", "-c", "exit 6", NULL};
    struct ran r;

    make_tree("ancestor", tree, sizeof tree);
    fix_format(record, sizeof record, "pid %ld\nheld script\n", (long)getpid());
    leave(tree, record);
    fix_format(self, sizeof self, "%ld", (long)getpid());
    setenv("RACH2_TREE_HELD", self, 1);
    get(lock_path(tree, "owner", path, sizeof path), before, sizeof before);
    run(argv, &r);
    get(path, after, sizeof after);
    CHECK(r.status == 6, "ancestor: the command runs under the hold");
    CHECK(strcmp(before, after) == 0 && strcmp(before, record) == 0,
          "ancestor: the record is the ancestor's");
    fix_format(path, sizeof path, "%s/build/.run.guard", tree);
    CHECK(!exists(path), "ancestor: the helper takes no lock");

    fix_format(self, sizeof self, "%ld", ended_pid());
    setenv("RACH2_TREE_HELD", self, 1);
    fix_format(record, sizeof record, "pid %s\nheld script\n", self);
    put(lock_path(tree, "owner", path, sizeof path), record);
    run(argv, &r);
    CHECK(r.status == 1 && strstr(r.err, "result: refused: the tree is not free") != NULL,
          "ancestor: a hold of a process that is not an ancestor is not the helper's");
    unsetenv("RACH2_TREE_HELD");
}

/** The guard: a tree whose guard another process locks is refused. */
static void test_busy(void)
{
    char tree[1024], ready[1200], cmd[1300], err[1200];
    char *first[] = {"etude", tree, "first", "sh", "-c", cmd, NULL};
    char *second[] = {"etude", tree, "second", "true", NULL};
    struct ran r;
    int status;

    make_tree("busy", tree, sizeof tree);
    fix_format(ready, sizeof ready, "%s/ready", tree);
    fix_format(cmd, sizeof cmd, "touch %s; while :; do sleep 0.05; done", ready);
    fix_format(err, sizeof err, "%s/first.err", tree);
    beside = start(first, err);
    CHECK(appears(ready, beside), "busy: the first hold runs its command");
    beside_job = job_of(tree);
    run(second, &r);
    CHECK(r.status == 1, "busy: a second hold is refused");
    CHECK(strstr(r.err, "second: another run holds this tree: ") != NULL &&
              strstr(r.err, "first sh -c touch") != NULL &&
              strstr(r.err, "second: result: refused: the tree is not free") != NULL,
          "busy: the refusal names the holder");
    CHECK(running(beside), "busy: the first hold is left as it was");
    kill(beside, SIGTERM);
    status = finish(beside);
    CHECK(status == 128 + SIGTERM, "busy: the first hold ends at a stop");
    beside = 0;
    if (status < 0)
        stop_beside();
    beside_job = 0;
}

/** A refusal of a lock directory left as `record`, or with no record; the
 * directory stays. */
static void refused(const char *row, const char *record, const char *says)
{
    char tree[1024], before[1024], after[1024], path[1200];
    char *argv[] = {"etude", "-r", tree, "next", "true", NULL};
    struct ran r;

    make_tree(row, tree, sizeof tree);
    leave(tree, record);
    get(lock_path(tree, "owner", path, sizeof path), before, sizeof before);
    run(argv, &r);
    get(path, after, sizeof after);
    CHECK(r.status == 1, row);
    CHECK(strstr(r.err, says) != NULL, row);
    CHECK(strstr(r.err, "next: result: refused: the tree is not free") != NULL, row);
    CHECK(exists(lock_path(tree, NULL, path, sizeof path)) && strcmp(before, after) == 0, row);
}

/** The refusals of a lock directory that a run or a script left. */
static void test_refusals(void)
{
    struct hold_proc me;
    char record[512];
    long gone;
    pid_t work;
    int go[2];
    char byte;

    fix_format(record, sizeof record, "pid %ld\npgid %ld\nsince then\nargs left\nheld script\n",
               ended_pid(), (long)getpgrp());
    refused("left-held", record, "a script's hold on this tree was left: left, since then");

    CHECK(hold_read(getpid(), &me) == 1, "refusals: the test reads its own identity");
    fix_format(record, sizeof record,
               "pid %ld\npgid %ld\nidentity %ld %ld %llu %llu\njob 0\nsince then\nargs live\n",
               me.pid, me.group, me.pid, me.group, me.start, me.fraction);
    refused("live-run", record, "another run holds this tree: ");

    fix_format(record, sizeof record, "pid %ld\npgid %ld\nsince then\nargs old\n", (long)getpid(),
               (long)getpgrp());
    refused("live-old-run", record, "another run holds this tree: ");

    if (pipe(go) != 0)
        return;
    work = fork();
    if (work == 0) {
        setpgid(0, 0);
        close(go[1]);
        while (read(go[0], &byte, 1) < 0 && errno == EINTR)
            ;
        _exit(0);
    }
    close(go[0]);
    setpgid(work, work);
    gone = ended_pid();
    fix_format(record, sizeof record,
               "pid %ld\npgid %ld\nidentity %ld %ld 1 0\njob %ld\nsince then\nargs gone\n", gone,
               (long)getpgrp(), gone, (long)getpgrp(), (long)work);
    refused("survivors", record, "a run that is gone left work running: pids ");
    close(go[1]);
    while (waitpid(work, NULL, 0) < 0 && errno == EINTR)
        ;

    refused("taking", NULL, "another run is taking this tree");
}

/** A takeover of a lock directory left as `record`, or with no record and
 * `age` seconds old: the command runs, and the directory goes after it. */
static void taken(const char *row, const char *record, int age, const char *says)
{
    char tree[1024], path[1200], ran[1200];
    char *argv[] = {"etude", tree, "next", "touch", ran, NULL};
    struct timeval tv[2];
    struct ran r;

    make_tree(row, tree, sizeof tree);
    leave(tree, record);
    fix_format(ran, sizeof ran, "%s/ran", tree);
    if (age > 0) {
        gettimeofday(&tv[0], NULL);
        tv[0].tv_sec -= age;
        tv[1] = tv[0];
        utimes(lock_path(tree, NULL, path, sizeof path), tv);
    }
    run(argv, &r);
    CHECK(r.status == 0 && exists(ran), row);
    CHECK(strstr(r.err, says) != NULL, row);
    CHECK(!exists(lock_path(tree, NULL, path, sizeof path)), row);
}

/** The takeovers of a run's hold whose owner and work have ended, and of a
 * directory with no owner record. */
static void test_takeovers(void)
{
    char record[512];
    long gone = ended_pid();

    fix_format(record, sizeof record,
               "pid %ld\npgid %ld\nidentity %ld %ld 1 0\njob %ld\nsince then\nargs gone\n", gone,
               gone, gone, gone, gone);
    taken("taken-run", record, 0, "next: taking over the lock a run left: ");
    fix_format(record, sizeof record, "pid %ld\npgid %ld\nwork /nowhere\nsince then\nargs old\n",
               gone, gone);
    taken("taken-old-run", record, 0, "next: taking over the lock a run left: ");
    taken("taken-unrecorded", NULL, 60,
          "next: taking over the lock a run left with no owner record: ");
}

/**
 * The stop: a TERM sent to the helper reaches the command's group once, as
 * TERM, and the helper ends with the command's status and removes the
 * directory.
 */
static void test_stop(void)
{
    char tree[1024], ready[1200], log[1200], cmd[4096], path[1200], text[256];
    char *argv[] = {"etude", tree, "stopped", "sh", "-c", cmd, NULL};
    int status;

    make_tree("stop", tree, sizeof tree);
    fix_format(ready, sizeof ready, "%s/ready", tree);
    fix_format(log, sizeof log, "%s/log", tree);
    fix_format(cmd, sizeof cmd,
               "sh -c 'trap \"echo child >> %s; exit 0\" TERM; while :; do sleep 0.05; done' & "
               "trap 'echo term >> %s; sleep 0.5; wait; exit 5' TERM; touch %s; "
               "while :; do sleep 0.05; done",
               log, log, ready);
    fix_format(path, sizeof path, "%s/stop.err", tree);
    beside = start(argv, path);
    CHECK(appears(ready, beside), "stop: the command runs");
    beside_job = job_of(tree);
    kill(beside, SIGTERM);
    /* interval: a second TERM follows the first by 100 ms */
    usleep(100000);
    kill(beside, SIGTERM);
    status = finish(beside);
    beside = 0;
    if (status < 0)
        stop_beside();
    beside_job = 0;
    get(log, text, sizeof text);
    CHECK(status == 5, "stop: the helper's status is the command's");
    CHECK(strstr(text, "term\n") != NULL && strstr(text, "term\nterm") == NULL &&
              strstr(text, "child\n") != NULL && strstr(text, "child\nchild") == NULL,
          "stop: the command's group has one TERM");
    CHECK(!exists(lock_path(tree, NULL, path, sizeof path)), "stop: the directory goes");
}

/** The kill: a command killed with KILL leaves the directory, which says
 * where it is. */
static void test_killed(void)
{
    char tree[1024], path[1200];
    char *argv[] = {"etude", tree, "killed", "sh", "-c", "kill -KILL $$", NULL};
    struct ran r;

    make_tree("killed", tree, sizeof tree);
    run(argv, &r);
    CHECK(r.status == 137, "killed: the status is the kill's");
    CHECK(exists(lock_path(tree, "owner", path, sizeof path)), "killed: the directory stays");
    CHECK(strstr(r.err, "killed: the command was killed;") != NULL &&
              strstr(r.err, "stays until a person has looked") != NULL,
          "killed: the helper says the directory stays");
}

/** The scratch: `-s` removes the directory it names, and an unknown command
 * gives 127 and leaves no lock. */
static void test_scratch(void)
{
    char tree[1024], scratch[1200], path[1200];
    char *argv[] = {"etude", "-s", scratch, tree, "scratch", "/nonexistent/command", NULL};
    struct ran r;

    make_tree("scratch", tree, sizeof tree);
    fix_format(scratch, sizeof scratch, "%s/scratch", tree);
    mkdir(scratch, 0700);
    fix_format(path, sizeof path, "%s/scratch/file", tree);
    put(path, "");
    run(argv, &r);
    CHECK(!exists(scratch), "scratch: the directory goes");
    CHECK(r.status == 127 && strstr(r.err, "scratch: cannot run /nonexistent/command") != NULL,
          "scratch: a command that cannot run gives 127");
    CHECK(!exists(lock_path(tree, NULL, path, sizeof path)), "scratch: no lock is left");
}

int main(void)
{
    signal(SIGHUP, interrupted);
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);
    atexit(stop_beside);
    unsetenv("RACH2_TREE_HELD");
    test_hold(0);
    test_hold(1);
    test_ancestor();
    test_busy();
    test_refusals();
    test_takeovers();
    test_stop();
    test_killed();
    test_scratch();
    if (fails != 0) {
        fprintf(stderr, "etude-lock-tree-test: %d checks failed\n", fails);
        return 1;
    }
    fix_passed();
    printf("etude-lock-tree-test: pass\n");
    return 0;
}
