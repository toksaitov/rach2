/*
 * runutil_machine: runutil.c's machine check run over a real process, for
 * runutil_owned.sh, which checks that the check stops a live machine and
 * passes over a zombie and a process whose PID was reused.
 */
#define main runutil_program_main
#include "runutil.c"
#undef main

/**
 * Fork a worker that ignores SIGTERM, and return its PID with its identity in
 * `*id`, as the worker read it. With `zombie` the worker exits at once and is
 * left unreaped; otherwise it waits for a signal, at most 30 seconds. A
 * failure exits 125.
 */
static pid_t start_worker(int zombie, struct identity *id)
{
    int ready[2];
    pid_t pid;
    if (pipe(ready) != 0)
        exit(125);
    pid = fork();
    if (pid < 0)
        exit(125);
    if (!pid) {
        close(ready[0]);
        signal(SIGTERM, SIG_IGN);
        alarm(30);
        if (process_identity(getpid(), id) != 1)
            _exit(125);
        write_all(ready[1], (const char *)id, sizeof(*id));
        close(ready[1]);
        if (zombie)
            _exit(0);
        for (;;)
            pause();
    }
    close(ready[1]);
    if (read(ready[0], id, sizeof(*id)) != sizeof(*id))
        exit(125);
    close(ready[0]);
    return pid;
}

/**
 * `runutil_machine CASE LEDGER` writes a worker's identity to LEDGER.identity
 * and runs `registered_machine` and `check_machines` over it.
 *
 * In `live` both must find the worker, and `check_machines` must kill it
 * with SIGKILL once SIGTERM has had its 15 seconds. In `reused` the saved
 * start time is one more than the worker's, as if its PID had been reused,
 * and in `zombie` the worker has exited and is not yet reaped, as a console
 * relay's child can be; in both neither function may find it, and the
 * worker must still be there afterwards. Prints `runutil-machine: result:
 * CASE passed` and returns 0, or returns 1; 125 is a failure of the fixture.
 */
int main(int argc, char **argv)
{
    struct identity id;
    pid_t pid;
    FILE *fp;
    char path[4096];
    int result, status, expected, zombie;
    if (argc != 3)
        return 125;
    zombie = !strcmp(argv[1], "zombie");
    expected = !strcmp(argv[1], "live");
    pid = start_worker(zombie, &id);
    if (zombie) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOWAIT) != 0)
            return 125;
    }
    if (!strcmp(argv[1], "reused"))
        id.start++;
    snprintf(path, sizeof(path), "%s.identity", argv[2]);
    fp = fopen(path, "w");
    if (!fp)
        return 125;
    fprintf(fp, "%ld %ld %llu %llu\n", (long)pid, (long)id.group, id.start, id.fraction);
    if (fclose(fp) != 0)
        return 125;
    result = registered_machine(argv[2], pid) != expected;
    if (check_machines(argv[1], argv[2]) != expected)
        result = 1;
    if (!expected && kill(pid, 0) != 0)
        result = 1;
    if (!expected)
        kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return 125;
    if (expected && (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL))
        result = 1;
    if (!result)
        printf("runutil-machine: result: %s passed\n", argv[1]);
    return result;
}
