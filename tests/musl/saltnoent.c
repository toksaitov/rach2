/**
 * The salt policy's negative half: with no trusted entropy the mint refuses,
 * says so once, and writes nothing.
 *
 * link-deps: utils/pwauth.c
 *
 * Nothing in `make test` can reach this. That run always boots `-cpu max`, so
 * the CPU has RDSEED and RDRAND and the kernel's random pool is trusted; a
 * minter that quietly fell back to the untrusted stream would pass every other
 * proof in the tree. So this program is staged at `/musl/saltnoent` and is
 * deliberately not in `tools/runtests.py`'s `PROBES`, since it must fail where
 * entropy is trusted. `make test-noentropy` boots the same image on a CPU that
 * exposes neither instruction and runs it there.
 *
 * The precondition is asserted first, that `getrandom` really does refuse, so a
 * machine that did have the instructions reports a wrong premise rather than a
 * mystery. Then the refusal itself: `pwauth_mint_salt` returns -1 with every
 * sentinel byte of its buffer untouched, since a partly filled salt is a
 * predictable one, and its one line arrives on standard error exactly once,
 * read back through a pipe put there for the call. Then the writer's whole
 * sequence around the failed mint, which is what a `passwd` would do: take the
 * transaction, read `/etc/shadow`, mint, fail, end. The database must be byte
 * for byte what it was, with no temporary file and no lock left behind.
 *
 * Prints "SALTNOENT OK" on success and "SALTNOENT FAIL (detail)" otherwise.
 * Linked against the ported musl and the credential library.
 */

#define _GNU_SOURCE /* getrandom */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>

#include "pwauth.h"

#define SENTINEL '#'

/** Print the failure line and return 1, this program's failing status. */
static int fail(const char *what)
{
    printf("SALTNOENT FAIL (%s)\n", what);

    return 1;
}

/**
 * Read at most `size - 1` bytes of `path` into `buffer`, terminating it.
 * Returns the byte count, or -1 if the file could not be read.
 */
static ssize_t slurp(const char *path, char *buffer, size_t size)
{
    ssize_t n;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return -1;
    }
    n = read(fd, buffer, size - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    buffer[n] = '\0';

    return n;
}

/** True if `path` names something that exists. */
static int present(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/**
 * How many temporary files of `/etc/shadow`'s the directory still has.
 *
 * A replacement writes through `<path>.tmp.<pid>`, so the question is about a
 * prefix and not one name: a temporary this program's own run left behind would
 * carry its pid, and one an earlier boot left behind would carry another. A
 * directory that cannot be opened counts as one, since a question this could
 * not
 * answer must not read as a clean `/etc`.
 */
static int shadow_temps(void)
{
    struct dirent *entry;
    DIR *dir = opendir("/etc");
    int n = 0;

    if (!dir) {
        return 1;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "shadow.tmp", 10) == 0) {
            n++;
        }
    }
    closedir(dir);

    return n;
}

/** How many times `needle` occurs in `haystack`, without overlaps. */
static int occurrences(const char *haystack, const char *needle)
{
    const char *p = haystack;
    int n = 0;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }

    return n;
}

int main(void)
{
    char shadow_before[512];
    char shadow_after[512];
    char salt[PWAUTH_SALT_LEN + 1];
    char report[128];
    unsigned char raw[PWAUTH_SALT_LEN];
    ssize_t before;
    ssize_t after;
    ssize_t got;
    size_t i;
    int pipefd[2];
    int saved;
    int minted;

    if (getrandom(raw, sizeof(raw), 0) != -1 || errno != EIO) {
        return fail("this CPU has trusted entropy; the premise of the run is wrong");
    }

    memset(salt, SENTINEL, sizeof(salt));
    if (pipe(pipefd) != 0) {
        return fail("could not make the pipe that captures the diagnostic");
    }
    saved = dup(2);
    if (saved < 0 || dup2(pipefd[1], 2) != 2) {
        return fail("could not put a pipe on standard error");
    }
    close(pipefd[1]);
    minted = pwauth_mint_salt(salt, sizeof(salt));
    dup2(saved, 2);
    close(saved);
    got = read(pipefd[0], report, sizeof(report) - 1);
    close(pipefd[0]);
    report[got > 0 ? got : 0] = '\0';

    if (minted == 0) {
        return fail("the mint returned a salt with no trusted entropy behind it");
    }
    for (i = 0; i < sizeof(salt); i++) {
        if (salt[i] != SENTINEL) {
            return fail("the refused mint wrote part of a salt anyway");
        }
    }
    if (occurrences(report, "pwauth: no trusted entropy") != 1) {
        return fail("the refusal was not reported exactly once");
    }

    before = slurp("/etc/shadow", shadow_before, sizeof(shadow_before));
    if (before < 0) {
        return fail("could not read /etc/shadow");
    }
    if (cred_txn_begin("saltnoent") != 0) {
        return fail("could not take the transaction a writer holds");
    }
    if (slurp("/etc/shadow", shadow_after, sizeof(shadow_after)) != before) {
        cred_txn_end();

        return fail("could not read /etc/shadow under the transaction");
    }
    if (pwauth_mint_salt(salt, sizeof(salt)) == 0) {
        cred_txn_end();

        return fail("the mint inside a transaction returned a salt");
    }
    if (cred_txn_end() != 0) {
        return fail("could not end the transaction");
    }

    after = slurp("/etc/shadow", shadow_after, sizeof(shadow_after));
    if (after != before || memcmp(shadow_before, shadow_after, (size_t)before) != 0) {
        return fail("/etc/shadow changed under a writer that minted nothing");
    }
    if (shadow_temps() != 0) {
        return fail("a writer that minted nothing left a temporary file behind");
    }
    if (present("/etc/.pwlock")) {
        return fail("a writer that minted nothing left its lock behind");
    }

    printf("SALTNOENT OK\n");

    return 0;
}
