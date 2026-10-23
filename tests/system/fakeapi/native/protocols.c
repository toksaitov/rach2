/* protocols.c: literal HTTP, Anthropic Messages and Gemini content. */
#include "api.h"
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#else
#define strcasecmp _stricmp
#endif
static const char *dummy = "rach2-offline-dummy-key";
const char *responses_validate(const struct value *step);
const char *responses_check(const struct value *step, const struct request *r);
int responses_reply(const struct value *step, struct peer *p);
static const char *reason(const char *format, ...)
{
    static _Thread_local char message[4096];
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    return message;
}
static size_t utf8_span(const char *text_value, size_t characters)
{
    size_t n = 0;
    while (text_value[n] && characters--) {
        n++;
        while (((unsigned char)text_value[n] & 0xc0) == 0x80)
            n++;
    }
    return n;
}
static const char *model(const struct value *step, const char *fallback)
{
    return *field(step, "model") ? field(step, "model") : fallback;
}
static const struct value *first(const struct value *v)
{
    return v ? v->child : NULL;
}
static char *text_of(const struct value *v, const char *separator)
{
    struct buffer b = {0};
    const struct value *part;
    if (v && v->type == 's')
        return strdup(text(v));
    for (part = first(v); part; part = part->next) {
        if (b.len)
            putsb(&b, separator);
        putsb(&b, field(part, "text"));
    }
    if (!b.data)
        putsb(&b, "");
    return b.data;
}
static int contains(const struct value *v, const char *actual)
{
    const struct value *p;
    if (!actual)
        return 0;
    if (get(v, "equals") && strcmp(field(v, "equals"), actual))
        return 0;
    for (p = first(get(v, "contains")); p; p = p->next)
        if (!strstr(actual, text(p)))
            return 0;
    return 1;
}
static const char *validate_plain(const struct value *s)
{
    const struct value *r = get(s, "request"), *reply = get(s, "reply"), *action;
    unsigned n = 0;
    const char *kinds[] = {"body", "json", "events", "websocket"};
    size_t i;
    if (!r || r->type != 'o' || !get(r, "path") || get(r, "path")->type != 's')
        return "the step has no request with a path";
    if (!reply || reply->type != 'o')
        return "the step has no reply";
    for (i = 0; i < 4; i++)
        if (get(reply, kinds[i]))
            n++;
    if (n > 1)
        return "the reply has more than one body kind";
    for (action = first(get(reply, "websocket")); action; action = action->next)
        if (action->type != 'o' || count(action) != 1 ||
            (!get(action, "send") && !get(action, "expect") && !get(action, "close")))
            return "an invalid WebSocket action";
    return NULL;
}
static const char *check_plain(const struct value *s, const struct request *r)
{
    const struct value *e = get(s, "request"), *h;
    const char *method = *field(e, "method") ? field(e, "method") : "GET", *why;
    if (strcmp(r->method, method))
        return reason("the method is %s, not %s", r->method, method);
    if (strcmp(r->path, field(e, "path")))
        return reason("the path is %s, not %s", r->path, field(e, "path"));
    if (get(e, "query") && strcmp(r->query, field(e, "query")))
        return "the query differs from the expected value";
    for (h = first(get(e, "headers")); h; h = h->next)
        if (strcmp(header(r, h->key), text(h)))
            return reason("the header %s differs from the expected value", h->key);
    if (get(get(s, "reply"), "websocket") && strcasecmp(header(r, "upgrade"), "websocket"))
        return "the request does not ask for a WebSocket";
    if (get(e, "body") && strcmp(r->body, field(e, "body")))
        return "the body differs from the expected value";
    if (get(e, "json")) {
        if (!r->json)
            return "the body is not JSON";
        why = subset(get(e, "json"), r->json);
        if (why)
            return reason("in the body, %s", why);
    }
    return NULL;
}
static int reply_plain(const struct value *s, struct peer *p)
{
    const struct value *r = get(s, "reply"), *h = get(r, "headers"), *v;
    int status = (int)number(r, "status", 200), ok;
    if (get(r, "websocket")) {
        if (!websocket_start(p))
            return 0;
        for (v = first(get(r, "websocket")); v; v = v->next) {
            const struct value *e = get(v, "expect"), *sent = get(v, "send");
            if (sent) {
                char *message = sent->type == 's' ? strdup(text(sent)) : encode(sent, 0);
                ok = websocket_send(p, message);
                free(message);
                if (!ok)
                    return 0;
            } else if (get(v, "close"))
                return websocket_close(p, (unsigned)number(v, "close", 1000));
            else if (e) {
                char *message = websocket_receive(p);
                struct value *actual = NULL;
                const char *why;
                if (!message) {
                    fail_peer(p, "the WebSocket closed where the script expects a message");
                    return 0;
                }
                if (e->type == 's')
                    why = strcmp(text(e), message) ? "the text differs" : NULL;
                else {
                    actual = parse(message);
                    why = actual ? subset(e, actual) : "the message is not JSON";
                }
                if (why)
                    fail_peer(p, "a WebSocket message the script does not expect: %s", why);
                destroy(actual);
                free(message);
                if (p->failed)
                    return websocket_close(p, 1008);
            }
        }
        return websocket_close(p, 1000);
    }
    if (get(r, "events")) {
        if (!events_start(p, status, h))
            return 0;
        for (v = first(get(r, "events")); v; v = v->next)
            if (!event(p, get(v, "event") ? field(v, "event") : NULL,
                       get(v, "id") ? field(v, "id") : NULL, get(v, "data")))
                return 0;
        return events_end(p);
    }
    if (get(r, "json")) {
        char *body = encode(get(r, "json"), 0);
        ok = response(p, status, h, "application/json", body);
        free(body);
        return ok;
    }
    return response(p, status, h, get(r, "body") ? "text/plain; charset=utf-8" : NULL,
                    field(r, "body"));
}
static const char *validate_results(const struct value *s)
{
    const struct value *r;
    for (r = first(get(s, "results")); r; r = r->next)
        if (!get(r, "id") || get(r, "id")->type != 's' ||
            (!get(r, "contains") && !get(r, "equals")))
            return "a required result needs id and contains or equals";
    return NULL;
}
static const char *validate_anthropic(const struct value *s)
{
    const struct value *r = get(s, "reply"), *b;
    if (!r || r->type != 'a' || !r->child)
        return "reply must be a nonempty Messages content array";
    for (b = r->child; b; b = b->next) {
        if (!strcmp(field(b, "type"), "text") && get(b, "text") && get(b, "text")->type == 's')
            continue;
        if (!strcmp(field(b, "type"), "tool_use") && get(b, "id") && get(b, "name") &&
            get(b, "id")->type == 's' && get(b, "name")->type == 's' && get(b, "input") &&
            get(b, "input")->type == 'o')
            continue;
        return "reply has an unsupported content block";
    }
    return validate_results(s);
}
static const char *anthropic_request(const struct value *s, const struct request *r)
{
    const struct value *b = r->json, *v;
    const char *why = NULL;
    if (strcmp(r->method, "POST") || strcmp(r->path, "/v1/messages")) {
        why = "expected POST /v1/messages";
        return why;
    }
    if (*r->query && strcmp(r->query, "beta=true")) {
        why = "unexpected Messages query";
        return why;
    }
    if (strcmp(header(r, "x-api-key"), dummy)) {
        why = "unexpected x-api-key header";
        return why;
    }
    if (strcmp(header(r, "anthropic-version"), "2023-06-01")) {
        why = "unexpected anthropic-version header";
        return why;
    }
    if (strncmp(header(r, "content-type"), "application/json", 16)) {
        why = "expected JSON content type";
        return why;
    }
    if (!b || b->type != 'o') {
        why = "expected a JSON Messages body";
        return why;
    }
    if (strcmp(field(b, "model"), model(s, "claude-sonnet-4-6")) ||
        flag(b, "stream", 0) != flag(s, "stream", 1)) {
        why = "unexpected model or stream setting";
        return why;
    }
    v = get(b, "max_tokens");
    if (!v || v->type != 'n' || v->number <= 0 || v->number != (long long)v->number) {
        why = "expected a positive max_tokens";
        return why;
    }
    return why;
}
static const char *anthropic_tools(const struct value *s, const struct request *r)
{
    const struct value *b = r->json, *v;
    const char *why = NULL;
    for (v = first(get(s, "tools")); v; v = v->next) {
        const struct value *tool;
        int found = 0;
        for (tool = first(get(b, "tools")); tool; tool = tool->next)
            if (!strcmp(field(tool, "name"), text(v)) && get(tool, "input_schema") &&
                get(tool, "input_schema")->type == 'o')
                found = 1;
        if (!found) {
            why = reason("missing tool definition: %s", text(v));
            return why;
        }
    }
    if (get(s, "body") && (why = subset(get(s, "body"), b))) {
        why = reason("unexpected body: %s", why);
        return why;
    }
    return why;
}
static const char *anthropic_history(const struct request *r, struct value *calls,
                                     struct value *results)
{
    const struct value *b = r->json, *m, *block;
    const char *why = NULL;
    for (m = first(get(b, "messages")); m; m = m->next) {
        const struct value *content = get(m, "content");
        if (!content || content->type != 'a')
            continue;
        for (block = content->child; block; block = block->next) {
            if (!strcmp(field(m, "role"), "assistant") && !strcmp(field(block, "type"), "tool_use"))
                set(calls, field(block, "id"), clone(block));
            if (!strcmp(field(m, "role"), "user") && !strcmp(field(block, "type"), "tool_result")) {
                const char *id = field(block, "tool_use_id");
                if (!get(calls, id) || get(results, id)) {
                    why = "a tool result has no preceding call or is duplicated";
                    return why;
                }
                add(results, id, clone(block));
            }
        }
    }
    return why;
}
static const char *anthropic_results(const struct value *s, const struct value *results)
{
    const struct value *v, *block;
    const char *why = NULL;
    for (v = first(get(s, "results")); v; v = v->next) {
        const char *id = field(v, "id");
        char *actual;
        const struct value *part;
        block = get(results, id);
        if (!block || flag(block, "is_error", 0)) {
            why = reason("missing or failed tool result: %s", id);
            return why;
        }
        for (part = first(get(block, "content")); part; part = part->next)
            if (strcmp(field(part, "type"), "text")) {
                why = reason("wrong tool result: %s", id);
                return why;
            }
        actual = text_of(get(block, "content"), "\n");
        if (!contains(v, actual))
            why = reason("wrong tool result: %s", id);
        free(actual);
        if (why)
            return why;
    }
    return why;
}
static const char *check_anthropic(const struct value *s, const struct request *r)
{
    struct value *calls = value('o'), *results = value('o');
    const char *why = anthropic_request(s, r);
    if (!why)
        why = anthropic_tools(s, r);
    if (!why)
        why = anthropic_history(r, calls, results);
    if (!why)
        why = anthropic_results(s, results);
    destroy(calls);
    destroy(results);
    return why;
}

