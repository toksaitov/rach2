/**
 * Check ISH waits, input lines, descriptors, environment names and the
 * search path.
 *
 * host-deps: ish/ish_shell_utilities.c ish/ish_cstring_utilities.c
 * host-cflags: -DISH_USE_STDLIB
 *
 * ishmusl shares the input, descriptor and environment cases. Native wrappers
 * inject wait errors and short or failed reads. ISH_RAW_HOSTTEST selects the
 * raw error branch while retaining host calls for process and file operations.
 */
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../../ish/ish_syscalls.h"

/* The scripted wait results, NULL for the host's waitpid: a pid, or -4 for
 * EINTR and any other negative for ECHILD; and the status of a reaped pid. */
static const int *wait_answers;
static int wait_answer_count;
static int wait_calls;
static int wait_status;
/* The most a read of standard input returns, 0 for no bound, and the reads
 * that succeed before one fails with EIO, -1 for none. */
static size_t read_limit;
static int read_failure_after = -1;
/* A path stat reports present without looking, and the times it did. */
static const char *stat_present;
static int stat_present_calls;

/** Supply scripted wait results, or wait for an actual test child. */
static int test_waitpid(int pid, int *status, int options)
{
    int result;

    if (wait_answers != NULL) {
        result = wait_calls < wait_answer_count ? wait_answers[wait_calls] : -10;
        ++wait_calls;
        if (result == pid) {
            *status = wait_status;
        }
        if (result >= 0) {
            return result;
        }
        errno = result == -4 ? EINTR : ECHILD;
    } else {
        result = waitpid(pid, status, options);
        if (result >= 0) {
            return result;
        }
    }
#ifdef ISH_RAW_HOSTTEST
    return errno == EINTR ? -4 : -10;
#else
    return -1;
#endif
}

/** Bound each shell read or fail after a fixed number of successful reads. */
static ssize_t test_read(int fd, void *buffer, size_t count)
{
    if (fd == 0) {
        if (read_failure_after == 0) {
            errno = EIO;
            return -1;
        }
        if (read_failure_after > 0) {
            --read_failure_after;
        }
        if (read_limit != 0 && count > read_limit) {
            count = read_limit;
        }
    }
    return read(fd, buffer, count);
}

/**
 * Stat through the host, or report `stat_present` as a file without looking.
 *
 * The host result goes to a buffer of its own in both builds, since the raw
 * build's scratch buffer is not the host's `struct stat`.
 */
static int test_stat(const char *path, void *unused)
{
    struct stat result;

    (void)unused;
    if (stat_present != NULL && strcmp(path, stat_present) == 0) {
        ++stat_present_calls;
        return 0;
    }
    return stat(path, &result);
}

#ifdef ISH_RAW_HOSTTEST
#undef ISH_USE_STDLIB
#endif
#define stat(path, result) test_stat(path, result)

#define waitpid test_waitpid
#define read    test_read
#define main    ish_main
#include "../../ish/ish.c"
#undef main
#undef read
#undef waitpid
#undef stat
#ifdef ISH_RAW_HOSTTEST
#define ISH_USE_STDLIB
#endif
#define ISH_HOSTTEST
#define main ish_probe_main
#include "../musl/ishmusl.c"
#undef main

/** Check a wait result and require every scripted call to be consumed. */
static int check_wait_case(const int *answers, int count, int status, int expected,
                           const char *name)
{
    int result;

    wait_answers = answers;
    wait_answer_count = count;
    wait_calls = 0;
    wait_status = status;
    result = ish_wait_for_child(5001);
    wait_answers = NULL;
    return result == expected && wait_calls == count ? 0 : fail(name);
}

/** Interrupted waits retry; other failures must not decode an absent status. */
static int check_waits(void)
{
    const int interrupted[] = {-4, -4, 5001};
    const int failed[] = {-10};
    const int empty[] = {0};
    const int other[] = {5002};
    const int reaped[] = {5001};
    int result = 0;

    result |= check_wait_case(interrupted, 3, 23 << 8, 23, "interrupted wait");
    result |= check_wait_case(failed, 1, 0, 126, "failed wait");
    result |= check_wait_case(empty, 1, 0, 126, "wait without a child status");
    result |= check_wait_case(other, 1, 0, 126, "wait returned another child");
    result |= check_wait_case(reaped, 1, 7 << 8, 7, "normal child status");
    result |= check_wait_case(reaped, 1, SIGTERM, 128 + SIGTERM, "signal child status");
    return result;
}

/** Force short reads and read errors independently of host file buffering. */
static int check_reads(void)
{
    int result;

    read_limit = 1;
    result = check_input_case("echo short\necho reads", "short\nreads\n", 0, "short input reads");
    read_limit = 0;
    read_failure_after = 0;
    result |= check_input_case("exit 7\n", "", 0, "initial input read error");
    read_failure_after = 1;
    result |= check_input_case("echo ignored", "", 0, "partial input read error");
    read_failure_after = -1;
    return result;
}

/** The joiner keeps a path that fits exactly and reports one cut short. */
static int check_join(void)
{
    char out[9];

    if (ish_combine_path_elements("/bin:/usr/bin", "cat", out, 8) != 0 ||
        strcmp(out, "/bin/cat") != 0) {
        return fail("path joined to its limit");
    }
    if (ish_combine_path_elements("/bin", "echo", out, 8) != -1 ||
        ish_combine_path_elements("/usr/bin", "ls", out, 8) != -1 ||
        ish_combine_path_elements("/usr/sbin", "ls", out, 8) != -1) {
        return fail("path joined past its limit");
    }
    return 0;
}

/**
 * A PATH entry too long to join with the command name must be skipped.
 *
 * The entry is 1019 bytes, so the joined path is cut at 1024 bytes to the
 * entry followed by `/prog`, a file this test reports present. The shell must
 * skip the entry and answer 127; running the cut path would run a program
 * other than the one named.
 */
static int check_long_path(void)
{
    char element[1100];
    char present[1100];
    char line[] = "prog-long-name";
    char *environment[] = {NULL};
    int status;

    memset(element, 'd', 1019);
    element[0] = '/';
    element[1019] = '\0';
    snprintf(present, sizeof(present), "%s/prog", element);
    stat_present = present;
    stat_present_calls = 0;
    fflush(NULL);
    status = ish_run_line(line, NULL, element, environment);
    stat_present = NULL;
    return status == 127 && stat_present_calls == 0 ? 0 : fail("long path entry");
}

/** Run the rows in a new directory under build/, which holds the probe's
 * output files and is removed after; 0 when every row passed. */
int main(void)
{
    char directory[] = "build/ish-hosttest.XXXXXX";
    int result;

    if (mkdtemp(directory) == NULL || chdir(directory) != 0) {
        perror("ish_hosttest: temporary directory");
        return 1;
    }
    result =
        check_descriptors() | check_environment() | check_input() | check_waits() | check_reads();
    result |= check_join() | check_long_path();
    unlink("ishmusl.output");
    unlink("ishmusl.capture");
    unlink("ishmusl.environment");
    if (chdir("../..") != 0 || rmdir(directory) != 0) {
        perror("ish_hosttest: cleanup");
        return 1;
    }
    if (result == 0) {
        puts("ish_hosttest: ok");
    }
    return result;
}
