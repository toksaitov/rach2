/* selftest.c: protocol state and wire checks without a network or client.
 * SSL reads and writes use byte buffers here. The server and each protocol
 * are the production C; the integration case supplies real TLS sockets. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "api.h"
#include <openssl/rand.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "../../../host/compat/fixdir_host.c"
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
static int test_tls_error(const SSL *tls, int result)
{
    (void)tls;
    return result == 0 ? SSL_ERROR_ZERO_RETURN : SSL_ERROR_SSL;
}
#ifndef _WIN32
static int interrupt_verdict, repeated_stop;
static struct buffer stopped_verdict;

/** Deliver a separate TERM after the first handler, during verdict output. */
static int test_fputs(const char *text, FILE *file)
{
    if (interrupt_verdict && !strncmp(text, "fakeapi: result:", 16)) {
        interrupt_verdict = 0;
        repeated_stop = pthread_kill(pthread_self(), SIGTERM) == 0;
        putsb(&stopped_verdict, text);
    }
    return fputs(text, file);
}
#define fputs test_fputs
#endif
#define SSL_write     test_write
#define SSL_read      test_read
#define SSL_get_error test_tls_error
#define main          fakeapi_program
#include "server.c"
#undef main
#undef SSL_write
#undef SSL_read
#undef SSL_get_error
#ifndef _WIN32
#undef fputs
#endif
static const unsigned char *serial_input;
static int test_serial_random(unsigned char *data, int n)
{
    if (!serial_input)
        return RAND_bytes(data, n);
    if (n != 8)
        return 0;
    memcpy(data, serial_input, 8);
    return 1;
}
#define RAND_bytes test_serial_random
#include "tls.c"
#undef RAND_bytes
static int failures;
static void check(int held, const char *what)
{
    if (!held) {
        fprintf(stderr, "fakeapi-test: FAIL: %s\n", what);
        failures++;
    }
}
static void serial_test(void)
{
    static const unsigned char inputs[][8] = {{0, 1, 2, 3, 4, 5, 6, 7},
                                              {0, 128, 2, 3, 4, 5, 6, 7},
                                              {0, 0, 1, 2, 3, 4, 5, 6},
                                              {0, 0, 0, 0, 0, 0, 0, 0},
                                              {127, 255, 255, 255, 255, 255, 255, 255},
                                              {255, 255, 255, 255, 255, 255, 255, 255}};
    EVP_PKEY *pkey = key();
    size_t i;

    check(pkey != NULL, "certificate serial test key");
    if (!pkey)
        return;
    for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        X509 *cert, *decoded = NULL;
        unsigned char *der = NULL;
        const unsigned char *p;
        uint64_t expected = 0, actual = 0;
        size_t j;
        int length = 0;

        serial_input = inputs[i];
        cert = certificate(pkey, NULL, "serial test");
        if (cert && X509_sign(cert, pkey, EVP_sha256()))
            length = i2d_X509(cert, &der);
        p = der;
        if (length > 0)
            decoded = d2i_X509(NULL, &p, length);
        for (j = 0; j < sizeof(inputs[i]); j++)
            expected = (expected << 8) | (j ? inputs[i][j] : inputs[i][j] & 127);
        if (!expected)
            expected = 1;
        check(decoded && p == der + length &&
                  ASN1_INTEGER_get_uint64(&actual, X509_get_serialNumber(decoded)) &&
                  actual == expected,
              "certificate serial has canonical positive encoding");
        X509_free(decoded);
        OPENSSL_free(der);
        X509_free(cert);
    }
    serial_input = NULL;
    EVP_PKEY_free(pkey);
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
        struct value *body = r->json, *history = (struct value *)get(body, "messages");
        struct value *result = (struct value *)get(history->child->next, "content")->child;
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
struct held_job {
    struct peer peer;
};
static void held_request(void *arg)
{
    struct held_job *job = arg;
    serve_request(&job->peer);
}
static void held_test(void)
{
    struct server s;
    struct wire turn = {0}, held = {0}, release = {0};
    struct request *r, *control;
    struct held_job job;
    struct peer p;
    service_thread thread;
    setup(&s, "{\"steps\":[{\"hold\":true,\"request\":{\"path\":\"/"
              "turn\"},\"reply\":{\"status\":404}}]}");
    r = get_request("GET", "/turn", "", "[]");
    job.peer = test_peer(&s, r, &turn);
    check(!service_thread_start(&thread, held_request, &job), "held request thread starts");
    service_lock(&s.lock);
    while (!s.held)
        service_wait(&s.condition, &s.lock);
    check(turn.out.len == 0 && !s.steps[0].played, "held reply writes nothing before release");
    service_unlock(&s.lock);
    control = get_request("GET", "/fakeapi/held", "", "[]");
    p = test_peer(&s, control, &held);
    serve_request(&p);
    check(strstr(held.out.data, "held 1\n") != NULL, "held control identifies the request");
    free_request(control);
    control = get_request("POST", "/fakeapi/release", "", "[]");
    p = test_peer(&s, control, &release);
    serve_request(&p);
    service_thread_join(thread);
    check(s.requests == 1 && !verdict(&s) && strstr(turn.out.data, "404 Not Found"),
          "release plays the held step without numbering control requests");
    free_request(control);
    free_request(r);
    clear(&turn.out);
    clear(&held.out);
    clear(&release.out);
    dispose(&s);
}
static void input_frame(struct buffer *b, unsigned opcode, int final, const char *data)
{
    unsigned char head[8];
    const unsigned char mask[4] = {1, 2, 3, 4};
    size_t n = strlen(data), i, size = 2;
    head[0] = (final ? 128 : 0) | opcode;
    if (n < 126)
        head[1] = 128 | (unsigned char)n;
    else {
        head[1] = 254;
        head[2] = (unsigned char)(n >> 8);
        head[3] = (unsigned char)n;
        size = 4;
    }
    append(b, head, size);
    append(b, mask, 4);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i] ^ mask[i % 4];
        append(b, &c, 1);
    }
}
static void websocket_test(void)
{
    unsigned test;
    for (test = 0; test < 3; test++) {
        struct server s;
        struct wire w = {0};
        struct buffer input = {0};
        struct request *r;
        struct peer p;
        setup(&s, "{\"steps\":[{\"request\":{\"path\":\"/"
                  "ws\"},\"reply\":{\"websocket\":[{\"expect\":\"hello\"},{\"send\":\"bye\"}]}}]}");
        r = get_request(
            "GET", "/ws", "",
            "[[\"Upgrade\",\"websocket\"],[\"Sec-WebSocket-Key\",\"dGhlIHNhbXBsZSBub25jZQ==\"]]");
        if (test) {
            input_frame(&input, 1, 0, "hel");
            input_frame(&input, 9, 1, "ping");
            input_frame(&input, 0, 1, "lo");
            if (test == 2)
                input_frame(&input, 8, 1, "\003\350");
        }
        w.in = (const unsigned char *)input.data;
        w.left = input.len;
        p = test_peer(&s, r, &w);
        serve_request(&p);
        check(test ? !verdict(&s) && s.steps[0].played : verdict(&s) && !s.steps[0].played,
              test ? "fragmented WebSocket message and ping play step before closing reset"
                   : "reset before expected WebSocket message fails the step");
        check(strstr(w.out.data, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL,
              "WebSocket upgrade accept digest");
        clear(&input);
        clear(&w.out);
        free_request(r);
        dispose(&s);
    }
}
static void http_test(void)
{
    const char *bytes =
        "POST /echo?one=1 HTTP/1.1\r\nHost: test\r\nExpect: 100-continue\r\nTransfer-Encoding: "
        "CHUNKED\r\n\r\n7;ext=yes\r\n{\"a\":1}\r\n0\r\nTrailer: value\r\n\r\n";
    struct wire w = {0};
    struct server s;
    struct peer p;
    struct request *r;
    int malformed;
    setup(&s, "{\"steps\":[]}");
    p = test_peer(&s, NULL, &w);
    w.in = (const unsigned char *)bytes;
    w.left = strlen(bytes);
    r = request(&p, &malformed);
    check(r && !malformed && !strcmp(r->body, "{\"a\":1}") && !strcmp(r->query, "one=1") &&
              strstr(w.out.data, "100 Continue"),
          "HTTP chunk extensions, trailers and continue handshake");
    if (r)
        free_request(r);
    clear(&w.out);
    dispose(&s);
}
#ifndef _WIN32
static char stop_record[4096], stop_status[4096];

/** Remove only the two files owned by the shutdown regression. */
static void stop_cleanup(void)
{
    if (*stop_status)
        unlink(stop_status);
    if (*stop_record)
        unlink(stop_record);
}

/** Check every recorded request, including the final successful exchange. */
static void stop_records(struct server *s, int refused)
{
    char entry[4096];
    unsigned i, n = refused ? 2 : 1;

    rewind(s->record);
    for (i = 0; i < n; i++) {
        struct value *v = fgets(entry, sizeof(entry), s->record) ? parse(entry) : NULL;
        const char *path = refused && !i ? "/nothing" : "/last";
        check(v && number(v, "n", 0) == i + 1 && !strcmp(field(v, "path"), path) &&
                  (refused && !i ? get(v, "unexpected") != NULL : number(v, "step", 0) == 1),
              "shutdown retains every request and its exact outcome");
        destroy(v);
    }
    check(fgetc(s->record) == EOF && !ferror(s->record), "request record ends exactly at EOF");
}

/** A second TERM must not terminate verdict or status serialization. */
static void stop_case(int refused)
{
    struct server s;
    struct wire w = {0};
    struct request *r;
    struct peer p;
    FILE *file, *record_file;
    char status[8] = {0};
    int i, result;
    const char *expected =
        refused ? "fakeapi: result: fail: 1 failed, the first request 1: GET /nothing: "
                  "step 1: the path is /nothing, not /last\n"
                : "fakeapi: result: pass: 1 steps played by 1 requests\n";

    record_file = fopen(stop_record, "w+b");
    check(record_file != NULL, "shutdown fixture opens its owned request record");
    if (!record_file)
        return;
    setup(&s, "{\"steps\":[{\"request\":{\"path\":\"/last\"},\"reply\":{\"body\":\"done\"}}]}");
    fclose(s.record);
    s.record = record_file;
    for (i = 0; i <= refused; i++) {
        r = get_request("GET", refused && !i ? "/nothing" : "/last", "", "[]");
        p = test_peer(&s, r, &w);
        serve_request(&p);
        free_request(r);
    }
    stopped = 0;
    check(pthread_kill(pthread_self(), SIGTERM) == 0 && stopped,
          "first TERM handler has completed before shutdown");
    repeated_stop = 0;
    interrupt_verdict = 1;
    result = verdict(&s);
    check(repeated_stop && stopped && !interrupt_verdict,
          "separate TERM during verdict preserves the installed handler");
    check(stopped_verdict.data && !strcmp(stopped_verdict.data, expected),
          "shutdown prints the exact pass or intentional refusal verdict");
    check(write_status(stop_record, result) == refused,
          "service returns its exact pass or intentional refusal exit status");
    file = fopen(stop_status, "rb");
    check(file && fgets(status, sizeof(status), file) && !strcmp(status, refused ? "1\n" : "0\n") &&
              fgetc(file) == EOF && !ferror(file),
          "a printed verdict also has its complete exact status file");
    if (file)
        fclose(file);
    stop_records(&s, refused);
    clear(&stopped_verdict);
    clear(&w.out);
    dispose(&s);
}

static void stop_test(void)
{
    const char *tmp = getenv("TMPDIR");
    int fd, length;
    size_t n;
    struct buffer invalid = {0};

    if (!tmp || !*tmp)
        tmp = "/tmp";
    if (atexit(stop_cleanup)) {
        check(0, "shutdown fixture cleanup installs before file creation");
        return;
    }
    length = snprintf(stop_record, sizeof(stop_record), "%s/rach2-fakeapi-stop.XXXXXX", tmp);
    if (length < 0 || length >= (int)sizeof(stop_record) - 7) {
        *stop_record = 0;
        check(0, "shutdown fixture path fits");
        return;
    }
    fd = mkstemp(stop_record);
    check(fd >= 0, "shutdown fixture owns a unique record path");
    if (fd < 0) {
        *stop_record = 0;
        return;
    }
    close(fd);
    n = strlen(stop_record);
    memcpy(stop_status, stop_record, n);
    memcpy(stop_status + n, ".status", 8);
    if (stop_handlers()) {
        check(0, "persistent stop handlers install");
        return;
    }
    stop_case(0);
    stop_case(1);
    putsb(&invalid, stop_record);
    putsb(&invalid, "/blocked");
    check(write_status(invalid.data, 0) == 2,
          "a printed pass cannot turn an unwritable status file into success");
    clear(&invalid);
    stop_cleanup();
    *stop_record = *stop_status = 0;
    stopped = 0;
}
#endif
/** Write an owned audit input, closing it on every path. */
static int audit_write(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    int ok;
    if (!file)
        return 0;
    ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok;
}

/** Exercise the file audit with an offline row and missing API exchanges. */
static void audit_test(void)
{
    const char *script = fix_path("script.json"), *record_path = fix_path("record.jsonl");
    const char *bad[] = {"{\"unexpected\":true}\n",
                         "{\"method\":\"GET\",\"path\":\"/v1/responses\",\"step\":1}\n"};
    size_t i;

    check(audit_write(script, "{\"steps\":[]}\n") && audit_write(record_path, ""),
          "offline audit inputs are written");
    check(!verify_record(script, record_path, "codex", "test"),
          "Codex startup accepts its declared empty exchange");
    check(verify_record(script, record_path, "codex-exec", "test") != 0 &&
              verify_record(script, record_path, "codex-api", "test") != 0,
          "Codex API workloads still require their actual WebSocket exchange");
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        check(audit_write(record_path, bad[i]), "invalid offline audit record writes");
        check(verify_record(script, record_path, "codex", "test") != 0,
              "offline startup rejects unexpected entries and undeclared requests");
    }
}

int main(void)
{
    serial_test();
#ifndef _WIN32
    stop_test();
#endif
    audit_test();
    parser();
    selection();
    literal();
    messages();
    gemini();
    responses_test();
    held_test();
    websocket_test();
    http_test();
    if (!failures) {
        fix_passed();
        puts("fakeapi-test: PASS");
    }
    return failures ? 1 : 0;
}
