/* assets.c: render case scripts and verify the service's JSON audit. */
#include "api.h"
#include <stdlib.h>
#include <string.h>
#define BODY_MAX (64U * 1024U * 1024U)
static const struct value *first(const struct value *v)
{
    return v ? v->child : NULL;
}
static void replace(struct value *v, const char *from, const char *to)
{
    struct value *p;
    if (v->type == 's') {
        struct buffer out = {0};
        const char *at = v->text, *found;
        while ((found = strstr(at, from))) {
            append(&out, at, (size_t)(found - at));
            putsb(&out, to);
            at = found + strlen(from);
        }
        putsb(&out, at);
        free(v->text);
        v->text = out.data;
    }
    for (p = v->child; p; p = p->next)
        replace(p, from, to);
}
static struct value *sibling(const char *source, const char *name)
{
    struct buffer b = {0};
    const char *slash = strrchr(source, '/');
    struct value *v;
    if (slash)
        append(&b, source, (size_t)(slash - source + 1));
    putsb(&b, name);
    v = read_json(b.data);
    clear(&b);
    return v;
}
static void join_steps(struct value *script, const struct value *other, int before,
                       int explicit_protocol)
{
    struct value *joined = value('a');
    const struct value *p;
    unsigned pass;
    for (pass = 0; pass < 2; pass++) {
        const struct value *from = (pass == 0) == before ? other : script;
        for (p = first(get(from, "steps")); p; p = p->next) {
            struct value *copy = clone(p);
            if (explicit_protocol && from == other && !get(copy, "protocol"))
                add(copy, "protocol", clone(get(other, "protocol")));
            add(joined, NULL, copy);
        }
    }
    set(script, "steps", joined);
}
int render_script(const char *source, const char *destination, const char *row)
{
    struct value *script = read_json(source), *extra = NULL;
    struct value *steps, *last;
    char repo[300];
    char *encoded;
    FILE *f;
    int ok;
    if (!script)
        return 2;
    snprintf(repo, sizeof(repo), "/tmp/agents/work/%s/session/repo", row);
    replace(script, "@REPO@", repo);
    if (!strcmp(row, "claude-screen"))
        extra = sibling(source, "claude-screen.json");
    else if (!strcmp(row, "codex-screen"))
        extra = sibling(source, "codex-screen.json");
    else if (!strncmp(row, "agy-", 4))
        extra = sibling(source, "agy-services.json");
    if (extra) {
        join_steps(script, extra, strncmp(row, "codex-", 6) != 0, 1);
        destroy(extra);
    } else if (!strcmp(row, "claude-screen") || !strcmp(row, "codex-screen") ||
               !strncmp(row, "agy-", 4)) {
        destroy(script);
        return 2;
    }
    if (!strcmp(row, "muse-tmux")) {
        struct value *joined = value('a'), *config;
        const struct value *p = first(get(script, "steps"));
        if (!p) {
            destroy(script);
            return 2;
        }
        add(joined, NULL, clone(p));
        add(joined, NULL, clone(p));
        config =
            parse("{\"protocol\":\"plain\",\"unordered\":true,\"request\":{\"path\":\"/muse-code/"
                  "config\",\"query\":\"channel=public\"},\"reply\":{\"status\":404}}");
        add(joined, NULL, config);
        for (p = p->next; p; p = p->next)
            add(joined, NULL, clone(p));
        set(script, "steps", joined);
    }
    steps = (struct value *)get(script, "steps");
    last = steps ? steps->child : NULL;
    while (last && last->next)
        last = last->next;
    if (last &&
        (!strcmp(row, "codex-exec") || !strcmp(row, "claude-print") || !strcmp(row, "agy-print")))
        set(last, "hold", boolean(1));
    encoded = encode(script, 0);
    f = fopen(destination, "wb");
    ok = f && fputs(encoded, f) >= 0;
    if (f && fclose(f))
        ok = 0;
    free(encoded);
    destroy(script);
    return ok ? 0 : 2;
}
struct value *read_json_lines(const char *path)
{
    FILE *f = fopen(path, "rb");
    struct value *entries = value('a');
    struct buffer b = {0};
    int c;
    if (!f) {
        destroy(entries);
        return NULL;
    }
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            struct value *v = parse(b.data ? b.data : "");
            if (!v) {
                destroy(v);
                destroy(entries);
                entries = NULL;
                break;
            }
            add(entries, NULL, v);
            clear(&b);
        } else {
            if (b.len >= BODY_MAX) {
                destroy(entries);
                entries = NULL;
                break;
            }
            append(&b, &c, 1);
        }
    }
    if (ferror(f) || b.len) {
        destroy(entries);
        entries = NULL;
    }
    clear(&b);
    fclose(f);
    return entries;
}
static const struct value *nth(const struct value *v, unsigned n)
{
    const struct value *p = first(v);
    while (p && n--)
        p = p->next;
    return p;
}
static int curl_request(const struct value *e, unsigned request)
{
    const char *paths[] = {"/v1/hello",  "/v1/echo",    "/v1/events",
                           "/v1/socket", "/v1/nothing", "/v1/last"};
    struct request r;
    const char *agent;
    if (request >= 6 || number(e, "n", 0) != request + 1 ||
        strcmp(field(e, "path"), paths[request]) ||
        strcmp(field(e, "method"), request == 1 ? "POST" : "GET"))
        return 1;
    if (request == 4 ? !get(e, "unexpected")
                     : number(e, "step", 0) != (request == 5 ? 5 : request + 1))
        return 1;
    memset(&r, 0, sizeof(r));
    r.headers = (struct value *)get(e, "headers");
    agent = header(&r, "user-agent");
    if (request != 3 && strncmp(agent, "curl/", 5))
        return 1;
    if (request == 1 && strcmp(field(e, "body"), "{\"say\":\"ping\",\"from\":\"rach2\"}"))
        return 1;
    return 0;
}
static int curl_socket(const struct value *e, unsigned socket_entry)
{
    const char *kind[] = {"received", "sent", "received", "sent", "sent_close", "received_close"};
    const struct value *v;
    const char *key;
    if (socket_entry >= sizeof(kind) / sizeof(kind[0]))
        return 1;
    key = kind[socket_entry];
    if (number(e, "n", 0) != 4 || !(v = get(e, key)))
        return 1;
    if (socket_entry == 0 && strcmp(text(v), "ping"))
        return 1;
    if (socket_entry == 1 && strcmp(text(v), "pong"))
        return 1;
    if (socket_entry == 2 && strlen(text(v)) != 216)
        return 1;
    if (socket_entry == 3 && strcmp(text(v), "{\"n\": 3}"))
        return 1;
    if (socket_entry >= 4 && number(e, key, 0) != 1000)
        return 1;
    return 0;
}
static int verify_curl(const struct value *entries)
{
    const struct value *e;
    unsigned request = 0, socket_entry = 0;
    for (e = first(entries); e; e = e->next) {
        if (get(e, "method")) {
            if (curl_request(e, request++))
                return 1;
        } else if (curl_socket(e, socket_entry++))
            return 1;
    }
    return request != 6 || socket_entry != 6;
}

