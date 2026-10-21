/**
 * keygennoent: check that with no trusted entropy Dropbear's key generator
 * refuses and writes no key.
 *
 * `dropbearmulti dropbearkey` reads the random device for a key's bytes, and
 * on a boot whose generator is untrusted the device answers EIO. The key
 * generator must then exit with its error that it could not read the random
 * device, and leave no key file, since a key made without those bytes would
 * be a predictable one. That is why rach2/run.sh and its siblings boot
 * `-cpu max`. The test image stages the program os-real stages, at the same
 * path.
 *
 * The premise is checked first, as saltnoent checks it: `getrandom` fails
 * with EIO, so a machine that has trusted entropy reports a wrong premise
 * and not a fault of Dropbear. The key generator's standard output and error
 * are read through one pipe. The key file is `keygennoent.key` in the working
 * directory, the case's workspace under etude, and is removed on every path.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, guest.noentropy.keygennoent without it, and beside them prints
 * "KEYGENNOENT OK" with status 0, or "KEYGENNOENT FAIL (detail)" with status
 * 1. A failed check ends the program: its assertion fails and those after it
 * are not run. Linked against the ported musl.
 */

#define _GNU_SOURCE /* getrandom */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "../probe_report.h"

#define DROPBEAR "/bin/dropbearmulti"
#define KEY      "keygennoent.key"

extern char **environ;

/** Report the path's next assertion failed, print the failure line, remove
 * the key and return 1, this program's failing status. */
static int fail(const char *what)
{
    report_stop(what);
    printf("KEYGENNOENT FAIL (%s)\n", what);
    unlink(KEY);

    return 1;
}

/**
 * Run the key generator for an ed25519 key in KEY, with its standard output
 * and error into `out`, at most `size - 1` bytes, terminated. Returns its
 * wait status, or -1 when it could not be started or waited for.
 */
static int run_keygen(char *out, size_t size)
{
    char *argv[] = {"dropbearmulti", "dropbearkey", "-t", "ed25519", "-f", KEY, NULL};
    size_t got = 0;
    ssize_t n;
    pid_t pid;
    int fds[2];
    int status;

    if (pipe(fds) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        execve(DROPBEAR, argv, environ);
        _exit(127);
    }
    close(fds[1]);
    while ((n = read(fds[0], out + got, size - 1 - got)) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0) {
            got += (size_t)n;
        }
        if (got == size - 1) {
            break;
        }
    }
    close(fds[0]);
    out[got] = '\0';
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return status;
}

int main(void)
{
    unsigned char raw[16];
    char output[1024];
    struct stat st;
    int status;

    report_begin(report_case(environ, "guest.noentropy.keygennoent"));
    report_plan("premise");
    report_plan("refused");
    report_plan("reason");
    report_plan("no-key");
    unlink(KEY);
    if (getrandom(raw, sizeof(raw), 0) != -1 || errno != EIO) {
        return fail("this CPU has trusted entropy; the premise of the run is wrong");
    }
    report_held();

    status = run_keygen(output, sizeof(output));
    fputs(output, stdout);
    if (status == -1) {
        return fail("could not run " DROPBEAR);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 127) {
        return fail(WIFEXITED(status) && WEXITSTATUS(status) == 0
                        ? "dropbearkey made a key with no trusted entropy"
                        : "dropbearkey did not exit with its error status");
    }
    report_held();
    if (strstr(output, "reading random device") == NULL) {
        return fail("dropbearkey failed without saying it could not read the random device");
    }
    report_held();
    if (stat(KEY, &st) == 0) {
        return fail("dropbearkey left a key file behind");
    }
    report_held();
    report_end();

    printf("KEYGENNOENT OK\n");

    return 0;
}
