/* Execute prepared fixture tools through the same native process adapter. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "etude/exec.h"
#include "command_host.h"

const char *host_tool_path(const char *name)
{
    static char paths[16][4096];
    static unsigned next;
    const char *root = getenv("HOST_TEST_TOOLS");
    char *out = paths[next++ % 16];
    int n;

    if (root == NULL || *root == '\0')
        root = "build/tests/host-suite/tools";
    n = snprintf(out, sizeof paths[0], "%s/%s", root, name);
    if (n < 0 || n >= (int)sizeof paths[0]) {
        fprintf(stderr, "host tool: path too long: %s\n", name);
        exit(2);
    }
    return out;
}

#ifndef _WIN32
extern char **environ;
#endif

/* The independent format checkers retain allocations at their process exit.
 * Preserve their existing leak setting while keeping ASan and UBSan fatal. */
static int command_environment(struct etude_exec_spec *spec)
{
    const char *name = strrchr(spec->argv[0], '/'), *back = strrchr(spec->argv[0], '\\');
    char **env;
    size_t n, count = 0;

    if (back != NULL && (name == NULL || back > name))
        name = back;
    name = name != NULL ? name + 1 : spec->argv[0];
    if (strcmp(name, "mkfatfixture") && strcmp(name, "mkfatfixture.exe") &&
        strcmp(name, "mkexfatfixture") && strcmp(name, "mkexfatfixture.exe"))
        return 0;
    for (n = 0; environ[n] != NULL; n++)
        ;
    env = calloc(n + 2, sizeof *env);
    if (env == NULL)
        return -1;
    for (n = 0; environ[n] != NULL; n++)
        if (strncmp(environ[n], "ASAN_OPTIONS=", 13) != 0)
            env[count++] = environ[n];
    env[count++] = "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1";
    spec->envp = env;
    return 0;
}

/** Run argv in this process's executor, as host_command describes, with the
 * length of the output copied to `out` in `*kept`. */
static int command_local(const char *const argv[], const char *out_path, const char *err_path,
                         char *out, unsigned long capacity, size_t *kept)
{
    struct etude_exec *ex = etude_exec_new();
    struct etude_proc *p = NULL;
    struct etude_exec_spec spec;
    const struct etude_exec_result *r;
    int status = -1;

    *kept = 0;
    if (out != NULL && capacity != 0)
        out[0] = '\0';
    if (ex == NULL)
        return -1;
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = "host-fixture-command";
    spec.argv = (char *const *)argv;
    spec.stdout_path = out_path;
    spec.stderr_path = err_path;
    spec.output_max = out != NULL && capacity ? (size_t)capacity - 1 : 4096;
    if (command_environment(&spec) != 0)
        goto done;
    if (etude_exec_start(ex, &spec, &p) != 0) {
        fprintf(stderr, "host tool: %s: %s\n", argv[0], strerror(errno));
        goto done;
    }
    while (etude_exec_wait(ex, -1) != p)
        ;
    r = etude_proc_result(p);
    if (r->cleanup != ETUDE_CLEANUP_CONFIRMED || r->output_error != 0 ||
        (out != NULL && (capacity == 0 || r->out.total != r->out.kept))) {
        fprintf(stderr, "host tool: %s: incomplete cleanup or output\n", argv[0]);
        goto done;
    }
    if (out != NULL) {
        memcpy(out, r->out.data, r->out.kept + 1);
        *kept = r->out.kept;
    }
    if (r->end == ETUDE_END_EXITED)
        status = r->value;
    else if (r->end == ETUDE_END_SIGNALED)
        status = 128 + r->value;
    if (status != 0 && err_path == NULL && r->err.kept)
        fprintf(stderr, "host tool: %s: %s", argv[0], r->err.data);
done:
    etude_exec_free(ex);
    free((void *)spec.envp);
    return status;
}

#ifndef _WIN32
/*
 * The command server. A fork copies the forking process's page tables, and a
 * test under the sanitizers grows to hundreds of megabytes, so each command
 * it ran itself cost two such copies, its keeper's and the command's, and
 * their teardown. The server is forked while the test is small and runs each
 * command in its own executor, in the test's environment and directory of
 * the moment, which the request carries.
 */
static int server_fd = -1; /* the test's end of the socket, or -1 */
static pid_t server_pid;

/** Write all `n` bytes of `p` to `fd`: 0, or -1. */
static int put_bytes(int fd, const void *p, size_t n)
{
    const char *at = p;

    while (n > 0) {
        ssize_t got = send(fd, at, n, MSG_NOSIGNAL);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        at += got;
        n -= (size_t)got;
    }
    return 0;
}

