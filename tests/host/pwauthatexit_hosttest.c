/**
 * pwauthatexit_hosttest: an acquire of the credential lock that cannot
 * register its release.
 *
 * host-deps: utils/pwauth.c vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c
 *
 * `utils/pwauth.c` drops a writer's lock at an ordinary `exit` through
 * functions it registers with `atexit`, and `atexit` can fail: musl has
 * thirty-two registrations in `.bss` and allocates a block for the next
 * thirty-two, so a registration fails when that allocation does. An acquire
 * that went on after the failure would hold a lock that nothing drops, and
 * the next writer would wait ten seconds and refuse, with the holder gone.
 *
 * The stand-in `atexit` replaces the host's for the whole process, and every
 * row of `pwauth_hosttest` needs its release registered, so this case is a
 * program of its own.
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

static int Failed;

/** Record one failed check, naming the contract that broke. */
static void fail(const char *what)
{
    fprintf(stderr, "pwauthatexit_hosttest: %s\n", what);
    Failed++;
}

static void (*Registered)(void); /* the one registration taken, or NULL */
static int Atexit_calls;         /* calls of `atexit`, all from the library */

/**
 * The host's `atexit(3)`, taking the first registration and refusing the
 * rest.
 *
 * The library registers two functions and needs both, so refusing the second
 * leaves one of them registered with no transaction held. The one taken is
 * kept in `Registered`, since this function replaces the host's and nothing
 * else would run it; `main` calls it.
 */
int atexit(void (*function)(void))
{
    Atexit_calls++;
    if (Atexit_calls == 1) {
        Registered = function;

        return 0;
    }

    return -1;
}

/** The `SIGPIPE` handler this program installs, known by its address. */
static void keep_marker(int sig)
{
    (void)sig;
}

char *__crypt_sha512(const char *key, const char *setting, char *output);

/**
 * The host's `crypt(3)`: the vendored musl SHA-512 the guest verifies with,
 * so the link is the same on a host whose libc keeps `crypt` in a separate
 * library, or has only the DES one.
 */
char *crypt(const char *key, const char *setting)
{
    static char output[256];

    return __crypt_sha512(key, setting, output);
}

/**
 * The host's `getspnam(3)`, finding no entry. An acquire reaches neither this
 * nor `getrandom`, and both must resolve for the link.
 */
struct spwd *getspnam(const char *name)
{
    (void)name;

    return NULL;
}

/** The host's `getrandom(2)`, failing with `EIO`. */
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

/**
 * Acquire a lock with the second registration refused, and require the
 * acquire to fail, to leave no lock file, and to have tried both
 * registrations. Then call the one function registered, as `exit` would: no
 * transaction was held, so it must leave alone a `SIGPIPE` disposition it
 * never saved, where putting back its zeroed copy would impose the default on
 * the rest of `exit`.
 *
 * The library prints the refusal, so standard error goes to `/dev/null`
 * during the acquire, to keep the log of a pass empty.
 */
int main(void)
{
    char dir[] = "/tmp/pwauthatexit_hosttest.XXXXXX";
    char lock[64];
    int taken;
    int saved = dup(2);
    int quiet = open("/dev/null", O_WRONLY);

    if (!mkdtemp(dir)) {
        fail("no directory could be made for the lock");
        return 1;
    }
    snprintf(lock, sizeof(lock), "%s/lock", dir);

    if (saved >= 0 && quiet >= 0) {
        dup2(quiet, 2);
    }
    taken = cred_txn_begin_at("pwauthatexit_hosttest", lock, 0);
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
    if (present(lock)) {
        fail("a refused acquire left its lock behind");
        unlink(lock);
    }
    if (Atexit_calls < 2) {
        fail("the library did not attempt the second registration");
    }

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

    rmdir(dir);

    return Failed > 100 ? 100 : Failed;
}
