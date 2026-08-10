/**
 * pwauthatexit_hosttest: an acquire that cannot register its release.
 *
 * host-deps: utils/pwauth.c vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c
 *
 * The credential library promises that an ordinary `exit` cannot leak the
 * writer's lock, and it keeps that promise with `atexit`. `atexit` can fail:
 * musl holds thirty-two registrations in `.bss` and allocates a block for the
 * next thirty-two, so a writer that arrives when that allocation cannot be made
 * gets a refusal. An acquire that shrugged one off would hold a lock with
 * nothing left to drop it, and the next writer would wait ten seconds, refuse,
 * and send an operator looking for a process that has already gone.
 *
 * The case therefore needs an `atexit` that fails, and it is a program of its
 * own rather than a case inside `pwauth_hosttest` because that failure cannot
 * be scoped: the stand-in replaces the real one for the whole process, and
 * every other case in that program needs a release that is registered. One
 * binary per contradiction is cheaper than a switch that has to be right.
 *
 * Returns the failed-check count (0 == pass).
 */

#define _GNU_SOURCE /* getspnam, getrandom */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <shadow.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "pwauth.h"

#define LOCK "/tmp/pwauthatexit_hosttest.lock"

static int Failed;

/** Record one failed check, naming the contract that broke. */
static void fail(const char *what)
{
    fprintf(stderr, "pwauthatexit_hosttest: %s\n", what);
    Failed++;
}

/**
 * The host's `atexit(3)`, accepting the first registration and refusing the
 * rest.
 *
 * The library registers two functions and needs both, so refusing the second is
 * the partial failure that leaves one of them installed with no transaction
 * behind it. The accepted one is kept rather than really registered, since this
 * program has replaced the machinery that would have run it, and is called by
 * hand at the point the walk would have reached it. Nothing else here registers
 * anything, so the count belongs entirely to the library.
 */
static void (*Registered)(void);
static int Atexit_calls;

int atexit(void (*function)(void))
{
    Atexit_calls++;
    if (Atexit_calls == 1) {
        Registered = function;

        return 0;
    }

    return -1;
}

/** The disposition this program installs, recognizable by its address alone. */
static void keep_marker(int sig)
{
    (void)sig;
}

/**
 * The host's `getspnam(3)` and `getrandom(2)`, as `pwauth_hosttest` supplies
 * them: neither is reached by an acquire, and both must resolve for the link.
 */
struct spwd *getspnam(const char *name)
{
    (void)name;

    return NULL;
}

ssize_t getrandom(void *buffer, size_t length, unsigned int flags)
{
    (void)buffer;
    (void)length;
    (void)flags;
    errno = EIO;

    return -1;
}

/** True when something exists at `path`. */
static int present(const char *path)
{
    struct stat info;

    return stat(path, &info) == 0;
}

int main(void)
{
    int taken;
    int saved = dup(2);
    int quiet = open("/dev/null", O_WRONLY);

    unlink(LOCK);

    /* The refusal prints, and that line is the point of the refusal rather than
     * a fault, so it is kept out of a log every other test leaves empty. */
    if (saved >= 0 && quiet >= 0) {
        dup2(quiet, 2);
    }
    taken = cred_txn_begin_at("pwauthatexit_hosttest", LOCK, 0);
    if (saved >= 0) {
        dup2(saved, 2);
        close(saved);
    }
    if (quiet >= 0) {
        close(quiet);
    }
    if (taken == 0) {
        fail("an acquire that could not register its release took the lock anyway");
        cred_txn_end();
    }
    /* The refusal has to leave the name as it found it. An acquire that created
     * the lock and then gave up without removing it would hand every later
     * writer the stale lock this program exists to prevent. */
    if (present(LOCK)) {
        fail("a refused acquire left its lock behind");
        unlink(LOCK);
    }
    if (Atexit_calls < 2) {
        fail("the library did not attempt the second registration");
    }

    /* The half that survives the partial failure. One function is registered
     * and no transaction was ever held, so when the walk reaches it, it must
     * put nothing back: the disposition it saved is a zeroed `struct sigaction`
     * that would impose the default on the rest of `exit`, over whatever this
     * program installed for itself. */
    if (Registered == NULL) {
        fail("the library registered nothing at all");
    } else {
        struct sigaction mine, after;

        memset(&mine, 0, sizeof(mine));
        mine.sa_handler = keep_marker;
        sigemptyset(&mine.sa_mask);
        mine.sa_flags = 0;
        if (sigaction(SIGPIPE, &mine, NULL) != 0) {
            fail("could not install the disposition the surviving callback must not touch");
        } else {
            Registered();
            if (sigaction(SIGPIPE, NULL, &after) != 0) {
                fail("could not read back the disposition after the surviving callback ran");
            } else if (after.sa_handler != keep_marker) {
                fail("the surviving callback restored a disposition it never saved");
            }
        }
    }

    return Failed > 100 ? 100 : Failed;
}
