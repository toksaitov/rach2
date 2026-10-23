/* flow_agents.c: interruption, broken pipes and credential argument privacy. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int secret_log(struct fixture *f)
{
    DIR *d = opendir(getenv("TMPDIR"));
    struct dirent *e;
    int found = 0;
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (!strncmp(e->d_name, "rach2-accept-secrets.", 21)) {
            char file[4096];
            snprintf(file, sizeof(file), "%s/%s/log", getenv("TMPDIR"), e->d_name);
            if (fx_exists(file)) {
                found = 1;
                break;
            }
        }
    }
    closedir(d);
    (void)f;
    return found;
}
static int until_log(struct fixture *f, struct fx_child *p)
{
    while (!secret_log(f)) {
        if (fx_poll(p))
            return 0;
        fx_pause();
    }
    return 1;
}
static int descendants(pid_t root, const char *token, int *leaked)
{
    struct item {
        pid_t pid, parent;
        int awk;
        char args[8192];
    } *items = NULL;
    size_t n = 0, cap = 0, i, j;
    DIR *d = opendir("/proc");
    struct dirent *e;
    int awk = 0;
    *leaked = 0;
    if (!d)
        return -1;
    while ((e = readdir(d))) {
        char path[100], *status, *at;
        FILE *f;
        size_t bytes;
        pid_t pid = (pid_t)strtol(e->d_name, NULL, 10);
        if (pid <= 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            items = realloc(items, cap * sizeof(*items));
            if (!items) {
                closedir(d);
                return -1;
            }
        }
        snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
        status = fx_read(path);
        if (!status)
            continue;
        at = strstr(status, "PPid:");
        if (!at) {
            free(status);
            continue;
        }
        items[n].pid = pid;
        items[n].parent = (pid_t)strtol(at + 5, NULL, 10);
        items[n].awk =
            strstr(status, "Name:\tawk\n") != NULL || strstr(status, "Name:\tgawk\n") != NULL;
        free(status);
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
        f = fopen(path, "rb");
        bytes = f ? fread(items[n].args, 1, sizeof(items[n].args) - 1, f) : 0;
        if (f)
            fclose(f);
        for (i = 0; i < bytes; i++)
            if (!items[n].args[i])
                items[n].args[i] = ' ';
        items[n].args[bytes] = 0;
        n++;
    }
    closedir(d);
    for (i = 0; i < n; i++) {
        pid_t up = items[i].parent;
        size_t depth = 0;
        while (up != root && up > 1 && depth++ <= n) {
            for (j = 0; j < n && items[j].pid != up; j++) {
            }
            if (j == n)
                break;
            up = items[j].parent;
        }
        if (up == root) {
            if (items[i].awk)
                awk = 1;
            if (strstr(items[i].args, token))
                *leaked = 1;
        }
    }
    free(items);
    return awk;
}
int flow_credential_arguments(struct fixture *f)
{
    const char *dummy = "rach2-dummy-antigravity-token-00000000";
    struct fx_child p;
    int fd, leaked = 0, awk, ok;
    const char *a[] = {"/bin/bash", flow_source(f, "tests/system/accept/agents_guest.sh"), "redact",
                       NULL};
    if (!flow_put(f, "home/rach2-accept/keys/agy", dummy) ||
        mkfifo(flow_path(f, "privacy-input"), 0600))
        return 0;
    if (!fx_start(f, &p, a, f->work, NULL, flow_path(f, "privacy-input")))
        return 0;
    fd = open(flow_path(f, "privacy-input"), O_WRONLY);
    if (fd < 0)
        return 0;
    while ((awk = descendants(p.pid, dummy, &leaked)) == 0) {
        if (fx_poll(&p)) {
            close(fd);
            return 0;
        }
        fx_pause();
    }
    ok = fx_check(f, awk == 1 && !leaked, "redact credential absent from arguments");
    {
        char line[200];
        int n = snprintf(line, sizeof(line), "a log with %s in it\n", dummy);
        if (write(fd, line, (size_t)n) != n)
            ok = 0;
    }
    close(fd);
    fx_wait(f, &p);
    ok = fx_check(f, !f->status && !strcmp(f->out, "a log with [credential] in it\n"),
                  "redact pipe output") &&
         ok;
    return ok;
}
int flow_agents_guest(struct fixture *f)
{
    static const char *names[] = {"secrets",      "filtered",  "work",
                                  "secrets-term", "work-term", "secrets-pipe"};
    char command[9000];
    unsigned i;
    int ok = 1;
    const char *setup[] = {"/bin/bash", flow_source(f, "tests/system/accept/agents_guest.sh"),
                           "setup", NULL};
    const char *prepare[] = {"/bin/bash", setup[1], "prepare", "claude", NULL};
    snprintf(command, sizeof(command), "exec '%s' --check\n", f->self);
    if (!flow_put(f, "home/rach2-accept/fixture/greeting.txt", "hello\n") ||
        !flow_put(f, "home/rach2-accept/fixture/check.sh", command))
        return 0;
    if (fx_run(f, setup, f->work, NULL, NULL) || fx_run(f, prepare, f->work, NULL, NULL))
        return 0;
    for (i = 0; i < 6; i++) {
        struct fx_child p;
        int in = -1, out = -1, held = 1;
        const char *a[] = {"/bin/bash", setup[1],
                           i == 1             ? "filtered"
                           : i == 2 || i == 4 ? "work"
                                              : "secrets",
                           i == 2 || i == 4 ? "claude" : "qemu", NULL};
        fx_remove(getenv("TMPDIR"));
        fx_mkdir(getenv("TMPDIR"));
        fx_remove(flow_path(f, "input"));
        fx_remove(flow_path(f, "output"));
        fx_remove(flow_path(f, "mark"));
        if (i == 0)
            flow_put(f, "input", "a log with rach2-offline-dummy-key in it\n");
        if (i == 3 || i == 5)
            if (mkfifo(flow_path(f, "input"), 0600))
                return 0;
        if (i == 5)
            if (mkfifo(flow_path(f, "output"), 0600))
                return 0;
        if (i == 4)
            fx_env("GUEST_TEST_MARK", flow_path(f, "mark"));
        if (!fx_start_io(f, &p, a, f->work, NULL,
                         i == 0 || i == 3 || i == 5 ? flow_path(f, "input") : NULL,
                         i == 5 ? flow_path(f, "output") : NULL))
            return 0;
        fx_env("GUEST_TEST_MARK", NULL);
        if (i == 3 || i == 5) {
            in = open(flow_path(f, "input"), O_WRONLY);
            if (in < 0)
                return 0;
        }
        if (i == 5) {
            out = open(flow_path(f, "output"), O_RDONLY);
            if (out < 0) {
                close(in);
                return 0;
            }
        }
        if (i == 3 || i == 5)
            held = until_log(f, &p);
        if (i == 4)
            held = fx_until(&p, flow_path(f, "mark"));
        if (i == 3 || i == 4)
            fx_stop(&p, SIGTERM);
        if (i == 5) {
            close(out);
            close(in);
            in = -1;
            while (!fx_poll(&p))
                fx_pause();
            f->status = p.status;
        } else
            fx_wait(f, &p);
        if (in >= 0)
            close(in);
        while (kill(-p.pid, 0) == 0)
            fx_pause();
        if (i == 0 || i == 1)
            held = held && !f->status &&
                   fx_regex(f->out, i ? "^0 of [0-9]+ strings$" : "^1 of [0-9]+ strings$");
        if (i == 2)
            held = held && !f->status && fx_regex(f->out, "^check checked$");
        if (i == 3 || i == 4)
            held = held && f->status == 143;
        if (i == 5)
            held = held && f->status == 141;
        ok = fx_check(f, held && fx_empty(getenv("TMPDIR")), names[i]) && ok;
    }
    return ok;
}
int flow_check_workload(int argc, char **argv)
{
    const char *mark = getenv("GUEST_TEST_MARK");
    (void)argc;
    (void)argv;
    if (mark && *mark) {
        if (!fx_text(mark, ""))
            return 1;
        for (;;)
            pause();
    }
    puts("checked");
    return 0;
}
