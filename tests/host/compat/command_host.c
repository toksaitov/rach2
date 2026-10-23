/* Execute prepared fixture tools through the same native process adapter. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

extern char **environ;

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

int host_command(const char *const argv[], const char *out_path, const char *err_path, char *out,
                 unsigned long capacity)
{
    struct etude_exec *ex = etude_exec_new();
    struct etude_proc *p = NULL;
    struct etude_exec_spec spec;
    const struct etude_exec_result *r;
    int status = -1;

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
    if (out != NULL)
        memcpy(out, r->out.data, r->out.kept + 1);
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
