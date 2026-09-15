#define main runutil_program_main
#include "runutil.c"
#undef main

/** Keep a real child available while the production checker reads its ID. */
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

/** Exercise saved identity, zombie exclusion and a live machine survivor.
 * The reused case changes the saved start time while retaining a real PID.
 * Its worker must survive both checking and selection for outer cleanup.
 * A zombie remains unreaped until after the verdict, as a console relay can.
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
