/* Check SSH readiness retries and the distinct deliberate flood path. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include "compat/fixdir_host.c"

static int failures, attempts, refusals, waits, waited, closes, idle;
static int connect_fails, wait_fails, interrupted, receive_interrupted, flooding, flood_heard;

/** Record a failed expectation without stopping the remaining checks. */
static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "sshpeer-test: %s\n", message);
        failures++;
    }
}

/** Open a modeled descriptor only after the previous refusal's wait. */
static int model_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    check(flooding || waited == attempts, "another attempt preceded its wait");
    return 7;
}

/** Connect the descriptor, or report a real transport failure. */
static int model_connect(int fd, const struct sockaddr *address, socklen_t size)
{
    (void)fd;
    (void)address;
    (void)size;
    attempts++;
    if (connect_fails) {
        errno = ECONNREFUSED;
        return -1;
    }
    return 0;
}

/** Refuse the chosen attempts, then supply the required SSH identification. */
static ssize_t model_recv(int fd, void *buffer, size_t size, int flags)
{
    (void)fd;
    (void)flags;
    if (receive_interrupted) {
        receive_interrupted = 0;
        errno = EINTR;
        return -1;
    }
    if (flooding) {
        if (flood_heard)
            return 0;
        flood_heard = 1;
    } else if (attempts <= refusals)
        return 0;
    check(size >= 4, "the identification buffer is too short");
    memcpy(buffer, "SSH-", 4);
    return 4;
}

/** Count every descriptor release. */
static int model_close(int fd)
{
    check(fd == 7, "an unknown descriptor was closed");
    closes++;
    return 0;
}

/** Record waits without a wall clock, including an interrupted retry. */
static int model_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    if (timeout == 50) {
        check(!fds && count == 0 && !flooding, "a retry wait changed the flood path");
        waits++;
        if (interrupted) {
            interrupted = 0;
            errno = EINTR;
            return -1;
        }
        if (wait_fails) {
            errno = EIO;
            return -1;
        }
        waited++;
        return 0;
    }
    check(flooding && timeout == -1, "an unexpected wait was requested");
    if (fds) {
        check(count == 1, "the flood lost its connection");
        fds[0].revents = POLLHUP;
        return 1;
    }
    idle++;
    return 0;
}

#define socket  model_socket
#define connect model_connect
#define recv    model_recv
#define close   model_close
#define poll    model_poll
#define main    peer_main
#include "../cases/machine/sshpeer.c"
#undef main
#undef socket
#undef connect
#undef recv
#undef close
#undef poll

/** Start a row with no prior transport or timing state. */
static void reset(int rejected)
{
    attempts = waits = waited = closes = idle = 0;
    connect_fails = wait_fails = interrupted = receive_interrupted = flooding = flood_heard = 0;
    refusals = rejected;
}

/** Require the banner and one completed delay after every refusal. */
static void readiness(void)
{
    reset(0);
    check(wait_answer(22) == 0 && attempts == 1 && closes == 1 && waits == 0,
          "an immediate SSH identification must need no retry wait");
    reset(3);
    check(wait_answer(22) == 0 && attempts == 4 && closes == 4 && waited == 3,
          "each refused identification needs one completed wait");
    reset(1);
    interrupted = receive_interrupted = 1;
    check(wait_answer(22) == 0 && attempts == 2 && waits == 2 && waited == 1,
          "an interrupted receive or wait must not open an extra connection");
}

/** Keep failures fatal and the flood's ownership wait distinct. */
static void errors_and_flood(void)
{
    reset(0);
    connect_fails = 1;
    check(wait_answer(22) == 1 && attempts == 1 && closes == 1 && waits == 0,
          "a connection error must remain fatal without a retry");
    reset(1);
    wait_fails = 1;
    check(wait_answer(22) == 1 && attempts == 1 && closes == 1 && waits == 1,
          "a failed wait must stop before opening another connection");
    reset(0);
    flooding = 1;
    check(flood(22, 1) == 0 && attempts == 1 && closes == 1 && idle == 1 && waits == 0,
          "the flood must retain its indefinite ownership wait without retry pacing");
}

static int saved_stdout = -1;
static FILE *captured;

/** Restore the caller's stream before normal or interrupted fixture cleanup. */
static void restore_output(void)
{
    if (saved_stdout >= 0) {
        fflush(stdout);
        if (dup2(saved_stdout, STDOUT_FILENO) < 0)
            failures++;
        close(saved_stdout);
        saved_stdout = -1;
    }
    if (captured) {
        fclose(captured);
        captured = NULL;
    }
}

static void stop_fixture(int number)
{
    exit(128 + number);
}

int main(void)
{
    signal(SIGTERM, stop_fixture);
    signal(SIGINT, stop_fixture);
#ifndef _WIN32
    signal(SIGHUP, stop_fixture);
#endif
    const char *output = fix_path("peer-output");
    atexit(restore_output);
    captured = fopen(output, "w+");
    saved_stdout = dup(STDOUT_FILENO);
    if (!captured || saved_stdout < 0 || fflush(stdout) ||
        dup2(fileno(captured), STDOUT_FILENO) < 0) {
        restore_output();
        return 1;
    }
    char *args[] = {"sshpeer", "invalid", NULL};
    check(peer_main(2, args) == 2, "the real peer entry must retain usage refusal");
    readiness();
    errors_and_flood();
    check(!fflush(stdout), "the peer output must flush completely");
#ifdef _WIN32
    check(_setmode(_fileno(captured), _O_BINARY) >= 0, "capture must read the raw peer bytes");
#endif
    rewind(captured);
    char actual[1024];
    size_t size = fread(actual, 1, sizeof actual, captured);
    const char expected[] = "sshpeer: answered after 0 refused\n"
                            "sshpeer: answered after 3 refused\n"
                            "sshpeer: answered after 1 refused\n"
                            "sshpeer: open 1\nsshpeer: answered 1\nsshpeer: ended 1\n";
    check(!ferror(captured) && size == sizeof expected - 1 &&
              !memcmp(actual, expected, sizeof expected - 1),
          "readiness and flood records must retain exact LF bytes");
    restore_output();
    if (!failures) {
        fix_passed();
        puts("sshpeer-test: result: pass");
    }
    return failures ? 1 : 0;
}