static struct value *typed(const char *type)
{
    struct value *v = value('o');
    add(v, "type", string(type));
    return v;
}
static int emit(struct peer *p, const char *name, struct value *data)
{
    int ok = event(p, name, NULL, data);
    destroy(data);
    return ok;
}
static int reply_anthropic(const struct value *s, struct peer *p)
{
    const struct value *content = get(s, "reply"), *block;
    struct value *message, *usage, *data;
    const char *stop_reason = "end_turn";
    struct buffer id = {0};
    size_t index = 0;
    int ok = 1;
    for (block = content->child; block; block = block->next)
        if (!strcmp(field(block, "type"), "tool_use"))
            stop_reason = "tool_use";
    putsb(&id, "msg_");
    putsb(&id, *field(content->child, "id") ? field(content->child, "id") : "done");
    message = typed("message");
    add(message, "id", string(id.data));
    clear(&id);
    add(message, "role", string("assistant"));
    add(message, "model", string(model(s, "claude-sonnet-4-6")));
    add(message, "content", clone(content));
    add(message, "stop_sequence", value('z'));
    add(message, "stop_reason", string(stop_reason));
    usage = value('o');
    add(usage, "input_tokens", integer(128));
    add(usage, "output_tokens", integer(32));
    add(message, "usage", usage);
    if (!flag(s, "stream", 1)) {
        ok = json_response(p, 200, message);
        destroy(message);
        return ok;
    }
    if (!events_start(p, 200, NULL)) {
        destroy(message);
        return 0;
    }
    data = typed("message_start");
    {
        struct value *start = clone(message);
        set(start, "content", value('a'));
        set(start, "stop_reason", value('z'));
        add(data, "message", start);
    }
    if (!emit(p, "message_start", data) || !emit(p, "ping", typed("ping"))) {
        destroy(message);
        return 0;
    }
    for (block = content->child; block; block = block->next, index++) {
        int tool = !strcmp(field(block, "type"), "tool_use");
        size_t offset, len;
        struct value *initial = clone(block);
        char *body;
        set(initial, tool ? "input" : "text", tool ? value('o') : string(""));
        data = typed("content_block_start");
        add(data, "index", integer((long long)index));
        add(data, "content_block", initial);
        if (!emit(p, "content_block_start", data)) {
            ok = 0;
            break;
        }
        body = tool ? encode(get(block, "input"), 0) : strdup(field(block, "text"));
        if (!body) {
            fail_peer(p, "cannot allocate a content block");
            ok = 0;
            break;
        }
        len = strlen(body);
        for (offset = 0; offset < len;) {
            char part[29];
            struct value *delta = typed(tool ? "input_json_delta" : "text_delta");
            size_t n = utf8_span(body + offset, 7);
            memcpy(part, body + offset, n);
            part[n] = 0;
            offset += n;
            add(delta, tool ? "partial_json" : "text", string(part));
            data = typed("content_block_delta");
            add(data, "index", integer((long long)index));
            add(data, "delta", delta);
            if (!emit(p, "content_block_delta", data)) {
                ok = 0;
                break;
            }
        }
        free(body);
        if (!ok)
            break;
        data = typed("content_block_stop");
        add(data, "index", integer((long long)index));
        if (!emit(p, "content_block_stop", data)) {
            ok = 0;
            break;
        }
    }
    if (ok) {
        struct value *delta = value('o'), *tokens = value('o');
        add(delta, "stop_reason", string(stop_reason));
        add(delta, "stop_sequence", value('z'));
        add(tokens, "output_tokens", integer(32));
        data = typed("message_delta");
        add(data, "delta", delta);
        add(data, "usage", tokens);
        ok = emit(p, "message_delta", data) && emit(p, "message_stop", typed("message_stop")) &&
             events_end(p);
    }
    destroy(message);
    return ok;
}
static char *signature(const char *id)
{
    struct buffer b = {0};
    unsigned char *encoded;
    putsb(&b, "rach2-signature-");
    putsb(&b, id);
    encoded = allocate(4 * ((b.len + 2) / 3) + 1);
    EVP_EncodeBlock(encoded, (unsigned char *)b.data, (int)b.len);
    clear(&b);
    return (char *)encoded;
}
static const char *validate_gemini(const struct value *s)
{
    const struct value *r = get(s, "reply"), *p;
    if (!r || r->type != 'a' || !r->child)
        return "reply must be a nonempty list of content parts";
    for (p = r->child; p; p = p->next) {
        const struct value *c = get(p, "functionCall");
        if (count(p) == 1 && get(p, "text") && get(p, "text")->type == 's')
            continue;
        if (count(p) == 1 && c && c->type == 'o' && get(c, "id") && get(c, "name") &&
            get(c, "id")->type == 's' && get(c, "name")->type == 's' && get(c, "args") &&
            get(c, "args")->type == 'o')
            continue;
        return "reply has an unsupported content part";
    }
    return validate_results(s);
}
static char *content_text(const struct value *content)
{
    struct buffer b = {0};
    const struct value *p;
    for (p = first(get(content, "parts")); p; p = p->next)
        if (!flag(p, "thought", 0))
            putsb(&b, field(p, "text"));
    if (!b.data)
        putsb(&b, "");
    return b.data;
}
static const char *gemini_request(const struct value *s, const struct request *r)
{
    char path[1024];
    const struct value *b = r->json, *contents;
    const char *why = NULL;
    snprintf(path, sizeof(path), "/v1beta/models/%s:%s", model(s, "gemini-3.1-pro-preview"),
             flag(s, "stream", 1) ? "streamGenerateContent" : "generateContent");
    if (strcmp(r->method, "POST") || strcmp(r->path, path)) {
        why = reason("expected POST %s", path);
        return why;
    }
    if (strcmp(r->query, flag(s, "stream", 1) ? "alt=sse" : "")) {
        why = "unexpected generateContent query";
        return why;
    }
    if (strcmp(header(r, "x-goog-api-key"), dummy)) {
        why = "unexpected x-goog-api-key header";
        return why;
    }
    if (strncmp(header(r, "content-type"), "application/json", 16)) {
        why = "expected JSON content type";
        return why;
    }
    if (!b || b->type != 'o') {
        why = "expected a JSON generateContent body";
        return why;
    }
    contents = get(b, "contents");
    if (!contents || contents->type != 'a' || !contents->child ||
        strcmp(field(contents->child, "role"), "user")) {
        why = "expected contents that begin with a user turn";
        return why;
    }
    return why;
}
static const char *gemini_prompts_tools(const struct value *s, const struct request *r)
{
    const struct value *b = r->json, *contents = get(b, "contents"), *v;
    const char *why = NULL;
    if (get(s, "system")) {
        char *actual = content_text(get(b, "systemInstruction"));
        int ok = strstr(actual, field(s, "system")) != NULL;
        free(actual);
        if (!ok) {
            why = "the system instruction lacks the step's fragment";
            return why;
        }
    }
    if (get(s, "prompt")) {
        char *actual = content_text(contents->child);
        int ok = strstr(actual, field(s, "prompt")) != NULL;
        free(actual);
        if (!ok) {
            why = "the first user turn lacks the step's prompt";
            return why;
        }
    }
    for (v = first(get(s, "tools")); v; v = v->next) {
        const struct value *tool, *f;
        int found = 0;
        for (tool = first(get(b, "tools")); tool; tool = tool->next)
            for (f = first(get(tool, "functionDeclarations")); f; f = f->next)
                if (!strcmp(field(f, "name"), text(v)))
                    found = 1;
        if (!found) {
            why = reason("missing function declaration: %s", text(v));
            return why;
        }
    }
    return why;
}
static const char *gemini_history(const struct value *contents, struct value *calls,
                                  struct value *results)
{
    const struct value *content, *part;
    const char *why = NULL;
    for (content = contents->child; content; content = content->next) {
        for (part = first(get(content, "parts")); part; part = part->next) {
            const struct value *call = get(part, "functionCall"),
                               *reply = get(part, "functionResponse");
            if (!strcmp(field(content, "role"), "model") && call && call->type == 'o') {
                char *expected = signature(field(call, "id"));
                int ok = !strcmp(expected, field(part, "thoughtSignature"));
                free(expected);
                if (!ok) {
                    why = reason("a call lacks its thought signature: %s", field(call, "id"));
                    return why;
                }
                set(calls, field(call, "id"), clone(call));
            }
            if (reply && reply->type == 'o') {
                const char *id = field(reply, "id");
                const struct value *previous = get(calls, id);
                if (!previous || get(results, id) ||
                    strcmp(field(reply, "name"), field(previous, "name"))) {
                    why = "a function response has no preceding call or is duplicated";
                    return why;
                }
                add(results, id, clone(get(reply, "response")));
            }
        }
    }
    return why;
}
static const char *gemini_results(const struct value *s, const struct value *results)
{
    const struct value *v;
    const char *why = NULL;
    for (v = first(get(s, "results")); v; v = v->next) {
        const char *id = field(v, "id");
        const struct value *reply = get(results, id), *output;
        char *actual;
        int ok;
        if (!reply || get(reply, "error")) {
            why = reason("missing or failed function response: %s", id);
            return why;
        }
        output = get(reply, "output");
        actual = output && output->type == 's' ? strdup(text(output))
                 : output                      ? encode(output, 0)
                                               : NULL;
        ok = contains(v, actual);
        free(actual);
        if (!ok) {
            why = reason("wrong function response: %s", id);
            return why;
        }
    }
    return why;
}
static const char *check_gemini(const struct value *s, const struct request *r)
{
    struct value *calls = value('o'), *results = value('o');
    const char *why = gemini_request(s, r);
    if (!why)
        why = gemini_prompts_tools(s, r);
    if (!why)
        why = gemini_history(get(r->json, "contents"), calls, results);
    if (!why)
        why = gemini_results(s, results);
    destroy(calls);
    destroy(results);
    return why;
}

