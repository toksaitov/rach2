/* Check clean private file pages, copy-on-write and truncation. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_report.h"

static char path[1024];
static volatile unsigned char *private_page, *shared_page;
static size_t length;
static int descriptor = -1;

/** Remove the file and every mapping and descriptor held by this process. */
static void cleanup(void)
{
    if (private_page && private_page != MAP_FAILED)
        munmap((void *)private_page, length);
    if (shared_page && shared_page != MAP_FAILED)
        munmap((void *)shared_page, length);
    if (descriptor >= 0)
        close(descriptor);
    if (*path)
        unlink(path);
    private_page = shared_page = NULL;
    descriptor = -1;
    *path = 0;
}

/** Reap `pid`, requiring its exact exit status or, with `sig`, signal. */
static int child_ended(pid_t pid, int sig)
{
    int status;
    pid_t got;

    if (pid < 0)
        return 0;
    do {
        got = waitpid(pid, &status, 0);
    } while (got < 0 && errno == EINTR);
    return got == pid && (sig ? WIFSIGNALED(status) && WTERMSIG(status) == sig
                              : WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/** Check a file on `dir`; return 0 or name the first failed contract. */
static const char *map_file(const char *dir)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    int reader;

    length = page * 3;
    snprintf(path, sizeof(path), "%s/privatefilemusl.XXXXXX", dir);
    descriptor = mkstemp(path);
    if (descriptor < 0 || ftruncate(descriptor, (off_t)length) != 0 ||
        pwrite(descriptor, "A", 1, 0) != 1)
        return "make file";
    reader = open(path, O_RDONLY);
    if (reader < 0)
        return "open read-only descriptor";
    private_page = mmap(NULL, length, PROT_READ, MAP_PRIVATE, reader, 0);
    close(reader);
    if (private_page == MAP_FAILED || private_page[0] != 'A')
        return "fault private page before shared mapping exists";
    shared_page = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (shared_page == MAP_FAILED)
        return "map shared page";
    shared_page[0] = 'B';
    if (private_page[0] != 'B')
        return "shared store reaches clean private page";
    if (pwrite(descriptor, "C", 1, 0) != 1 || private_page[0] != 'C' || shared_page[0] != 'C')
        return "descriptor write reaches both mappings";
    return NULL;
}

/** Write privately and fork, leaving the shared page and parent unchanged. */
static const char *private_writes(void)
{
    pid_t child;
    unsigned char byte;

    if (mprotect((void *)private_page, length, PROT_READ | PROT_WRITE) != 0)
        return "make read-only descriptor's private mapping writable";
    private_page[0] = 'D';
    shared_page[0] = 'E';
    if (private_page[0] != 'D' || pread(descriptor, &byte, 1, 0) != 1 || byte != 'E')
        return "private write leaves file and shared page unchanged";
    child = fork();
    if (child == 0) {
        private_page[0] = 'F';
        _exit(private_page[0] == 'F' && shared_page[0] == 'E' ? 0 : 1);
    }
    if (!child_ended(child, 0) || private_page[0] != 'D')
        return "fork isolates private writes";
    return NULL;
}

/** Truncate a private copy, discard another and keep an unlinked mapping. */
static const char *truncate_file(void)
{
    size_t page = length / 3;
    pid_t child;
    unsigned char byte;

    private_page[page * 2] = 'G';
    if (ftruncate(descriptor, (off_t)page) != 0)
        return "truncate file";
    child = fork();
    if (child == 0) {
        byte = private_page[page * 2];
        _exit(byte == 'G' ? 2 : 3);
    }
    if (!child_ended(child, SIGBUS))
        return "truncation removes private copies past EOF with SIGBUS";
    if (madvise((void *)private_page, length, MADV_DONTNEED) != 0 || private_page[0] != 'E')
        return "discard private copy and refault file page";
    if (unlink(path) != 0 || close(descriptor) != 0)
        return "unlink and close mapped file";
    *path = 0;
    descriptor = -1;
    shared_page[0] = 'H';
    if (private_page[0] != 'H')
        return "mappings keep unlinked file alive";
    cleanup();
    return NULL;
}

/** Check tmpfs and an optional disk directory, removing scratch on exit. */
int main(int argc, char **argv, char **envp)
{
    const char *why;
    struct rlimit core = {0, 0};
    int i;

    atexit(cleanup);
    setrlimit(RLIMIT_CORE, &core);
    report_begin(report_case(envp, "guest.default.privatefilemusl"));
    report_plan("private-pages");
    for (i = 0; i < (argc > 1 ? 2 : 1); i++) {
        const char *dir = i ? argv[1] : "/dev/shm";

        why = map_file(dir);
        if (!why)
            why = private_writes();
        if (!why)
            why = truncate_file();
        if (why) {
            fprintf(stderr, "privatefilemusl: %s: %s: errno %d\n", dir, why, errno);
            report_stop(why);
            return 1;
        }
        printf("privatefilemusl: passed: %s\n", dir);
    }
    report_held();
    report_end();
    puts("PRIVATEFILEMUSL OK");
    return 0;
}
