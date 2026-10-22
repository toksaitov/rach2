/*
 * etude_hold.c: etude-hold, which holds a tree's run lock while one command
 * runs, so that no suite builds or boots the tree while the command changes
 * it. tools/etude/Readme.md gives its usage.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hold.h"

/* The longest path the helper makes, and the longest `args` it records. */
#define PATH_MAX_LEN 4096
#define ARGS_MAX     1000

/* The most survivors a refusal names. */
#define SURVIVORS 32

/* The seconds a lock directory with no owner record is taken to be one that
 * a run is taking this moment. */
#define TAKING_SECONDS 10

/* What the lock's owner record says, or what a record that is not there
 * leaves of it: no pid. */
struct record {
    long pid;       /* 0 when the record has none */
    long pgid;      /* the holder's process group */
    long job;       /* the command's process group; 0 when none is given */
    int identified; /* it has an `identity` line, in `id` */
    int held;       /* it has a `held` line: a script's hold */
    struct hold_proc id;
    char since[64];
    char args[ARGS_MAX + 1];
};

/* The helper's arguments and the paths of the lock. */
struct hold {
    const char *name;
    char *const *argv;
    int run; /* -r: a run's hold, which may be taken over */
    char guard[PATH_MAX_LEN];
    char lock[PATH_MAX_LEN];
    char owner[PATH_MAX_LEN];
};

static int usage(void)
{
    fprintf(stderr, "usage: etude-hold [-r] [-s SCRATCH] TREE NAME COMMAND [ARG...]\n       "
                    "etude-hold -q TREE\n");
    return 2;
}

/** Copy the words of `line` after the key `key` and a space into `out` of
 * `size` bytes; 1 when the line has the key. */
static int field(const char *line, const char *key, char *out, size_t size)
{
    size_t n = strlen(key);

    if (strncmp(line, key, n) != 0 || line[n] != ' ')
        return 0;
    snprintf(out, size, "%s", line + n + 1);
    return 1;
}

/**
 * Read the owner record `path` into `*r`. A record that is not there, or
 * that has no `pid` line, gives a pid of 0, and the fields it lacks are
 * empty.
 */
static void read_record(const char *path, struct record *r)
{
    char line[ARGS_MAX + 64], word[ARGS_MAX + 1];
    FILE *f = fopen(path, "r");

    memset(r, 0, sizeof *r);
    while (f != NULL && fgets(line, sizeof line, f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (field(line, "pid", word, sizeof word))
            r->pid = strtol(word, NULL, 10);
        else if (field(line, "pgid", word, sizeof word))
            r->pgid = strtol(word, NULL, 10);
        else if (field(line, "job", word, sizeof word))
            r->job = strtol(word, NULL, 10);
        else if (field(line, "identity", word, sizeof word))
            r->identified = sscanf(word, "%ld %ld %llu %llu", &r->id.pid, &r->id.group,
                                   &r->id.start, &r->id.fraction) == 4;
        else if (field(line, "since", word, sizeof word))
            snprintf(r->since, sizeof r->since, "%.63s", word);
        else if (field(line, "args", word, sizeof word))
            snprintf(r->args, sizeof r->args, "%s", word);
        else if (strncmp(line, "held ", 5) == 0)
            r->held = 1;
    }
    if (f != NULL)
        fclose(f);
    if (r->pid <= 0)
        r->pid = 0;
}

/**
 * Say whether the tree is held for an ancestor of this process:
 * RACH2_TREE_HELD names a process, the owner record names the same one, and
 * it is this process, its parent, or that parent's, up to the first process.
 */
static int held_for_ancestor(const struct hold *h)
{
    const char *env = getenv("RACH2_TREE_HELD");
    long held = env != NULL ? strtol(env, NULL, 10) : 0, pid;
    struct record r;
    int depth;

    if (held <= 1)
        return 0;
    read_record(h->owner, &r);
    if (r.pid != held)
        return 0;
    for (pid = hold_self(), depth = 0; pid > 1 && depth < 4096; pid = hold_parent(pid), depth++)
        if (pid == held)
            return 1;
    return 0;
}

/** Say why the tree is not free, then that the request is refused; 0. */
static int refuse(const struct hold *h, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", h->name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n%s: result: refused: the tree is not free\n", h->name);
    return 0;
}

/**
 * Say whether the owner of record `r` runs: 1 when it does, 0 when it has
 * ended, -1 when that cannot be told. A record with an identity is its
 * owner's while the process of that pid has that start; one without, as an
 * older tests/run writes, while the process of that pid is in the recorded
 * group.
 */
