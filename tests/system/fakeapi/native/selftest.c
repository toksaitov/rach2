/* selftest.c: protocol state and wire checks without a network or client.
 * SSL reads and writes use byte buffers here. The server and each protocol
 * are the production C; the integration case supplies real TLS sockets. */
#include "api.h"
#include <stdlib.h>
#include <string.h>
struct wire {
    struct buffer out;
    const unsigned char *in;
    size_t left;
};
static int test_write(SSL *tls, const void *bytes, int n)
{
    struct wire *w = (struct wire *)tls;
    append(&w->out, bytes, (size_t)n);
    return n;
}
static int test_read(SSL *tls, void *bytes, int n)
{
    struct wire *w = (struct wire *)tls;
    if ((size_t)n > w->left)
        n = (int)w->left;
    if (!n)
        return 0;
    memcpy(bytes, w->in, (size_t)n);
    w->in += n;
    w->left -= (size_t)n;
    return n;
}
#define SSL_write test_write
#define SSL_read  test_read
#define main      fakeapi_program
#include "server.c"
#undef main
#undef SSL_write
#undef SSL_read
static int failures;
static void check(int held, const char *what)
{
    if (!held) {
        fprintf(stderr, "fakeapi-test: FAIL: %s\n", what);
        failures++;
    }
}
static void setup(struct server *s, const char *script)
{
    const struct value *v;
    size_t i = 0;
    memset(s, 0, sizeof(*s));
    service_mutex_init(&s->lock);
    service_condition_init(&s->condition);
    s->record = tmpfile();
    s->script = parse(script);
    s->nsteps = count(get(s->script, "steps"));
    s->steps = allocate(s->nsteps * sizeof(*s->steps));
    for (v = get(s->script, "steps")->child; v; v = v->next, i++) {
        s->steps[i].script = v;
        s->steps[i].protocol = *field(v, "protocol")           ? field(v, "protocol")
                               : *field(s->script, "protocol") ? field(s->script, "protocol")
                                                               : "plain";
    }
}
static void dispose(struct server *s)
{
    fclose(s->record);
    destroy(s->script);
    free(s->steps);
}
static struct request *get_request(const char *method, const char *path, const char *body,
                                   const char *headers_json)
{
    struct request *r = allocate(sizeof(*r));
    char *part;
    r->method = strdup(method);
    r->target = strdup(path);
    r->path = strdup(path);
    part = strchr(r->path, '?');
    if (part) {
        *part++ = 0;
        r->query = strdup(part);
    } else
        r->query = strdup("");
    r->version = strdup("HTTP/1.1");
    r->body = strdup(body);
    r->json = parse(body);
    r->headers = parse(headers_json);
    return r;
}
static struct peer test_peer(struct server *s, struct request *r, struct wire *w)
{
    struct peer p;
    memset(&p, 0, sizeof(p));
    p.server = s;
    p.request = r;
    p.keep = 1;
    p.tls = (SSL *)w;
    return p;
}
static void selection(void)
{
    struct server s;
    struct wire w = {0};
    struct request *r;
    struct peer p;
    setup(&s, "{\"steps\":[{\"unordered\":true,\"request\":{\"path\":\"/"
              "title\"},\"reply\":{\"status\":404}},"
              "{\"unordered\":true,\"repeat\":true,\"request\":{\"path\":\"/log\"},\"reply\":{}},"
              "{\"request\":{\"path\":\"/turn\"},\"reply\":{}}]}");
    r = get_request("GET", "/turn", "", "[]");
    p = test_peer(&s, r, &w);
    serve_request(&p);
    check(p.step == 3 && s.steps[2].played && !s.steps[0].taken,
          "ordered request before an unordered step");
    free_request(r);
    r = get_request("GET", "/title", "", "[]");
    p = test_peer(&s, r, &w);
    serve_request(&p);
    check(p.step == 1, "unordered step follows ordered step");
    free_request(r);
    r = get_request("GET", "/log", "", "[]");
    p = test_peer(&s, r, &w);
    serve_request(&p);
    p = test_peer(&s, r, &w);
    serve_request(&p);
    check(p.step == 2 && !s.steps[1].taken && !s.steps[1].played && verdict(&s) == 0,
          "repeat is reusable and not owed");
    free_request(r);
    r = get_request("GET", "/wrong", "", "[]");
    p = test_peer(&s, r, &w);
    serve_request(&p);
    check(s.failures == 1 && verdict(&s) == 1 && strstr(w.out.data, "400 Bad Request"),
          "unexpected request fails verdict");
    free_request(r);
    clear(&w.out);
    dispose(&s);
}
static void literal(void)
{
    struct server s;
    struct wire w = {0};
    struct request *r;
    struct peer p;
    struct value *step;
    setup(&s, "{\"steps\":[]}");
    step = parse(
        "{\"request\":{\"method\":\"POST\",\"path\":\"/"
        "echo\",\"headers\":{\"Content-Type\":\"application/json\"},\"json\":{\"a\":[true,2]}},"
        "\"reply\":{\"events\":[{\"event\":\"count\",\"id\":\"2\",\"data\":{\"n\":2}},{\"data\":"
        "\"two\\nlines\"}]}}");
    r = get_request("POST", "/echo", "{\"a\":[true,2],\"extra\":4.25}",
                    "[[\"content-type\",\"application/json\"]]");
    check(protocol_check("plain", step, r) == NULL,
          "JSON objects permit extra members and decimal values");
    p = test_peer(&s, r, &w);
    check(protocol_reply("plain", step, &p), "literal event response");
    check(strstr(w.out.data, "Transfer-Encoding: chunked\r\n") &&
              strstr(w.out.data, "event: count\nid: 2\ndata: {\"n\":2}\n\n") &&
              strstr(w.out.data, "data: two\ndata: lines\n\n") &&
              !strcmp(w.out.data + w.out.len - 5, "0\r\n\r\n"),
          "chunked SSE framing and multiline data");
    destroy(r->json);
    r->json = parse("{\"a\":[1,2]}");
    check(protocol_check("plain", step, r) != NULL, "booleans are distinct from integers");
    free_request(r);
    destroy(step);
    clear(&w.out);
    dispose(&s);
}
static void messages(void)
{
    const char *headers_json = "[[\"x-api-key\",\"rach2-offline-dummy-key\"],[\"anthropic-"
                               "version\",\"2023-06-01\"],[\"Content-Type\",\"application/json\"]]";
    struct value *step = parse("{\"results\":[{\"id\":\"t1\",\"equals\":\"expected\\n\"}],"
                               "\"reply\":[{\"type\":\"text\",\"text\":\"finished "
                               "text\"},{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"Bash\","
                               "\"input\":{\"command\":\"echo done\"}}]}");
    struct request *r = get_request(
        "POST", "/v1/messages?beta=true",
        "{\"model\":\"claude-sonnet-4-6\",\"stream\":true,\"max_tokens\":512,\"messages\":[{"
        "\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":"
        "\"Bash\",\"input\":{}}]},{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_"
        "use_id\":\"t1\",\"content\":[{\"type\":\"text\",\"text\":\"expected\\n\"}]}]}]}",
        headers_json);
    struct server s;
    struct wire w = {0};
    struct peer p;
    check(!protocol_validate("anthropic", step) && !protocol_check("anthropic", step, r),
          "Messages validates preceding call and text-block result");
    {
        struct value *body = r->json, *messages = (struct value *)get(body, "messages");
        struct value *result = (struct value *)get(messages->child->next, "content")->child;
        set(result, "is_error", boolean(1));
        check(protocol_check("anthropic", step, r) != NULL, "Messages rejects failed tool result");
        set(result, "is_error", boolean(0));
        set(result, "content", string("wrong"));
        check(protocol_check("anthropic", step, r) != NULL,
              "Messages rejects incorrect tool result");
    }
    setup(&s, "{\"steps\":[]}");
    p = test_peer(&s, r, &w);
    check(protocol_reply("anthropic", step, &p), "Messages streaming reply");
    check(strstr(w.out.data, "event: message_start\n") && strstr(w.out.data, "input_json_delta") &&
              strstr(w.out.data, "event: message_stop\n"),
          "Messages stream includes split tool JSON and completion");
    clear(&w.out);
    set(step, "stream", boolean(0));
    p = test_peer(&s, r, &w);
    check(protocol_reply("anthropic", step, &p) &&
              strstr(w.out.data, "\"stop_reason\": \"tool_use\""),
          "Messages whole response");
    clear(&w.out);
    free_request(r);
    destroy(step);
    dispose(&s);
}
static void gemini(void)
{
    struct value *step =
        parse("{\"tools\":[\"read\"],\"results\":[{\"id\":\"c1\",\"equals\":\"ok\"}],\"reply\":[{"
              "\"text\":\"finished "
              "text\"},{\"functionCall\":{\"id\":\"c2\",\"name\":\"read\",\"args\":{}}}]}");
    struct request *r = get_request(
        "POST", "/v1beta/models/gemini-3.1-pro-preview:streamGenerateContent?alt=sse",
        "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"hello\"}]},{\"role\":\"model\","
        "\"parts\":[{\"functionCall\":{\"id\":\"c1\",\"name\":\"read\",\"args\":{}},"
        "\"thoughtSignature\":\"cmFjaDItc2lnbmF0dXJlLWMx\"}]},{\"role\":\"user\",\"parts\":[{"
        "\"functionResponse\":{\"id\":\"c1\",\"name\":\"read\",\"response\":{\"output\":\"ok\"}}}]}"
        "],\"tools\":[{\"functionDeclarations\":[{\"name\":\"read\"}]}]}",
        "[[\"x-goog-api-key\",\"rach2-offline-dummy-key\"],[\"Content-Type\",\"application/"
        "json\"]]");
    struct server s;
    struct wire w = {0};
    struct peer p;
    check(!protocol_validate("gemini", step) && !protocol_check("gemini", step, r),
          "Gemini validates signature and function response");
    {
        struct value *contents = (struct value *)get(r->json, "contents");
        set((struct value *)get(contents->child->next, "parts")->child, "thoughtSignature",
            string("wrong"));
        check(protocol_check("gemini", step, r) != NULL,
              "Gemini rejects incorrect thought signature");
    }
    setup(&s, "{\"steps\":[]}");
    p = test_peer(&s, r, &w);
    check(protocol_reply("gemini", step, &p) && strstr(w.out.data, "thoughtSignature") &&
              strstr(w.out.data, "finishReason") && strstr(w.out.data, "usageMetadata"),
          "Gemini streamed content has signature, completion and usage");
    clear(&w.out);
    set(step, "stream", boolean(0));
    p = test_peer(&s, r, &w);
    check(protocol_reply("gemini", step, &p) &&
              strstr(w.out.data, "Content-Type: application/json"),
          "Gemini whole response");
    clear(&w.out);
    free_request(r);
    destroy(step);
    dispose(&s);
}
static void responses_test(void)
{
    struct value *step =
        parse("{\"turn\":{\"request\":{\"model\":\"gpt-test\"},\"input\":[{\"output_of\":\"c1\","
              "\"text\":[\"done\"]}],\"output\":[{\"call\":\"run\",\"namespace\":\"shell\",\"id\":"
              "\"c2\",\"arguments\":{\"cmd\":\"true\"}},{\"custom\":\"edit\",\"id\":\"c3\","
              "\"input\":\"patch\"},{\"message\":\"finished\"}]}}");
    struct request *r = get_request(
        "POST", "/v1/responses",
        "{\"model\":\"gpt-test\",\"stream\":true,\"input\":[{\"type\":\"function_call\",\"call_"
        "id\":\"c1\"},{\"type\":\"function_call_output\",\"call_id\":\"c1\",\"output\":\"done\"}],"
        "\"tools\":[{\"type\":\"namespace\",\"name\":\"shell\",\"tools\":[{\"type\":\"function\","
        "\"name\":\"run\"}]},{\"type\":\"custom\",\"name\":\"edit\"}]}",
        "[]");
    struct server s;
    struct wire w = {0};
    struct peer p;
    check(!protocol_validate("responses", step) && !protocol_check("responses", step, r),
          "Responses validates tool output and namespace tools");
    setup(&s, "{\"steps\":[]}");
    p = test_peer(&s, r, &w);
    check(protocol_reply("responses", step, &p) && strstr(w.out.data, "response.created") &&
              strstr(w.out.data, "response.function_call_arguments.delta") &&
              strstr(w.out.data, "response.custom_tool_call_input.delta") &&
              strstr(w.out.data, "response.output_text.delta") &&
              strstr(w.out.data, "response.completed"),
          "Responses stream has all output variants and completion");
    set(r->json, "previous_response_id", string("unknown"));
    check(protocol_check("responses", step, r) != NULL, "HTTPS cannot continue unstored response");
    set(r->json, "generate", boolean(0));
    check(protocol_check("responses", step, r) != NULL, "HTTPS rejects warmup");
    clear(&w.out);
    destroy(step);
    free_request(r);
    dispose(&s);
}
static void parser(void)
{
    const char *bad[] = {"{",  "[",  "{\"a\"",       "{\"a\":",           "[1,",        "01",
                         "1.", "1e", "[true false]", "{\"a\":1,\"a\":2}", "\"\\ud800\""};
    size_t i;
    struct value *v, *round;
    char *encoded;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        v = parse(bad[i]);
        check(v == NULL, "malformed JSON refused");
        destroy(v);
    }
    v = parse("{\"text\":\"\\u03b1\\ud83d\\ude00\",\"number\":-2.5e3}");
    encoded = encode(v, 0);
    round = parse(encoded);
    check(v && round && !subset(v, round), "UTF-8 and number round trip");
    free(encoded);
    destroy(v);
    destroy(round);
}
int main(void)
{
    parser();
    selection();
    literal();
    messages();
    gemini();
    responses_test();
    if (!failures)
        puts("fakeapi-test: PASS");
    return failures ? 1 : 0;
}
