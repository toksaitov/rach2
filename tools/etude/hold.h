/*
 * hold.h: the system half of etude-hold, which etude_hold.c calls and
 * posix/hold.c and win/hold.c give: the processes, the record lock, the
 * files of the lock's directory, and the command run under the hold.
 */
#ifndef ETUDE_HOLD_H
#define ETUDE_HOLD_H

#include <stddef.h>

/* A process as the lock's owner record gives it in its `identity` line. */
struct hold_proc {
    long pid;
    long group;                  /* its process group; 0 on Windows */
    unsigned long long start;    /* its start: Linux's clock ticks since boot,
                                    macOS's seconds since the epoch, Windows's
                                    100 ns units since 1601 */
    unsigned long long fraction; /* microseconds past `start` on macOS, else 0 */
};

/** Give this process's pid. */
long hold_self(void);

/**
 * Read the identity of process `pid`.
 *
 * @return 1 when it runs, with its identity in `*p`; 0 when it has ended or
 *         is a zombie; -1 when the process table cannot be read, which says
 *         nothing of whether it runs.
 */
int hold_read(long pid, struct hold_proc *p);

/**
 * Give the parent of process `pid`, or 0 when it has none that can be
 * found. On Windows, where a parent's pid outlives the parent and may be
 * reused, a process that started after `pid` is not its parent.
 */
long hold_parent(long pid);

/**
 * List into `pids` at most `max` of the running processes of the process
 * group `group`, other than this one.
 *
 * @return how many were found, which may be more than `max`; 0 for a group
 *         of 0 or less, which Windows always gives.
 */
size_t hold_members(long group, long *pids, size_t max);

/**
 * Take the record lock on the file `path` for as long as this process
 * lives, making the file when it is missing. It is never removed, since two
 * holders could then lock two files at one path. No command this process
 * starts inherits it.
 *
 * @return 0 when taken; 1 when another process holds it; -1 with errno set
 *         when the file cannot be made or locked.
 */
int hold_guard(const char *path);

/** Make the directory `path`; 0, or -1 with errno EEXIST when it is there. */
int hold_mkdir(const char *path);

/** Rename `from` to `to`, replacing a file there; 0, or -1 with errno. */
int hold_rename(const char *from, const char *to);

/** Remove `path`, a file or a tree; 0, or -1 when something is left. */
int hold_remove(const char *path);

/** Say whether `path` was modified less than `seconds` ago. */
int hold_young(const char *path, double seconds);

/**
 * Note a stop: on POSIX systems a TERM, INT or HUP, on Windows a console's
 * close, logoff or shutdown. Before hold_start, the stop is only noted; after
 * it, the command's tree is stopped once. Called once, before the lock is
 * taken.
 */
void hold_catch(void);

/** Give the number of the first stop noted, a signal's on POSIX systems and
 * SIGTERM's for each stop on Windows, or 0. */
int hold_stopped(void);

/**
 * Start `argv` as a process tree of its own, a process group on POSIX
 * systems and a Job Object on Windows, held before it runs its program
 * until hold_go. It inherits the standard streams and the environment,
 * whose RACH2_TREE_HELD the caller has set. `name` prefixes the message of
 * a program that cannot be run.
 *
 * @return 0 with the tree's process group in `*group` (0 on Windows), or -1
 *         with errno set.
 */
int hold_start(const char *name, char *const *argv, long *group);

/** Let the command that hold_start started run its program. */
void hold_go(void);

/** End the command that hold_start started before it runs its program, and
 * wait for it. */
void hold_cancel(void);

/**
 * Wait for the command to end. A stop noted before or during the wait
 * reaches its tree once: SIGTERM to its group on POSIX systems; on Windows,
 * where a stop has no grace, the end of its job. On Windows the end of the
 * command also ends what it left in its job.
 *
 * @return its status as a shell gives it: the exit status, or 128 plus the
 *         number of the signal that ended it; on Windows its exit code, or
 *         137, as for SIGKILL, when a stop ended its job.
 */
int hold_wait(void);

/**
 * Run `argv` in place of this process, as a command run under the hold of an
 * ancestor: on POSIX systems by exec; on Windows as a child whose exit code
 * becomes this process's. Returns only when the command cannot be run.
 */
void hold_exec(char *const *argv);

#endif