static int owner_runs(const struct record *r)
{
    struct hold_proc p;
    int state = hold_read(r->pid, &p);

    if (state != 1)
        return state;
    if (r->identified)
        return p.start == r->id.start && p.fraction == r->id.fraction;
    return p.group == r->pgid;
}

/**
 * Move the lock directory aside by rename, which one taker at most can do,
 * make it again for this process and remove what was moved. 1 when it is
 * this process's.
 */
static int take_over(const struct hold *h)
{
    char old[PATH_MAX_LEN + 32];
    int taken;

    snprintf(old, sizeof old, "%s.old.%ld", h->lock, hold_self());
    taken = hold_rename(h->lock, old) == 0 && hold_mkdir(h->lock) == 0;
    hold_remove(old);
    if (!taken)
        return refuse(h, "another run took the tree first");
    return 1;
}

/**
 * Make the lock directory for this process, the guard being held. A
 * directory left by a script, whose record says `held`, is never taken
 * over; one left by a run is taken over when its owner and every process of
 * its group have ended; and one with no owner record, or a record with no
 * pid, once it is ten seconds old, since a younger one may be a run's that
 * takes the tree this moment.
 *
 * @return 1 when the directory is this process's; 0 after a refusal.
 */
static int take(const struct hold *h)
{
    struct record r;
    long pids[SURVIVORS];
    char list[SURVIVORS * 24];
    size_t n, i, len;
    int runs;

    if (hold_mkdir(h->lock) == 0)
        return 1;
    if (errno != EEXIST)
        return refuse(h, "cannot make %s: %s", h->lock, strerror(errno));
    read_record(h->owner, &r);
    if (r.pid == 0) {
        if (hold_young(h->lock, TAKING_SECONDS))
            return refuse(h, "another run is taking this tree");
        fprintf(stderr, "%s: taking over the lock a run left with no owner record: %s\n", h->name,
                h->lock);
        return take_over(h);
    }
    if (r.held)
        return refuse(h,
                      "a script's hold on this tree was left: %s, since %s; its work may still "
                      "run; stop its work and remove %s",
                      r.args, r.since, h->lock);
    runs = owner_runs(&r);
    if (runs > 0)
        return refuse(h, "another run holds this tree: %ld, %s, since %s", r.pid, r.args, r.since);
    if (runs < 0)
        return refuse(h, "cannot tell whether the run that holds this tree, %ld, still runs",
                      r.pid);
    n = hold_members(r.identified ? r.job : r.pgid, pids, SURVIVORS);
    if (n > 0) {
        for (i = 0, len = 0; i < n && i < SURVIVORS; i++)
            len += (size_t)snprintf(list + len, sizeof list - len, "%s%ld", i ? " " : "", pids[i]);
        return refuse(h,
                      "a run that is gone left work running: pids %s; stop them before the "
                      "next run",
                      list);
    }
    fprintf(stderr, "%s: taking over the lock a run left: %ld, %s\n", h->name, r.pid, r.args);
    return take_over(h);
}

/** Give the name and the command's words as one line of at most ARGS_MAX
 * bytes, for the record's `args`. */
static void join_args(const struct hold *h, char *out)
{
    size_t len = (size_t)snprintf(out, ARGS_MAX + 1, "%s", h->name), i;
    char *const *a;

    for (a = h->argv; *a != NULL && len < ARGS_MAX; a++)
        len += (size_t)snprintf(out + len, ARGS_MAX + 1 - len, " %s", *a);
    for (i = 0; out[i] != '\0'; i++)
        if (out[i] == '\n' || out[i] == '\r')
            out[i] = ' ';
}

/**
 * Write the owner record for this process, whose command's group is `job`,
 * as `owner.new` and rename it into place, so that a reader finds it whole
 * or not at all. 0, or -1 with errno set.
 */
static int write_record(const struct hold *h, long job)
{
    char path[PATH_MAX_LEN + 8], args[ARGS_MAX + 1], since[64];
    struct hold_proc me;
    time_t now = time(NULL);
    FILE *f;
    int bad;

    if (hold_read(hold_self(), &me) != 1) {
        errno = ESRCH;
        return -1;
    }
    if (strftime(since, sizeof since, "%Y-%m-%d %H:%M:%S", localtime(&now)) == 0)
        snprintf(since, sizeof since, "unknown");
    join_args(h, args);
    snprintf(path, sizeof path, "%s.new", h->owner);
    if ((f = fopen(path, "w")) == NULL)
        return -1;
    fprintf(f, "pid %ld\npgid %ld\nidentity %ld %ld %llu %llu\njob %ld\nsince %s\nargs %s\n",
            me.pid, me.group, me.pid, me.group, me.start, me.fraction, job, since, args);
    if (!h->run)
        fprintf(f, "held script\n");
    bad = ferror(f);
    if (fclose(f) != 0 || bad)
        return -1;
    return hold_rename(path, h->owner);
}