static int worker_prompt(const char *script_path, const struct value *steps,
                         const struct value *step, const struct value *body)
{
    const char *root_end = strstr(script_path, "/tests/system/");
    const struct value *before, *messages = get(body, "messages"), *content;
    struct buffer filename = {0}, prompt = {0}, actual = {0};
    struct value *expected;
    FILE *f;
    char block[4096], issue[32];
    size_t n;
    unsigned session = 0;
    if (!messages || !messages->child)
        return 0;
    for (before = steps->child; before; before = before->next) {
        if (!get(before, "results"))
            session++;
        if (before == step)
            break;
    }
    if (session < 1 || session > 2)
        return 0;
    if (root_end) {
        append(&filename, script_path, (size_t)(root_end - script_path));
        putsb(&filename, "/tools/flow/worker.txt");
    } else {
        const char *slash = strrchr(script_path, '/');
        if (!slash)
            return 0;
        append(&filename, script_path, (size_t)(slash - script_path));
        putsb(&filename, "/assets/tools/flow/worker.txt");
    }
    f = fopen(filename.data, "rb");
    clear(&filename);
    if (!f)
        return 0;
    while ((n = fread(block, 1, sizeof(block), f)))
        append(&prompt, block, n);
    fclose(f);
    while (prompt.len && strchr(" \t\r\n", prompt.data[prompt.len - 1]))
        prompt.data[--prompt.len] = 0;
    expected = string(prompt.data ? prompt.data : "");
    clear(&prompt);
    snprintf(issue, sizeof(issue), "%u", session == 1 ? 901U : 902U);
    replace(expected, "@ISSUE@", issue);
    content = get(messages->child, "content");
    if (content && content->type == 's')
        putsb(&actual, text(content));
    else
        for (before = first(content); before; before = before->next) {
            if (strcmp(field(before, "type"), "text"))
                continue;
            if (actual.len)
                putsb(&actual, "\n");
            putsb(&actual, field(before, "text"));
        }
    n = actual.data && strstr(actual.data, text(expected)) != NULL;
    destroy(expected);
    clear(&actual);
    return (int)n;
}
struct audit {
    struct value *script, *entries;
    const struct value *steps;
    const char *row, *script_path;
    unsigned *seen, requests, turns, socket_n, closes;
    char agent[200];
};

