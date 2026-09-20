/**
 * Check ISH waits, input lines, descriptors and environment names.
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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../../ish/ish_syscalls.h"

static const int *wait_answers;
static int wait_answer_count;
static int wait_calls;
static int wait_status;
static size_t read_limit;
static int read_failure_after = -1;

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

#ifdef ISH_RAW_HOSTTEST
/** Keep the host stat layout out of the freestanding shell's scratch buffer. */
static int test_stat(const char *path, void *unused)
{
    struct stat result;

    (void)unused;
    return stat(path, &result);
}
#undef ISH_USE_STDLIB
#define stat test_stat
#endif
#define waitpid test_waitpid
#define read    test_read
#define main    ish_main
#include "../../ish/ish.c"
#undef main
#undef read
#undef waitpid
#ifdef ISH_RAW_HOSTTEST
#undef stat
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

/** Keep probe output files inside a private directory in this worktree. */
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
