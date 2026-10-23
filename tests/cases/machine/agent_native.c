/* agent_native.c: prepare, run and judge guest rows without host scripts. */
#include "agent_native.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct row {
    struct net_case *u;
    struct net_boot *b;
    const char *work, *name;
    char assets[4096], script[4096], version[100], package[100], verifier[100];
    char hosts[32][256];
    unsigned nhosts, sequence;
    int fake;
};
static int path(char *out, size_t size, const char *format, ...)
{
    va_list ap;
    int n;
    va_start(ap, format);
    n = vsnprintf(out, size, format, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < size;
}
static int host(struct row *s, char *const argv[], const char *label)
{
    struct net_run r;
    int ok;
    if (net_host(s->u, argv, label, &r) != ETUDE_OK)
        return 0;
    fputs(r.out, stdout);
    fputs(r.err, stderr);
    ok = net_exited0(&r);
    net_run_free(&r);
    return ok;
}
static int command(struct row *s, const char *const argv[], const char *label, struct net_run *r)
{
    struct etude_guest_result end;
    enum etude_status st;
    st = etude_guest_command(s->b->m, argv, NULL, NULL, label, &end);
    memset(r, 0, sizeof(*r));
    if (st != ETUDE_OK) {
        net_fail(s->u, "%s: guest command did not complete", label);
        return 0;
    }
    r->end = end.termination;
    r->out = net_file(etude_evidence(s->u->c, label));
    {
        char error[200];
        snprintf(error, sizeof(error), "%s.err", label);
        r->err = net_file(etude_evidence(s->u->c, error));
    }
    return 1;
}
static int shell(struct row *s, const char *text, const char *label, struct net_run *r)
{
    const char *argv[] = {"/bin/bash", "-c", text, NULL};
    return command(s, argv, label, r);
}
static int run(struct row *s, const char *text, const char *label)
{
    struct net_run r;
    int ok;
    if (!shell(s, text, label, &r))
        return 0;
    fputs(r.out, stdout);
    fputs(r.err, stderr);
    ok = net_exited0(&r);
    net_run_free(&r);
    return ok;
}
static int put(struct row *s, const char *host_path, const char *guest_path, const char *mode)
{
    return etude_guest_put(s->b->m, host_path, guest_path, mode) == ETUDE_OK;
}
static char *fetch(struct row *s, const char *guest_path, const char *label)
{
    char evidence[200];
    snprintf(evidence, sizeof(evidence), "agent.%s.%u", label, ++s->sequence);
    if (etude_guest_get(s->b->m, guest_path, evidence) != ETUDE_OK)
        return NULL;
    return net_file(etude_evidence(s->u->c, evidence));
}
static char *trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
    return s;
}
static int write_text(const char *file, const char *text)
{
    FILE *f = fopen(file, "wb");
    int ok;
    if (!f)
        return 0;
    ok = fputs(text, f) >= 0;
    if (fclose(f))
        ok = 0;
    return ok;
}
static int add_host(struct row *s, const char *host_name)
{
    unsigned i;
    const char *p;
    if (!*host_name || strlen(host_name) >= sizeof(s->hosts[0]))
        return 0;
    for (p = host_name; *p; p++)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-", *p))
            return 0;
    for (i = 0; i < s->nhosts; i++)
        if (!strcmp(s->hosts[i], host_name))
            return 1;
    if (s->nhosts == 32)
        return 0;
    strcpy(s->hosts[s->nhosts++], host_name);
    return 1;
}
static int metadata(struct row *s)
{
    char filename[4096], *file, *line, *save = NULL;
    snprintf(s->package, sizeof(s->package), "%s", s->name);
    if (s->fake) {
        add_host(s, "fakeapi.test");
        add_host(s, "10.0.2.100");
        return path(s->script, sizeof(s->script), "%s/tests/system/fakeapi/fakeapi-test.json",
                    s->assets);
    }
    if (!path(filename, sizeof(filename), "%s/tests/system/agents/%s.sh", s->assets, s->name))
        return 0;
    file = net_file(filename);
    if (!*file) {
        free(file);
        return 0;
    }
    path(s->script, sizeof(s->script), "%s/tests/system/agents/%s.json", s->assets, s->name);
    for (line = strtok_r(file, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "# hosts: ", 9)) {
            char *token, *cursor = NULL;
            for (token = strtok_r(line + 9, " \t", &cursor); token;
                 token = strtok_r(NULL, " \t", &cursor))
                if (!add_host(s, token)) {
                    free(file);
                    return 0;
                }
        } else if (!strncmp(line, "# script: ", 10))
            path(s->script, sizeof(s->script), "%s/tests/system/%s", s->assets, line + 10);
        else if (!strncmp(line, "# package: ", 11))
            snprintf(s->package, sizeof(s->package), "%s", line + 11);
        else if (!strncmp(line, "# verifier: ", 12))
            snprintf(s->verifier, sizeof(s->verifier), "%s", line + 12);
    }
    free(file);
    if (!*s->verifier) {
        FILE *candidate;
        path(filename, sizeof(filename), "%s/tests/system/agents/%s_verify.py", s->assets, s->name);
        candidate = fopen(filename, "rb");
        if (candidate) {
            snprintf(s->verifier, sizeof(s->verifier), "%s_verify.py", s->name);
            fclose(candidate);
        }
    }
    if (!s->nhosts)
        add_host(s, "fakeapi.test");
    path(filename, sizeof(filename), "%s/vendor/gnu/agents/packages/%s.sh", s->assets, s->package);
    file = net_file(filename);
    save = NULL;
    for (line = strtok_r(file, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (!strncmp(line, "VERSION=", 8))
            snprintf(s->version, sizeof(s->version), "%s", line + 8);
    free(file);
    return 1;
}
static int prepare(struct row *s)
{
    const char *assets = etude_artifact(s->u->c, "agent-assets");
    char ca[4096], destination[4096];
    char *unpack[] = {"agent_server", "unpack", (char *)assets, s->assets, NULL};
    char *args[38];
    unsigned i;
    if (!assets || !host(s, unpack, "unpack-assets") || !metadata(s))
        return 0;
    path(ca, sizeof(ca), "%s/ca", s->work);
    args[0] = "agent_server";
    args[1] = "ca";
    args[2] = ca;
    for (i = 0; i < s->nhosts; i++)
        args[3 + i] = s->hosts[i];
    args[3 + i] = NULL;
    if (!host(s, args, "authority"))
        return 0;
    path(destination, sizeof(destination), "%s/script.json", s->work);
    {
        FILE *f = fopen(s->script, "rb");
        char *render[] = {"agent_server", "render", s->script, destination, (char *)s->name, NULL};
        if (!f)
            return write_text(destination, "{\"steps\":[]}\n");
        fclose(f);
        return host(s, render, "render-script");
    }
}
static int setup(struct row *s)
{
    char password[128], ca[4096], line[16384], key[4096];
    size_t used;
    unsigned i;
    struct net_run r;
    s->b->at = 0;
    if (!net_secret(s->u, "demo-password", password, sizeof(password)) ||
        net_login(s->b, "root", password, "# ") != ETUDE_OK ||
        etude_guest_open(s->b->m) != ETUDE_OK)
        return 0;
    if (!run(s, "mkdir -p /tmp/agents", "agent-directory"))
        return 0;
    if (!put(s, etude_artifact(s->u->c, "agent-assets"), "/tmp/agents/tree.tar", "0644"))
        return 0;
    if (!run(s,
             "mkdir -p /tmp/agents/tree && tar -xf /tmp/agents/tree.tar -C /tmp/agents/tree && "
             "cp /tmp/agents/tree/tests/system/agents_rows.sh /tmp/agents/ && "
             "cp /tmp/agents/tree/tests/system/agents/*.sh /tmp/agents/ && "
             "mkdir -p /tmp/agents/fixture && cp /tmp/agents/tree/tests/system/fakeapi/repo/* "
             "/tmp/agents/fixture/ && "
             "chown -R test:test /tmp/agents",
             "stage-rows"))
        return 0;
    if (!metadata(s))
        return 0;
    path(ca, sizeof(ca), "%s/ca/ca.pem", s->work);
    if (!put(s, ca, "/tmp/agents/ca.pem", "0644"))
        return 0;
    used = (size_t)snprintf(line, sizeof(line), "printf '");
    for (i = 0; i < s->nhosts && used < sizeof(line); i++)
        used += (size_t)snprintf(line + used, sizeof(line) - used, "10.0.2.100 %s\\n", s->hosts[i]);
    if (used >= sizeof(line) ||
        snprintf(line + used, sizeof(line) - used, "' >> /etc/hosts") >=
            (int)(sizeof(line) - used) ||
        !run(s, line, "api-hosts"))
        return 0;
    net_key(s->u, "test-key", key, sizeof(key));
    if (!put(s, key, "/tmp/agents/test.key", "0600"))
        return 0;
    if (!shell(s,
               "ssh -F /dev/null -i /tmp/agents/test.key -o StrictHostKeyChecking=no "
               "-o UserKnownHostsFile=/dev/null -o BatchMode=yes -o IdentitiesOnly=yes "
               "test@127.0.0.1 id -u",
               "test-login", &r))
        return 0;
    i = net_exited0(&r) && !strcmp(r.out, "1000\n");
    net_run_free(&r);
    if (!i)
        return 0;
    if (s->fake) {
        const char *client = etude_artifact(s->u->c, "wsclient");
        return client && put(s, client, "/tmp/agents/wsclient", "0755");
    }
    return 1;
}
static int row_run(struct row *s)
{
    char line[600], expected[200];
    struct net_run r;
    int ok;
    if (!metadata(s))
        return 0;
    snprintf(line, sizeof(line), "/bin/bash /tmp/agents/agents_rows.sh %s %s", s->name, s->version);
    {
        const char *args[] = {"/bin/su", "-s", "/bin/bash", "test", "-c", line, NULL};
        if (!command(s, args, "row", &r))
            return 0;
    }
    snprintf(expected, sizeof(expected), "AGENTROW OK %s", s->name);
    ok = net_exited0(&r) && net_find(r.out, expected);
    fputs(r.out, stdout);
    fputs(r.err, stderr);
    net_run_free(&r);
    return ok;
}
static int guest_expect(struct row *s, const char *const argv[], const char *expected,
                        const char *label)
{
    struct net_run r;
    int ok;
    if (!command(s, argv, label, &r))
        return 0;
    ok = net_exited0(&r) && !strcmp(r.out, expected);
    if (!ok)
        fprintf(stderr, "agents: %s: expected [%s], read [%s] [%s]\n", label, expected, r.out,
                r.err);
    net_run_free(&r);
    return ok;
}
static int git_expect(struct row *s, const char *arg, const char *extra, const char *expected,
                      const char *label)
{
    const char *argv[] = {"/usr/bin/git", "-C", "/tmp/agents/verify/repo", arg, extra, NULL};
    return guest_expect(s, argv, expected, label);
}
static int verify_repo(struct row *s)
{
    char expected[600];
    const char *name = !strncmp(s->name, "codex", 5) ? "Codex" : "Muse";
    const char *agent = !strncmp(s->name, "codex", 5) ? "codex" : "muse";
    int fixture = !strncmp(s->name, "claude", 6) || !strncmp(s->name, "agy", 3);
    const char *cat[] = {"/bin/cat",
                         fixture ? "/tmp/agents/verify/repo/fixture.txt"
                                 : "/tmp/agents/verify/repo/greeting.txt",
                         NULL};
    if (fixture) {
        snprintf(expected, sizeof(expected), "rach2 fixture %s\nanswer=after\n",
                 !strncmp(s->name, "claude", 6) ? "489" : "493");
        if (!guest_expect(s, cat, expected, "edited-fixture"))
            return 0;
        {
            const char *argv[] = {
                "/usr/bin/git", "-C", "/tmp/agents/verify/repo", "rev-list", "--count",
                "HEAD",         NULL};
            if (!guest_expect(s, argv, "2\n", "commit-count"))
                return 0;
        }
        {
            const char *argv[] = {"/usr/bin/git", "-C", "/tmp/agents/verify/repo", "log", "-1",
                                  "--format=%s",  NULL};
            if (!guest_expect(s, argv, "Update the offline fixture\n", "commit-title"))
                return 0;
        }
        if (!git_expect(s, "show", "HEAD:fixture.txt", expected, "committed-fixture"))
            return 0;
        {
            const char *argv[] = {"/usr/bin/git",
                                  "-C",
                                  "/tmp/agents/verify/repo",
                                  "diff-tree",
                                  "--no-commit-id",
                                  "--name-only",
                                  "-r",
                                  "HEAD",
                                  NULL};
            if (!guest_expect(s, argv, "fixture.txt\n", "committed-paths"))
                return 0;
        }
    } else {
        if (!guest_expect(s, cat, "hello, rach2\n", "edited-greeting"))
            return 0;
        snprintf(expected, sizeof(expected),
                 "Greet Rach2|%s Test <%s@fakeapi.test>\nThe fixture|%s Test <%s@fakeapi.test>\n",
                 name, agent, name, agent);
        if (!git_expect(s, "log", "--format=%s|%an <%ae>", expected, "commit-authors") ||
            !git_expect(s, "show", "HEAD:greeting.txt", "hello, rach2\n", "committed-greeting") ||
            !git_expect(s, "show", "HEAD~1:greeting.txt", "hello, world\n", "original-greeting"))
            return 0;
        {
            const char *argv[] = {
                "/usr/bin/git", "-C", "/tmp/agents/verify/repo", "ls-tree", "--name-only",
                "HEAD",         NULL};
            if (!guest_expect(s, argv, "check.sh\ngreeting.txt\nnotes.txt\n", "committed-tree"))
                return 0;
        }
        {
            const char *argv[] = {"/usr/bin/git", "-C",          "/tmp/agents/verify/repo",
                                  "diff",         "--name-only", "HEAD~1",
                                  "HEAD",         NULL};
            if (!guest_expect(s, argv, "greeting.txt\n", "committed-paths"))
                return 0;
        }
        if (!run(s,
                 "cmp /tmp/agents/verify/repo/check.sh /tmp/agents/fixture/check.sh && "
                 "cmp /tmp/agents/verify/repo/notes.txt /tmp/agents/fixture/notes.txt",
                 "unchanged-fixture-files"))
            return 0;
    }
    {
        const char *argv[] = {
            "/usr/bin/git",          "-C", "/tmp/agents/verify/repo", "status", "--porcelain",
            "--untracked-files=all", NULL};
        return guest_expect(s, argv, "", "clean-work-tree");
    }
}
static int worker_git(struct row *s, const char *arg, const char *extra, const char *expected,
                      const char *label)
{
    const char *argv[] = {"/usr/bin/git", "--git-dir=/tmp/agents/verify/origin.git", arg, extra,
                          NULL};
    return guest_expect(s, argv, expected, label);
}
static int closing(const char *message, unsigned wanted)
{
    const char *p = message;
    unsigned found = 0;
    while (*p) {
        char word[32];
        unsigned n = 0;
        while (*p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')))
            p++;
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            if (n + 1 < sizeof(word))
                word[n++] = (char)(*p | 32);
            p++;
        }
        word[n] = 0;
        if (!strcmp(word, "close") || !strcmp(word, "closes") || !strcmp(word, "closed") ||
            !strcmp(word, "fix") || !strcmp(word, "fixes") || !strcmp(word, "fixed") ||
            !strcmp(word, "resolve") || !strcmp(word, "resolves") || !strcmp(word, "resolved")) {
            const char *q = p;
            char *end;
            unsigned long issue;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
                q++;
            if (*q != '#')
                continue;
            issue = strtoul(q + 1, &end, 10);
            if (end == q + 1)
                continue;
            if (!wanted || issue != wanted || found++)
                return 0;
        }
    }
    return found == (wanted ? 1U : 0U);
}
static int verify_worker(struct row *s)
{
    char directory[4096];
    struct net_run r;
    char *history, *save = NULL, *hash;
    unsigned n = 0;
    char commits[3][80];
    const char *archive;
    if (etude_guest_get(s->b->m, "/tmp/agents/results/worker-session.tar", "worker-result.tar") !=
        ETUDE_OK)
        return 0;
    archive = etude_evidence(s->u->c, "worker-result.tar");
    path(directory, sizeof(directory), "%s/worker", s->work);
    {
        char *args[] = {"agent_server", "worker-proof", (char *)archive, directory, NULL};
        if (!host(s, args, "worker-proof"))
            return 0;
    }
    {
        const char *args[] = {"/usr/bin/git", "--git-dir=/tmp/agents/verify/origin.git",
                              "rev-list",     "--reverse",
                              "master",       NULL};
        if (!command(s, args, "worker-commits", &r))
            return 0;
    }
    if (!net_exited0(&r)) {
        net_run_free(&r);
        return 0;
    }
    history = strdup(r.out);
    net_run_free(&r);
    for (hash = strtok_r(history, "\n", &save); hash; hash = strtok_r(NULL, "\n", &save)) {
        if (n == 3 || strlen(hash) >= sizeof(commits[0])) {
            free(history);
            return 0;
        }
        strcpy(commits[n++], hash);
    }
    free(history);
    if (n != 3)
        return 0;
    for (n = 1; n < 3; n++) {
        char object[128], expected[100], filename[80];
        unsigned issue = n == 1 ? 901U : 902U;
        const char *args[] = {"/usr/bin/git",
                              "--git-dir=/tmp/agents/verify/origin.git",
                              "log",
                              "-1",
                              "--format=%B",
                              commits[n],
                              NULL};
        if (!command(s, args, "worker-commit-message", &r))
            return 0;
        if (!net_exited0(&r) || !closing(r.out, n == 1 ? 901U : 0U)) {
            net_run_free(&r);
            return 0;
        }
        net_run_free(&r);
        snprintf(filename, sizeof(filename), "worker-%u.txt\n", issue);
        {
            const char *files[] = {"/usr/bin/git", "--git-dir=/tmp/agents/verify/origin.git",
                                   "diff-tree",    "--no-commit-id",
                                   "--name-only",  "-r",
                                   commits[n],     NULL};
            if (!guest_expect(s, files, filename, "worker-committed-path"))
                return 0;
        }
        snprintf(object, sizeof(object), "%s:worker-%u.txt", commits[n], issue);
        snprintf(expected, sizeof(expected), "worker fixture %u\n", issue);
        if (!worker_git(s, "show", object, expected, "worker-committed-file"))
            return 0;
    }
    return 1;
}
static int verify(struct row *s)
{
    char script[4096], record_path[4096], line[1000];
    char *args[7];
    char *before = NULL, *after = NULL, *memory = NULL, *expected = NULL;
    const char *digest = NULL;
    int ok = 0;
    if (!metadata(s))
        return 0;
    path(script, sizeof(script), "%s/script.json", s->work);
    path(record_path, sizeof(record_path), s->fake ? "%s/record" : "%s/row.%s.record", s->work,
         s->name);
    args[0] = "agent_server";
    args[1] = "verify";
    args[2] = script;
    args[3] = record_path;
    args[4] = (char *)s->name;
    args[5] = s->version;
    args[6] = NULL;
    if (!host(s, args, "audit"))
        return 0;
    if (!*s->verifier || s->fake)
        return 1;
    snprintf(line, sizeof(line),
             "mkdir -p /tmp/agents/verify && tar -xf /tmp/agents/results/%s.tar -C "
             "/tmp/agents/verify && chown -R root:root /tmp/agents/verify",
             s->name);
    if (!run(s, line, "unpack-result"))
        return 0;
    if (!strcmp(s->name, "worker-session"))
        return verify_worker(s);
    {
        char *mode = fetch(s, "/tmp/agents/verify/mode", "mode");
        const char *wanted = strstr(s->name, "screen")       ? "screen"
                             : !strcmp(s->name, "muse-tmux") ? "tmux"
                             : strstr(s->name, "print")      ? "print"
                                                             : "exec";
        if (!mode || strcmp(trim(mode), wanted)) {
            free(mode);
            return 0;
        }
        free(mode);
    }
    if (!verify_repo(s))
        return 0;
    before = fetch(s, "/tmp/agents/verify/binary.before", "binary-before");
    after = fetch(s, "/tmp/agents/verify/binary.after", "binary-after");
    if (!strncmp(s->name, "claude", 6))
        digest = "24972e3bc859fab2b46ed4c1e51f7d6130f06d3bd550811a114640de3370d0de";
    else if (!strncmp(s->name, "agy", 3))
        digest = "60e72242f07c131f75afcd9a9e425d6ee097a1a1b3190ea98ae78fa4180bbfa8";
    else if (!strncmp(s->name, "muse", 4))
        digest = "decefc0fb33c48ee43419c3e72ac2dbc781743868611074c7292807f5960dfcf";
    else {
        const char *file = etude_artifact(s->u->c, "codex-digest");
        if (file)
            expected = net_file(file);
        digest = expected;
    }
    if (!digest || strlen(digest) < 64 || !before || !after || strncmp(before, digest, 64) ||
        strncmp(after, digest, 64))
        goto done;
    memory = fetch(s, "/tmp/agents/verify/memory", "free-frames");
    if (!memory || !*memory || strspn(memory, "0123456789\r\n") != strlen(memory))
        goto done;
    {
        char *seconds = fetch(s, "/tmp/agents/verify/seconds", "seconds"),
             *steps = fetch(s, "/tmp/agents/verify/steps", "steps");
        if (seconds)
            printf("agents: %s: session %s s\n", s->name, trim(seconds));
        if (steps)
            fputs(steps, stdout);
        free(seconds);
        free(steps);
    }
    ok = 1;
done:
    free(before);
    free(after);
    free(memory);
    free(expected);
    return ok;
}
static int as_test(struct row *s, const char *line, const char *label, struct net_run *r)
{
    const char *argv[] = {"/bin/su", "-s", "/bin/bash", "test", "-c", line, NULL};
    return command(s, argv, label, r);
}
static int curl_case(struct row *s)
{
    struct net_run r;
    char record_path[4096], *record_data, pad[201], line[1800];
    int ok;
    if (!as_test(s, "curl -sS -o /dev/null http://1.1.1.1/; printf 'rc=%s\\n' \"$?\"",
                 "outside-route", &r))
        return 0;
    ok = strstr(r.out, "rc=") && !strstr(r.out, "rc=0");
    net_run_free(&r);
    if (!ok)
        return 0;
    if (!as_test(s, "curl -sS https://fakeapi.test/v1/hello; printf 'rc=%s\\n' \"$?\"",
                 "system-ca-rejected", &r))
        return 0;
    ok = strstr(r.out, "rc=60") != NULL;
    net_run_free(&r);
    if (!ok)
        return 0;
    path(record_path, sizeof(record_path), "%s/record", s->work);
    record_data = net_file(record_path);
    ok = !*record_data;
    free(record_data);
    if (!ok)
        return 0;
    if (!as_test(s,
                 "curl -sS --cacert /tmp/agents/ca.pem -w ' %{ssl_verify_result} %{http_version}' "
                 "https://fakeapi.test/v1/hello",
                 "tls-get", &r))
        return 0;
    ok = net_exited0(&r) && !strcmp(r.out, "{\"hello\": \"rach2\"} 0 1.1");
    net_run_free(&r);
    if (!ok)
        return 0;
    if (!as_test(s,
                 "curl -sS --cacert /tmp/agents/ca.pem -w ' %{http_code}' -H 'Content-Type: "
                 "application/json' -d '{\"say\":\"ping\",\"from\":\"rach2\"}' "
                 "https://fakeapi.test/v1/echo",
                 "json-post", &r))
        return 0;
    ok = net_exited0(&r) && !strcmp(r.out, "pong\n 201");
    net_run_free(&r);
    if (!ok)
        return 0;
    if (!as_test(
            s, "curl -sS --cacert /tmp/agents/ca.pem -N 'https://fakeapi.test/v1/events?stream=1'",
            "event-stream", &r))
        return 0;
    ok = net_exited0(&r) && !strcmp(r.out, "event: count\ndata: {\"n\":1}\n\nevent: count\nid: "
                                           "2\ndata: {\"n\":2}\n\ndata: two\ndata: lines\n\n");
    net_run_free(&r);
    if (!ok)
        return 0;
    memset(pad, 'x', 200);
    pad[200] = 0;
    snprintf(line, sizeof(line),
             "/tmp/agents/wsclient fakeapi.test /v1/socket /tmp/agents/ca.pem ping "
             "'{\"n\":2,\"pad\":\"%s\"}'",
             pad);
    if (!as_test(s, line, "websocket", &r))
        return 0;
    ok = net_exited0(&r) && !strcmp(r.out, "pong keepalive\npong\n{\"n\": 3}\nclose 1000\n");
    net_run_free(&r);
    if (!ok)
        return 0;
    if (!as_test(s,
                 "curl -sS --cacert /tmp/agents/ca.pem -w ' %{http_code}' "
                 "https://fakeapi.test/v1/nothing",
                 "unexpected-request", &r))
        return 0;
    ok = net_exited0(&r) && strstr(r.out, "unexpected_request") &&
         strstr(r.out, "the path is /v1/nothing, not /v1/last") && strstr(r.out, " 400");
    net_run_free(&r);
    if (!ok)
        return 0;
    {
        const char *argv[] = {"/usr/bin/curl",
                              "-sS",
                              "--cacert",
                              "/tmp/agents/ca.pem",
                              "https://fakeapi.test/v1/last",
                              NULL};
        return guest_expect(s, argv, "last\n", "step-after-refusal");
    }
}
int agent_native_action(struct net_case *u, struct net_boot *b, const char *work, const char *name,
                        const char *verb, int fake)
{
    struct row s;
    memset(&s, 0, sizeof(s));
    s.u = u;
    s.b = b;
    s.work = work;
    s.name = name;
    s.fake = fake;
    if (!path(s.assets, sizeof(s.assets), "%s/assets", work))
        return 0;
    if (!strcmp(verb, "prepare"))
        return prepare(&s);
    if (!strcmp(verb, "setup"))
        return setup(&s);
    if (!strcmp(verb, "row"))
        return row_run(&s);
    if (!strcmp(verb, "client"))
        return curl_case(&s);
    if (!strcmp(verb, "verify"))
        return verify(&s);
    return 0;
}