static int audit_metadata(struct audit *a, const char *version)
{
    const char *row = a->row;
    const struct value *e, *entries = a->entries;
    if (!strncmp(row, "codex", 5)) {
        for (e = first(entries); e; e = e->next)
            if (get(e, "method") && number(e, "step", 0) == 1)
                a->socket_n = (unsigned)number(e, "n", 0);
        if (!a->socket_n)
            return 0;
        snprintf(a->agent, sizeof(a->agent), "%s/%s ",
                 !strcmp(row, "codex-screen") ? "codex-tui" : "codex_exec", version);
    }
    if (!strncmp(row, "claude", 6) || !strcmp(row, "worker-session"))
        snprintf(a->agent, sizeof(a->agent), "claude-cli/%s ", version);
    if (!strncmp(row, "muse", 4)) {
        snprintf(a->agent, sizeof(a->agent), "muse-build/%s", version);
        {
            char *dash = strchr(a->agent + strlen("muse-build/"), '-');
            if (dash)
                *dash = 0;
        }
        strcat(a->agent, " ");
    }
    return 1;
}
static int audit_headers(struct audit *a, const struct request *r, const char *protocol)
{
    const char *row = a->row, *expected_key = NULL;
    if (!strcmp(protocol, "responses"))
        expected_key = "Bearer sk-fakeapi-dummy";
    if (!strncmp(row, "muse", 4))
        expected_key = "Bearer meta-fakeapi-dummy";
    if (expected_key && strcmp(header(r, "authorization"), expected_key))
        return 0;
    if (*a->agent && (strcmp(protocol, "plain") || !strncmp(row, "muse", 4)) &&
        strncmp(header(r, "user-agent"), a->agent, strlen(a->agent)))
        return 0;
    if (!strcmp(protocol, "gemini") && strcmp(header(r, "host"), "fakeapi.test"))
        return 0;
    return 1;
}
static int audit_request(struct audit *a, const struct value *e)
{
    const char *row = a->row, *script_path = a->script_path;
    const struct value *steps = a->steps, *script = a->script, *step;
    struct request r;
    const char *protocol, *why;
    unsigned n = (unsigned)number(e, "step", 0);
    if (!n || n > count(steps))
        return 0;
    step = nth(steps, n - 1);
    a->seen[n]++;
    a->requests++;
    memset(&r, 0, sizeof(r));
    r.method = (char *)field(e, "method");
    r.path = (char *)field(e, "path");
    r.query = (char *)field(e, "query");
    r.target = r.path;
    r.version = "HTTP/1.1";
    r.body = (char *)field(e, "body");
    r.headers = (struct value *)get(e, "headers");
    r.json = parse(r.body);
    protocol = *field(step, "protocol")     ? field(step, "protocol")
               : *field(script, "protocol") ? field(script, "protocol")
                                            : "plain";
    why = protocol_check(protocol, step, &r);
    if (!why && !strcmp(row, "worker-session") && !get(step, "results") &&
        !worker_prompt(script_path, steps, step, r.json))
        why = "the first request lacks its worker prompt";
    destroy(r.json);
    if (why) {
        log_line("audit: request %u: %s", n, why);
        return 0;
    }
    return audit_headers(a, &r, protocol);
}
static int audit_socket(struct audit *a, const struct value *e)
{
    const char *row = a->row;
    if (get(e, "received") && number(e, "n", 0) == a->socket_n && !strncmp(row, "codex", 5)) {
        struct value *body = parse(field(e, "received"));
        if (!body || strcmp(field(body, "type"), "response.create") ||
            flag(body, "generate", 1) != (a->turns != 0)) {
            destroy(body);
            return 0;
        }
        a->turns++;
        destroy(body);
    }
    if (!strncmp(row, "codex", 5) && number(e, "n", 0) == a->socket_n && get(e, "sent_close")) {
        if (number(e, "sent_close", 0) != 1000)
            return 0;
        a->closes++;
    }
    if (!get(e, "method")) {
        if (!strncmp(row, "muse", 4))
            return 0;
        return 1;
    }
    return 1;
}
static int audit_complete(struct audit *a)
{
    const struct value *step, *steps = a->steps;
    const char *row = a->row;
    for (step = steps->child; step; step = step->next) {
        unsigned n = 1;
        const struct value *before;
        for (before = steps->child; before != step; before = before->next)
            n++;
        if (!flag(step, "repeat", 0) && a->seen[n] != 1)
            return 0;
    }
    if (!strncmp(row, "codex", 5) && (a->turns != 3 || a->closes != 1))
        return 0;
    return !(a->requests == 0 && steps->child);
}
int verify_record(const char *script_path, const char *record_path, const char *row,
                  const char *version)
{
    struct audit a;
    const struct value *e;
    int result = 1;
    memset(&a, 0, sizeof(a));
    a.script = read_json(script_path);
    a.entries = read_json_lines(record_path);
    a.steps = get(a.script, "steps");
    a.seen = allocate((count(a.steps) + 1) * sizeof(*a.seen));
    a.row = row;
    a.script_path = script_path;
    if (!a.script || !a.entries || !a.steps)
        goto done;
    if (!strcmp(row, "curl")) {
        result = verify_curl(a.entries);
        goto done;
    }
    if (!audit_metadata(&a, version))
        goto done;
    for (e = first(a.entries); e; e = e->next) {
        if (e->type != 'o' || get(e, "failed") || get(e, "unexpected") || !audit_socket(&a, e))
            goto done;
        if (get(e, "method") && !audit_request(&a, e))
            goto done;
    }
    result = !audit_complete(&a);
done:
    log_line("audit: %s: %u requests", result ? "fail" : "pass", a.requests);
    free(a.seen);
    destroy(a.script);
    destroy(a.entries);
    return result;
}

