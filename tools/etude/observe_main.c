/* Standalone native observer. Its parent owns its Job or process group. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include "win/files.h"
#else
#include <poll.h>
#endif
#include "etude/observe.h"
#include "observe_sequence.h"

static int stop_input(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD bytes = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &bytes, NULL))
        return 1;
    if (bytes)
        return 1;
    Sleep(100);
    return 0;
#else
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    return poll(&input, 1, 100) > 0 && input.revents != 0;
#endif
}

static int stop_request(const char *path, const char *run)
{
    char text[257];
    FILE *f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? 0 : -1;
    size_t n = fread(text, 1, sizeof text, f);
    int error = ferror(f);
    fclose(f);
    size_t size = strlen(run);
    if (!error && n < size + 1 && !memcmp(text, run, n))
        return 0;
    return !error && n == size + 1 && !memcmp(text, run, size) && text[size] == '\n' ? 1 : -1;
}

/* Query the persistent owner's creation identity before preparation starts. */
static int identity_main(int argc, char **argv)
{
    char *end;
    struct etude_pid identity;
    if (argc != 3) return 2;
    errno = 0;
    long pid = strtol(argv[2], &end, 10);
    if (errno || !argv[2][0] || *end || pid <= 0 ||
        etude_process_state(pid, &identity) != 1 || !identity.start_known) return 1;
    return printf("%llu\n", (unsigned long long)identity.start) < 0 ? 1 : 0;
}

int etude_observe_main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "identity"))
        return identity_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "sequence"))
        return etude_observe_sequence_main(argc, argv);
    const char *path = NULL, *run = NULL, *stop = NULL;
    struct etude_pid root = {0};
    char *end;
    int opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "o:p:c:r:s:")) != -1) {
        errno = 0;
        if (opt == 'o')
            path = optarg;
        else if (opt == 'r')
            run = optarg;
        else if (opt == 's')
            stop = optarg;
        else if (opt == 'p') {
            root.pid = strtol(optarg, &end, 10);
            if (errno || *end || root.pid <= 0)
                return 2;
        } else if (opt == 'c') {
            root.start = strtoull(optarg, &end, 10);
            if (errno || *end || !root.start)
                return 2;
            root.start_known = 1;
        } else
            return 2;
    }
    if (!path || !run || !root.pid || !root.start_known || optind != argc) {
        fputs("usage: etude observe -o TRACE -p OWNER_PID -c OWNER_CREATION -r RUN_ID [-s "
              "STOP_REQUEST]\n",
              stderr);
        return 2;
    }
    if (stop && (access(stop, F_OK) == 0 || errno != ENOENT))
        return 2;
    struct etude_observer *o = etude_observe_start(path, &root, run);
    if (!o) {
        perror("etude observe");
        return 2;
    }
    fputs("etude observe: ready\n", stdout);
    fflush(stdout);
    int requested = 0;
    while (!etude_observe_done(o)) {
        requested = stop ? stop_request(stop, run) : stop_input();
        if (requested)
            break;
        if (stop && stop_input()) {
            requested = -1;
            break;
        }
#ifdef _WIN32
        if (stop)
            Sleep(100);
#else
        if (stop)
            poll(NULL, 0, 100);
#endif
    }
    int rc = (requested < 0 ? etude_observe_cancel(o) : etude_observe_stop(o)) != 0;
    char *summary = etude_observe_summary(o, NULL);
    if (summary) {
        fputs(summary, stdout);
        free(summary);
    }
    etude_observe_free(o);
    return rc == 0 ? 0 : 1;
}