/** Read all `n` bytes into `p` from `fd`: 0, or -1 at an error or EOF. */
static int get_bytes(int fd, void *p, size_t n)
{
    char *at = p;

    while (n > 0) {
        ssize_t got = read(fd, at, n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        at += got;
        n -= (size_t)got;
    }
    return 0;
}

/** Send the string `s`, or a null pointer, as a length and its bytes. */
static int put_text(int fd, const char *s)
{
    uint32_t n = s != NULL ? (uint32_t)strlen(s) : UINT32_MAX;
    return put_bytes(fd, &n, sizeof n) || (s != NULL && put_bytes(fd, s, n)) ? -1 : 0;
}

/** Receive a string of put_text into `*s`, NULL for a null pointer; the
 * caller frees it. 0, or -1. */
static int get_text(int fd, char **s)
{
    uint32_t n;

    *s = NULL;
    if (get_bytes(fd, &n, sizeof n) != 0)
        return -1;
    if (n == UINT32_MAX)
        return 0;
    if ((*s = malloc((size_t)n + 1)) == NULL || get_bytes(fd, *s, n) != 0)
        return -1;
    (*s)[n] = '\0';
    return 0;
}

/** Send the null-terminated list `v` as a count and its strings. */
static int put_list(int fd, char *const *v)
{
    uint32_t n = 0, i;

    while (v[n] != NULL)
        n++;
    if (put_bytes(fd, &n, sizeof n) != 0)
        return -1;
    for (i = 0; i < n; i++)
        if (put_text(fd, v[i]) != 0)
            return -1;
    return 0;
}

/** Free the null-terminated list `v` of get_list. */
static void free_list(char **v)
{
    size_t i;

    for (i = 0; v != NULL && v[i] != NULL; i++)
        free(v[i]);
    free(v);
}

/** Receive a list of put_list into `*v`, which the caller frees with
 * free_list. 0, or -1. */
static int get_list(int fd, char ***v)
{
    uint32_t n, i;

    *v = NULL;
    if (get_bytes(fd, &n, sizeof n) != 0 || n > 65536 ||
        (*v = calloc((size_t)n + 1, sizeof **v)) == NULL)
        return -1;
    for (i = 0; i < n; i++)
        if (get_text(fd, &(*v)[i]) != 0 || (*v)[i] == NULL)
            return -1;
    return 0;
}

/** Serve requests on `fd` until the test closes its end, then exit. */
static void serve(int fd)
{
    for (;;) {
        char **argv = NULL, **env = NULL, *out_path = NULL, *err_path = NULL, *cwd = NULL;
        char *out = NULL, **prior = environ;
        uint64_t capacity;
        uint32_t has_out, sent;
        size_t kept = 0;
        int32_t status = -1;

        if (get_list(fd, &argv) != 0 || get_text(fd, &out_path) != 0 ||
            get_text(fd, &err_path) != 0 || get_bytes(fd, &has_out, sizeof has_out) != 0 ||
            get_bytes(fd, &capacity, sizeof capacity) != 0 || get_list(fd, &env) != 0 ||
            get_text(fd, &cwd) != 0 || argv[0] == NULL || cwd == NULL || capacity > UINT32_MAX)
            _exit(0);
        if (has_out && (out = malloc(capacity != 0 ? capacity : 1)) == NULL)
            _exit(1);
        if (chdir(cwd) == 0) {
            environ = env;
            status = command_local((const char *const *)argv, out_path, err_path, out,
                                   (unsigned long)capacity, &kept);
            environ = prior;
        } else {
            fprintf(stderr, "host tool: %s: %s\n", cwd, strerror(errno));
        }
        sent = (uint32_t)kept;
        if (put_bytes(fd, &status, sizeof status) != 0 || put_bytes(fd, &sent, sizeof sent) != 0 ||
            (sent != 0 && put_bytes(fd, out, sent) != 0))
            _exit(1);
        free(out);
        free(cwd);
        free(err_path);
        free(out_path);
        free_list(env);
        free_list(argv);
    }
}

/** Close the test's end, which ends the server, and reap it. */
static void server_stop(void)
{
    int status;

    if (server_fd < 0)
        return;
    close(server_fd);
    server_fd = -1;
    while (waitpid(server_pid, &status, 0) < 0 && errno == EINTR)
        ;
}

void host_command_server(void)
{
    int fds[2];
    pid_t pid;

    if (server_fd >= 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return;
    fflush(NULL);
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        close(fds[0]);
        serve(fds[1]);
    }
    close(fds[1]);
    server_fd = fds[0];
    server_pid = pid;
    atexit(server_stop);
}

/** Run argv in the server, as host_command describes. A server lost in
 * the request is stopped, and the command counts as not run to its end. */
static int command_served(const char *const argv[], const char *out_path, const char *err_path,
                          char *out, unsigned long capacity)
{
    char cwd[4096];
    uint64_t want = capacity;
    uint32_t has_out = out != NULL, kept;
    int32_t got;

    if (out != NULL && capacity != 0)
        out[0] = '\0';
    if (getcwd(cwd, sizeof cwd) == NULL) {
        fprintf(stderr, "host tool: %s: %s\n", argv[0], strerror(errno));
        return -1;
    }
    if (put_list(server_fd, (char *const *)argv) != 0 || put_text(server_fd, out_path) != 0 ||
        put_text(server_fd, err_path) != 0 || put_bytes(server_fd, &has_out, sizeof has_out) != 0 ||
        put_bytes(server_fd, &want, sizeof want) != 0 || put_list(server_fd, environ) != 0 ||
        put_text(server_fd, cwd) != 0 || get_bytes(server_fd, &got, sizeof got) != 0 ||
        get_bytes(server_fd, &kept, sizeof kept) != 0 || (kept != 0 && kept >= want) ||
        (kept != 0 && get_bytes(server_fd, out, kept) != 0)) {
        fprintf(stderr, "host tool: %s: the command server was lost\n", argv[0]);
        server_stop();
        return -1;
    }
    if (has_out && want != 0)
        out[kept] = '\0';
    return got;
}
#else
void host_command_server(void)
{}
#endif

int host_command(const char *const argv[], const char *out_path, const char *err_path, char *out,
                 unsigned long capacity)
{
    size_t kept;

#ifndef _WIN32
    if (server_fd >= 0)
        return command_served(argv, out_path, err_path, out, capacity);
#endif
    return command_local(argv, out_path, err_path, out, capacity, &kept);
}

#ifndef _WIN32
#define SPAWN_MAX 16
static pid_t spawned[SPAWN_MAX]; /* started and not yet reaped, or 0 */
static pid_t spawn_owner;        /* the process that started them */

/** Stop and reap each process still in `spawned`, at the test's exit. A
 * forked copy of the test that exits leaves them to the test. */
static void spawn_stop(void)
{
    int i, status;

    if (getpid() != spawn_owner)
        return;
    for (i = 0; i < SPAWN_MAX; i++) {
        if (spawned[i] == 0)
            continue;
        kill(spawned[i], SIGTERM);
        while (waitpid(spawned[i], &status, 0) < 0 && errno == EINTR)
            ;
        spawned[i] = 0;
    }
}

long host_command_spawn(const char *const argv[], const char *out_path)
{
    pid_t pid;
    int in, out, slot;

    for (slot = 0; slot < SPAWN_MAX && spawned[slot] != 0; slot++)
        ;
    if (slot == SPAWN_MAX) {
        fprintf(stderr, "host tool: %s: more than %d processes started\n", argv[0], SPAWN_MAX);
        return -1;
    }
    if (spawn_owner == 0) {
        spawn_owner = getpid();
        atexit(spawn_stop);
    }
    fflush(NULL);
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "host tool: %s: %s\n", argv[0], strerror(errno));
        return -1;
    }
    if (pid == 0) {
        in = open("/dev/null", O_RDONLY);
        out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (in < 0 || out < 0 || dup2(in, 0) < 0 || dup2(out, 1) < 0 || dup2(out, 2) < 0)
            _exit(127);
        close(in);
        close(out);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    spawned[slot] = pid;
    return (long)pid;
}

int host_command_reap(long pid, const char *out_path)
{
    char buf[8192];
    size_t n;
    int i, status;
    FILE *f;

    for (i = 0; i < SPAWN_MAX && spawned[i] != (pid_t)pid; i++)
        ;
    if (pid <= 0 || i == SPAWN_MAX) {
        fprintf(stderr, "host tool: %ld was not started by host_command_spawn\n", pid);
        return -1;
    }
    while (waitpid((pid_t)pid, &status, 0) < 0)
        if (errno != EINTR) {
            fprintf(stderr, "host tool: wait for %ld: %s\n", pid, strerror(errno));
            spawned[i] = 0;
            return -1;
        }
    spawned[i] = 0;
    f = fopen(out_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "host tool: %s: %s\n", out_path, strerror(errno));
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        fwrite(buf, 1, n, stdout);
    fclose(f);
    fflush(stdout);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}
#else
long host_command_spawn(const char *const argv[], const char *out_path)
{
    (void)argv;
    (void)out_path;
    return -1;
}

int host_command_reap(long pid, const char *out_path)
{
    (void)pid;
    (void)out_path;
    return -1;
}
#endif