static struct value *gemini_response(const struct value *s, struct value *parts, size_t total,
                                     int last)
{
    struct value *r = value('o'), *candidates = value('a'), *candidate = value('o'),
                 *content = value('o');
    char id[80];
    snprintf(id, sizeof(id), "rach2-%lu", (unsigned long)total);
    add(content, "role", string("model"));
    add(content, "parts", parts);
    add(candidate, "content", content);
    add(candidate, "index", integer(0));
    if (last)
        add(candidate, "finishReason", string("STOP"));
    add(candidates, NULL, candidate);
    add(r, "candidates", candidates);
    add(r, "modelVersion", string(model(s, "gemini-3.1-pro-preview")));
    add(r, "responseId", string(id));
    if (last) {
        struct value *usage = value('o');
        add(usage, "promptTokenCount", integer(128));
        add(usage, "candidatesTokenCount", integer(32));
        add(usage, "totalTokenCount", integer(160));
        add(r, "usageMetadata", usage);
    }
    return r;
}
static int reply_gemini(const struct value *s, struct peer *p)
{
    const struct value *part;
    struct value *parts = value('a');
    size_t total, index = 0;
    int ok = 1;
    for (part = first(get(s, "reply")); part; part = part->next) {
        if (get(part, "text")) {
            const char *text_value = field(part, "text");
            size_t offset, n = strlen(text_value);
            for (offset = 0; offset < n;) {
                struct value *v = value('o');
                char chunk[29];
                size_t len = utf8_span(text_value + offset, 7);
                memcpy(chunk, text_value + offset, len);
                chunk[len] = 0;
                offset += len;
                add(v, "text", string(chunk));
                add(parts, NULL, v);
            }
        } else {
            struct value *v = clone(part);
            char *sig = signature(field(get(part, "functionCall"), "id"));
            add(v, "thoughtSignature", string(sig));
            free(sig);
            add(parts, NULL, v);
        }
    }
    total = count(parts);
    if (!total) {
        destroy(parts);
        fail_peer(p, "the Gemini reply has no content");
        return 0;
    }
    if (!flag(s, "stream", 1)) {
        struct value *r = gemini_response(s, parts, total, 1);
        ok = json_response(p, 200, r);
        destroy(r);
        return ok;
    }
    if (!events_start(p, 200, NULL)) {
        destroy(parts);
        return 0;
    }
    for (part = parts->child; part; part = part->next, index++) {
        struct value *one = value('a'), *r;
        add(one, NULL, clone(part));
        r = gemini_response(s, one, total, index + 1 == total);
        ok = event(p, NULL, NULL, r);
        destroy(r);
        if (!ok)
            break;
    }
    destroy(parts);
    return ok && events_end(p);
}
const char *protocol_validate(const char *name, const struct value *step)
{
    if (!step || step->type != 'o')
        return "the step is not an object";
    if (!strcmp(name, "plain"))
        return validate_plain(step);
    if (!strcmp(name, "anthropic"))
        return validate_anthropic(step);
    if (!strcmp(name, "gemini"))
        return validate_gemini(step);
    if (!strcmp(name, "responses"))
        return responses_validate(step);
    return "unknown API protocol";
}
const char *protocol_check(const char *name, const struct value *step, const struct request *r)
{
    if (!strcmp(name, "plain"))
        return check_plain(step, r);
    if (!strcmp(name, "anthropic"))
        return check_anthropic(step, r);
    if (!strcmp(name, "gemini"))
        return check_gemini(step, r);
    return responses_check(step, r);
}
int protocol_reply(const char *name, const struct value *step, struct peer *p)
{
    if (!strcmp(name, "plain"))
        return reply_plain(step, p);
    if (!strcmp(name, "anthropic"))
        return reply_anthropic(step, p);
    if (!strcmp(name, "gemini"))
        return reply_gemini(step, p);
    return responses_reply(step, p);
}
