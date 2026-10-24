/* agent_native.c: prepare, run and judge guest rows without host scripts. */
#include "agent_native.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct row {
    struct net_case *u;
    struct net_boot *b;
    const char *work, *name, *test_key;
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
    char password[128], ca[4096], line[16384];
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
    if (!put(s, s->test_key, "/tmp/agents/test.key", "0600"))
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
static int closing_word(const char *word)
{
    static const char *verbs[] = {"close", "closes",  "closed",   "fix",     "fixes",
                                  "fixed", "resolve", "resolves", "resolved"};
    unsigned i;
    for (i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++)
        if (!strcmp(word, verbs[i]))
            return 1;
    return 0;
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
        if (closing_word(word)) {
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
static int verify_runtime(struct row *s)
{
    char *before = NULL, *after = NULL, *memory = NULL, *expected = NULL;
    const char *digest = NULL;
    int ok = 0;
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
static int verify(struct row *s)
{
    char script[4096], record_path[4096], line[1000];
    char *args[7];
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
        const char *wanted = strstr(s->name, "print") ? "print" : "exec";
        if (!mode || strcmp(trim(mode), wanted)) {
            free(mode);
            return 0;
        }
        free(mode);
    }
    if (!verify_repo(s))
        return 0;
    return verify_runtime(s);
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
             "/etude-wsclient fakeapi.test /v1/socket /tmp/agents/ca.pem ping "
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
                        const char *verb, int fake, const char *test_key)
{
    struct row s;
    memset(&s, 0, sizeof(s));
    s.u = u;
    s.b = b;
    s.work = work;
    s.name = name;
    s.fake = fake;
    s.test_key = test_key;
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

/* The upstream CLI proofs retain their case identities while moving their
 * isolated process environment into a prepared GNU Rach2 machine. */
static int api_prepare(struct row *s)
{
    const char *assets = etude_artifact(s->u->c, "agent-assets");
    char ca[4096], destination[4096], *names[] = {"agent_server", "ca", ca, "fakeapi.test", NULL};
    char *unpack[] = {"agent_server", "unpack", (char *)assets, s->assets, NULL};
    char *render[] = {"agent_server", "render", s->script, destination, (char *)s->name, NULL};
    if (!assets || !host(s, unpack, "unpack-api-assets"))
        return 0;
    path(s->script, sizeof(s->script), "%s/tests/system/fakeapi/%s.json", s->assets,
         !strncmp(s->name, "codex", 5) ? "codex" : "muse");
    path(ca, sizeof(ca), "%s/ca", s->work);
    path(destination, sizeof(destination), "%s/script.json", s->work);
    return host(s, names, "api-authority") && host(s, render, "api-script");
}
static int api_file(struct row *s, const char *name, const char *guest, const char *contents,
                    const char *mode)
{
    char file[4096];
    path(file, sizeof(file), "%s/%s", s->work, name);
    return write_text(file, contents) && put(s, file, guest, mode);
}
static int api_setup(struct row *s, int refusal)
{
    char file[4096], config[5000];
    unsigned i;
    const char *files[] = {"check.sh", "greeting.txt", "notes.txt"};
    if (!run(s,
             "mkdir -p /tmp/agentapi/home/.codex /tmp/agentapi/home/.config/muse /tmp/agentapi/tmp "
             "/tmp/agents/verify/repo /tmp/agents/fixture && printf '10.0.2.100 fakeapi.test\n' >> "
             "/etc/hosts",
             "api-directories"))
        return 0;
    for (i = 0; i < 3; i++) {
        char guest[200];
        path(file, sizeof(file), "%s/tests/system/fakeapi/repo/%s", s->assets, files[i]);
        snprintf(guest, sizeof(guest), "/tmp/agents/fixture/%s", files[i]);
        if (!put(s, file, guest, i == 0 ? "0755" : "0644"))
            return 0;
    }
    path(file, sizeof(file), "%s/ca/ca.pem", s->work);
    if (!put(s, file, "/tmp/agentapi/ca.pem", "0644"))
        return 0;
    snprintf(config, sizeof(config), "[user]\n name = %s Test\n email = %s@fakeapi.test\n",
             !strncmp(s->name, "codex", 5) ? "Codex" : "Muse",
             !strncmp(s->name, "codex", 5) ? "codex" : "muse");
    if (!api_file(s, "gitconfig", "/tmp/agentapi/home/.gitconfig", config, "0644"))
        return 0;
    if (!strncmp(s->name, "codex", 5)) {
        if (!api_file(s, "codex.toml", "/tmp/agentapi/home/.codex/config.toml",
                      "model = \"gpt-5.5\"\nmodel_provider = \"fakeapi\"\napproval_policy = "
                      "\"never\"\nsandbox_mode = \"danger-full-access\"\n"
                      "check_for_update_on_startup = false\n[analytics]\nenabled = "
                      "false\n[model_providers.fakeapi]\n"
                      "name = \"fakeapi\"\nbase_url = \"https://fakeapi.test/v1\"\nenv_key = "
                      "\"FAKEAPI_KEY\"\n"
                      "wire_api = \"responses\"\nsupports_websockets = true\n",
                      "0644"))
            return 0;
    } else if (!api_file(
                   s, "muse-settings.json", "/tmp/agentapi/home/.config/muse/settings.json",
                   "{\"schema_version\":1,\"telemetry\":{\"enabled\":false},\"runtime_"
                   "capabilities\":{"
                   "\"plugin:tbh-reminders:reminder:goal-reminder\":{\"enabled\":false},"
                   "\"plugin:tbh-reminders:reminder:memory\":{\"enabled\":false},"
                   "\"plugin:tbh-reminders:reminder:scope-reminder\":{\"enabled\":false},"
                   "\"plugin:tbh-reminders:reminder:skill-reminder\":{\"enabled\":false},"
                   "\"plugin:tbh-reminders:reminder:todo-reminder\":{\"enabled\":false},"
                   "\"plugin:tbh-reminders:reminder:verify-reminder\":{\"enabled\":false}}}\n",
                   "0644"))
        return 0;
    if (!run(s,
             "cp /tmp/agents/fixture/* /tmp/agents/verify/repo/ && "
             "chown -R test:test /tmp/agentapi /tmp/agents",
             "api-fixture"))
        return 0;
    if (refusal && !api_file(s, "wrong-notes.txt", "/tmp/agents/verify/repo/notes.txt",
                             "Change the greeting in greeting.txt to: hello, there\n", "0644"))
        return 0;
    {
        struct net_run r;
        int ok = as_test(s,
                         "cd /tmp/agents/verify/repo && export HOME=/tmp/agentapi/home && "
                         "git init -q . && git add . && git commit -qm 'The fixture'",
                         "api-initial-commit", &r) &&
                 net_exited0(&r);
        net_run_free(&r);
        return ok;
    }
}
static int api_start(struct row *s)
{
    const char *prompt =
        "Read notes.txt and do what it says. Check the change with check.sh, then commit it.";
    char line[5000];
    if (!strncmp(s->name, "codex", 5))
        snprintf(
            line, sizeof(line),
            "cd /tmp/agents/verify/repo && exec env -i PATH=/usr/bin:/bin HOME=/tmp/agentapi/home "
            "CODEX_HOME=/tmp/agentapi/home/.codex TMPDIR=/tmp/agentapi/tmp LANG=C.UTF-8 TERM=dumb "
            "FAKEAPI_KEY=sk-fakeapi-dummy CODEX_CA_CERTIFICATE=/tmp/agentapi/ca.pem "
            "/opt/codex/bin/codex exec '%s'",
            prompt);
    else
        snprintf(
            line, sizeof(line),
            "cd /tmp/agents/verify/repo && exec env -i PATH=/usr/bin:/bin HOME=/tmp/agentapi/home "
            "TMPDIR=/tmp/agentapi/tmp LANG=C.UTF-8 TERM=dumb META_API_KEY=meta-fakeapi-dummy "
            "/opt/muse/bin/muse exec --yolo --model muse-spark-1.2 --max-model-steps 8 "
            "--base-url https://fakeapi.test/v1 -c "
            "'endpoint_transport.ca_bundle=\"/tmp/agentapi/ca.pem\"' '%s'",
            prompt);
    {
        const char *args[] = {"/bin/su", "-s", "/bin/bash", "test", "-c", line, NULL};
        return etude_guest_start(s->b->m, args, NULL, NULL) == ETUDE_OK;
    }
}
static int api_audit(struct row *s)
{
    char script[4096], record_path[4096];
    char *args[] = {"agent_server",  "verify",   script, record_path,
                    (char *)s->name, s->version, NULL};
    path(script, sizeof(script), "%s/script.json", s->work);
    path(record_path, sizeof(record_path), "%s/record", s->work);
    return host(s, args, "api-audit");
}
struct api_case {
    struct etude_case *c;
    struct net_case u;
    struct net_boot b;
    struct row s;
    struct etude_case_service *server;
    struct etude_termination end;
    struct etude_guest_result result;
    char work[4096], password[128], script[4096], record_path[4096], ca[4096], port[20];
    int codex, refusal, running, stopped_service, service_ok, prepared;
    int agent_ok, audit_ok, cleanup, channel_ready, muse_live;
    char *service_log, *status;
};

static void api_fixture(struct api_case *a)
{
    struct etude_case *c = a->c;
    net_step(&a->u, "prepare");
    if (mkdir(a->work, 0700) || !api_prepare(&a->s))
        net_fail(&a->u, "the API fixtures could not be prepared");
    a->prepared = net_ok(&a->u);
    net_step(&a->u, "server");
    path(a->script, sizeof(a->script), "%s/script.json", a->work);
    path(a->record_path, sizeof(a->record_path), "%s/record", a->work);
    path(a->ca, sizeof(a->ca), "%s/ca", a->work);
    snprintf(a->port, sizeof(a->port), "%u", etude_port(c, 0));
    {
        char *args[] = {"serve", a->script, a->record_path, a->ca, a->port, NULL};
        if (net_ok(&a->u) && etude_service(c, "fakeapi", args, &a->server, &a->end) != ETUDE_OK)
            net_fail(&a->u, "the API service did not listen");
    }
}
static void api_boot(struct api_case *a)
{
    net_step(&a->u, "boot");
    net_boot(&a->b, &a->u, "gnu", 1, NULL);
    net_wait(&a->b, "ext2: root: /dev/disk0p1");
    net_wait(&a->b, "init: sshd: started as pid");
    a->b.at = 0;
    if (net_ok(&a->u) && (!net_secret(&a->u, "demo-password", a->password, sizeof(a->password)) ||
                          net_login(&a->b, "root", a->password, "# ") != ETUDE_OK ||
                          etude_guest_open(a->b.m) != ETUDE_OK))
        net_fail(&a->u, "the guest command channel did not start");
    a->channel_ready = net_ok(&a->u);
}
static void api_isolation(struct api_case *a)
{
    struct net_run r;
    net_step(&a->u, "setup");
    if (net_ok(&a->u) && !api_setup(&a->s, a->refusal))
        net_fail(&a->u, "the fresh CLI home and repository could not be prepared");
    if (net_ok(&a->u) &&
        shell(&a->s, "curl -sS -o /dev/null http://1.1.1.1/; printf 'rc=%s\\n' \"$?\"",
              "api-no-route", &r)) {
        a->agent_ok = strstr(r.out, "rc=") && !strstr(r.out, "rc=0");
        net_run_free(&r);
    }
    net_assert(&a->u, "isolated", a->agent_ok, "the restricted guest reached an outside address");
    a->agent_ok = 0;
}
static void api_version(struct api_case *a)
{
    struct net_run r;
    net_step(&a->u, "version");
    {
        const char *args[] = {a->codex ? "/opt/codex/bin/codex" : "/opt/muse/bin/muse", "--version",
                              NULL};
        if (net_ok(&a->u) && command(&a->s, args, "api-version", &r)) {
            a->agent_ok =
                net_exited0(&r) && !strcmp(r.out, a->codex ? "codex-cli 0.162.1\n"
                                                           : "Muse Code 1.4.10 (1.4.10-R6427.1)\n");
            net_run_free(&r);
        }
    }
    net_assert(&a->u, "version", a->agent_ok, "the upstream program is not the pinned version");
    if (net_ok(&a->u) && !a->agent_ok)
        net_fail(&a->u, "the upstream version differs from the case");
}
static void api_execute(struct api_case *a)
{
    net_step(&a->u, "execute");
    if (net_ok(&a->u))
        a->running = api_start(&a->s);
    if (net_ok(&a->u) && !a->running)
        net_fail(&a->u, "the upstream CLI did not start");
    if (a->running) {
        uint64_t at;
        if (a->refusal && !a->codex &&
            etude_service_wait(a->server, ": unexpected:", 0, &at) != ETUDE_OK)
            net_fail(&a->u, "the API did not reject Muse's incorrect tool result");
        if (a->refusal && !a->codex)
            a->muse_live = etude_guest_running(a->b.m);
        if (etude_guest_finish(a->b.m, a->refusal && !a->codex, "upstream-cli", &a->result) !=
            ETUDE_OK)
            net_fail(&a->u, "the upstream CLI result was lost");
        a->running = 0;
        a->agent_ok = a->result.complete && a->result.cleanup == ETUDE_CLEANUP_CONFIRMED &&
                      a->result.termination.end == ETUDE_END_EXITED &&
                      a->result.termination.value == 0;
    }
}
static void api_verdict(struct api_case *a)
{
    struct etude_case *c = a->c;
    net_step(&a->u, "verdict");
    if (a->server) {
        a->stopped_service = etude_service_stop(a->server, &a->end) == ETUDE_OK;
        a->server = NULL;
    }
    a->service_log = net_file(etude_evidence(c, "fakeapi.1.out"));
    {
        char file[4096];
        path(file, sizeof(file), "%s/record.status", a->work);
        a->status = net_file(file);
    }
    a->service_ok =
        a->stopped_service && !strcmp(a->status, a->refusal ? "1\n" : "0\n") &&
        strstr(a->service_log,
               a->refusal ? a->codex ? "the first request 1: turn 2: the output of call_read"
                                     : "result: fail:"
               : a->codex ? "result: pass: 5 steps played by 5 requests"
                          : "result: pass: 6 steps played by 6 requests");
    if (a->refusal && !a->codex)
        a->service_ok =
            a->service_ok &&
            strstr(a->service_log,
                   "the first request 3: POST /v1/responses: step 3: the output of call_read");
    fputs(a->service_log, stdout);
}
static void api_verify(struct api_case *a)
{
    net_step(&a->u, "verify");
    if (a->refusal) {
        net_assert(&a->u, "refused", a->service_ok && a->muse_live,
                   "the API did not refuse the first incorrect tool result");
        {
            const char *cat[] = {"/bin/cat", "/tmp/agents/verify/repo/greeting.txt", NULL};
            a->audit_ok =
                net_ok(&a->u) && guest_expect(&a->s, cat, "hello, world\n", "unchanged-greeting") &&
                git_expect(&a->s, "log", "--format=%s", "The fixture\n", "unchanged-history");
        }
        net_assert(&a->u, "unchanged", a->audit_ok,
                   "the refusal changed the greeting or repository history");
    } else {
        net_assert(&a->u, "exit", a->agent_ok, "the upstream CLI did not exit with status zero");
        net_assert(&a->u, "verdict", a->service_ok,
                   "the API did not play the complete expected exchange");
        a->audit_ok = net_ok(&a->u) && api_audit(&a->s);
        net_assert(&a->u, "exchange", a->audit_ok,
                   "the API audit differs from the scripted exchange");
        net_assert(&a->u, "credentials", a->audit_ok,
                   "an API request lacks the dummy key or upstream user agent");
        {
            const char *cat[] = {"/bin/cat", "/tmp/agents/verify/repo/greeting.txt", NULL};
            net_assert(&a->u, "greeting",
                       net_ok(&a->u) &&
                           guest_expect(&a->s, cat, "hello, rach2\n", "final-greeting"),
                       "the greeting differs from the requested text");
        }
        net_assert(&a->u, "commit", net_ok(&a->u) && verify_repo(&a->s),
                   "the upstream commit or work tree differs from the fixture's expected result");
    }
}
static void api_cleanup(struct api_case *a)
{
    struct etude_case *c = a->c;
    net_step(&a->u, "cleanup");
    if (a->running)
        etude_guest_finish(a->b.m, 1, "upstream-cli-cancelled", &a->result);
    if (a->server)
        a->cleanup = etude_service_stop(a->server, &a->end) == ETUDE_OK;
    if (a->b.m) {
        if (a->channel_ready) {
            int removed = run(&a->s,
                              "rm -rf /tmp/agentapi /tmp/agents && test ! -e /tmp/agentapi "
                              "&& test ! -e /tmp/agents",
                              "remove-cli-home");
            a->cleanup = a->cleanup && removed;
        }
        {
            char *reply = NULL;
            etude_qmp(a->b.m, "quit", NULL, &reply);
            free(reply);
            int ended = etude_wait_end(a->b.m, &a->end) == ETUDE_OK;
            a->cleanup = a->cleanup && ended;
        }
    }
    if (a->prepared || *a->ca) {
        char *remove[] = {"agent_server", "remove",
                          net_ok(&a->u) && a->u.failures == 0 ? a->work : a->ca, NULL};
        int removed = etude_host(c, remove, "remove-api-keys", &a->end) == ETUDE_OK &&
                      a->end.end == ETUDE_END_EXITED && a->end.value == 0;
        a->cleanup = removed && a->cleanup;
    }
    etude_assert(c, "cleanup", a->cleanup,
                 "the private CLI processes, home or certificate keys outlived cleanup");
    if (!a->cleanup)
        a->u.failures++;
}
int agentapi_native(struct etude_case *c)
{
    struct api_case a;
    const char *id = etude_case_id(c);
    memset(&a, 0, sizeof(a));
    a.c = c;
    a.codex = strstr(id, ".codexapi.") != NULL;
    a.refusal = strstr(id, ".refusal") != NULL;
    a.cleanup = 1;
    a.muse_live = 1;
    net_case(&a.u, c);
    a.s.u = &a.u;
    a.s.b = &a.b;
    a.s.name = a.codex ? "codex-api" : "muse-api";
    snprintf(a.s.version, sizeof(a.s.version), "%s", a.codex ? "0.162.1" : "1.4.10-R6427.1");
    snprintf(a.work, sizeof(a.work), "%s", etude_evidence(c, "api-work"));
    a.s.work = a.work;
    path(a.s.assets, sizeof(a.s.assets), "%s/assets", a.work);
    api_fixture(&a);
    api_boot(&a);
    api_isolation(&a);
    api_version(&a);
    api_execute(&a);
    api_verdict(&a);
    api_verify(&a);
    api_cleanup(&a);
    free(a.service_log);
    free(a.status);
    return net_end(&a.u);
}