/** Remove the lock directory when the owner record still names this
 * process. */
static void release(const struct hold *h)
{
    struct record r;

    read_record(h->owner, &r);
    if (r.pid == hold_self())
        hold_remove(h->lock);
}

/** Set RACH2_TREE_HELD to `pid` for the command; 0, or -1 with errno. */
static int set_held(const char *pid)
{
#ifdef _WIN32
    errno = _putenv_s("RACH2_TREE_HELD", pid);
    return errno != 0 ? -1 : 0;
#else
    return setenv("RACH2_TREE_HELD", pid, 1);
#endif
}

/**
 * Hold the tree and run the command: take the guard and the directory,
 * start the command with RACH2_TREE_HELD naming this process, write the
 * record, let the command run and wait for it.
 *
 * @return the command's status; 1 after a refusal; 127 when the command
 *         could not be started; 128 plus a signal that stopped the helper
 *         before the command started.
 */
static int hold_tree(struct hold *h)
{
    char self[32];
    struct record r;
    long job;
    int status;

    hold_catch();
    status = hold_guard(h->guard);
    if (status < 0) {
        fprintf(stderr, "%s: cannot lock %s: %s\n", h->name, h->guard, strerror(errno));
        return 1;
    }
    if (status > 0) {
        read_record(h->owner, &r);
        if (r.pid == 0)
            refuse(h, "another run holds this tree: unknown, unknown");
        else
            refuse(h, "another run holds this tree: %ld, %s", r.pid, r.args);
        return 1;
    }
    if (!take(h))
        return 1;
    if (hold_stopped() != 0) {
        hold_remove(h->lock);
        return 128 + hold_stopped();
    }
    snprintf(self, sizeof self, "%ld", hold_self());
    if (set_held(self) != 0 || hold_start(h->name, h->argv, &job) != 0) {
        fprintf(stderr, "%s: cannot start %s: %s\n", h->name, h->argv[0], strerror(errno));
        hold_remove(h->lock);
        return 127;
    }
    if (write_record(h, job) != 0) {
        fprintf(stderr, "%s: cannot write %s: %s\n", h->name, h->owner, strerror(errno));
        hold_cancel();
        hold_remove(h->lock);
        return 1;
    }
    hold_go();
    status = hold_wait();
    if (status == 137) {
        if (h->run)
            fprintf(stderr,
                    "%s: the command was killed; its work may still run; %s stays until a run "
                    "takes it over, once nothing of this one is left\n",
                    h->name, h->lock);
        else
            fprintf(stderr,
                    "%s: the command was killed; its work may still run; %s stays until a "
                    "person has looked\n",
                    h->name, h->lock);
        return status;
    }
    release(h);
    return status;
}

int main(int argc, char **argv)
{
    struct hold h;
    const char *scratch = NULL, *tree;
    int i, query = 0;

    memset(&h, 0, sizeof h);
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-q") == 0)
            query = 1;
        else if (strcmp(argv[i], "-r") == 0)
            h.run = 1;
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            scratch = argv[++i];
        else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else
            return usage();
    }
    if ((query ? argc - i != 1 : argc - i < 3) || argv[i][0] == '\0' ||
        (!query && argv[i + 1][0] == '\0'))
        return usage();
    tree = argv[i];
    h.name = query ? "etude-hold" : argv[i + 1];
    h.argv = argv + i + 2;
    if (scratch != NULL)
        hold_remove(scratch);
    if ((size_t)snprintf(h.guard, sizeof h.guard, "%s/build/.run.guard", tree) >= sizeof h.guard ||
        (size_t)snprintf(h.lock, sizeof h.lock, "%s/build/.run.lock", tree) >= sizeof h.lock ||
        (size_t)snprintf(h.owner, sizeof h.owner, "%s/owner", h.lock) >= sizeof h.owner) {
        fprintf(stderr, "%s: the tree's path is too long\n", h.name);
        return 2;
    }
    if (query)
        return held_for_ancestor(&h) ? 0 : 1;
    if (held_for_ancestor(&h)) {
        hold_exec(h.argv);
        fprintf(stderr, "%s: cannot run %s: %s\n", h.name, h.argv[0], strerror(errno));
        return 127;
    }
    snprintf(h.lock, sizeof h.lock, "%s/build", tree);
    if (hold_mkdir(h.lock) != 0 && errno != EEXIST) {
        fprintf(stderr, "%s: cannot make %s: %s\n", h.name, h.lock, strerror(errno));
        return 1;
    }
    snprintf(h.lock, sizeof h.lock, "%s/build/.run.lock", tree);
    return hold_tree(&h);
}