int prepare_acceptance(const char *agent_source, const char *gh_source, const char *directory,
                       const char *agent)
{
    struct value *source = read_json(agent_source), *github = read_json(gh_source);
    struct value *hosts = value('a');
    const struct value *h, *v;
    struct buffer path = {0}, names_text = {0};
    const char *parts[] = {"session", "after", "gh"};
    char **names;
    unsigned i;
    int ok = 0;
    char repo[200];
    if (!source || !github)
        goto done;
    for (i = 0; i < 2; i++)
        for (h = first(get(i ? github : source, "hosts")); h; h = h->next) {
            int seen = 0;
            for (v = hosts->child; v; v = v->next)
                if (!strcmp(text(v), text(h)))
                    seen = 1;
            if (!seen)
                add(hosts, NULL, clone(h));
        }
    names = allocate((count(hosts) + 1) * sizeof(*names));
    i = 0;
    for (h = hosts->child; h; h = h->next) {
        names[i++] = (char *)text(h);
        putsb(&names_text, text(h));
        putsb(&names_text, "\n");
    }
    putsb(&path, directory);
    putsb(&path, "/ca");
    ok = make_ca(path.data, (int)i, names) == 0;
    free(names);
    clear(&path);
    if (!ok)
        goto done;
    putsb(&path, directory);
    putsb(&path, "/hosts");
    {
        FILE *f = fopen(path.data, "wb");
        ok = f && fputs(names_text.data ? names_text.data : "", f) >= 0;
        if (f && fclose(f))
            ok = 0;
    }
    clear(&path);
    if (!ok)
        goto done;
    snprintf(repo, sizeof(repo), "/home/test/rach2-accept/%s", agent);
    for (i = 0; i < 3; i++) {
        struct value *script = clone(get(i == 2 ? github : source, i == 2 ? "after" : parts[i]));
        char *encoded;
        FILE *f;
        if (script->type != 'o') {
            destroy(script);
            ok = 0;
            break;
        }
        replace(script, "@REPO@", repo);
        encoded = encode(script, 0);
        destroy(script);
        putsb(&path, directory);
        putsb(&path, "/");
        putsb(&path, parts[i]);
        putsb(&path, ".json");
        f = fopen(path.data, "wb");
        ok = f && fputs(encoded, f) >= 0;
        if (f && fclose(f))
            ok = 0;
        free(encoded);
        clear(&path);
        if (!ok)
            break;
    }
done:
    clear(&path);
    clear(&names_text);
    destroy(hosts);
    destroy(source);
    destroy(github);
    return ok ? 0 : 2;
}
