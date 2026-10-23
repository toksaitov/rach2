/* responses.c: Responses over HTTP events and continued WebSocket turns. */
#include "api.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#else
#define strcasecmp _stricmp
#endif
static const struct value *first(const struct value *v)
{
    return v ? v->child : NULL;
}
static const char *reason(const char *format, ...)
{
    static _Thread_local char why[4096];
    va_list ap;
    va_start(ap, format);
    vsnprintf(why, sizeof(why), format, ap);
    va_end(ap);
    return why;
}
static int call_type(const struct value *v)
{
    return !strcmp(field(v, "type"), "function_call") ||
           !strcmp(field(v, "type"), "custom_tool_call");
}
static int result_type(const struct value *v)
{
    return !strcmp(field(v, "type"), "function_call_output") ||
           !strcmp(field(v, "type"), "custom_tool_call_output");
}
static struct value *typed(const char *type)
{
    struct value *v = value('o');
    add(v, "type", string(type));
    return v;
}
static const char *validate_turn(const struct value *t)
{
    const struct value *v;
    if (!t || t->type != 'o' || !get(t, "output") || get(t, "output")->type != 'a')
        return "a turn has no output array";
    for (v = first(get(t, "input")); v; v = v->next)
        if (v->type != 'o' || !get(v, "text") || !!get(v, "role") == !!get(v, "output_of"))
            return "an invalid input to match";
    for (v = first(get(t, "output")); v; v = v->next) {
        if (!!get(v, "message") + !!get(v, "call") + !!get(v, "custom") != 1)
            return "an invalid output item";
        if (!get(v, "message") && (!get(v, "id") || get(v, "id")->type != 's'))
            return "an output item has no call id";
    }
    return NULL;
}
const char *responses_validate(const struct value *s)
{
    const struct value *v;
    const char *why;
    if (!!get(s, "websocket") + !!get(s, "refuse") + !!get(s, "turn") != 1)
        return "a Responses step needs one of websocket, refuse and turn";
    if (get(s, "refuse"))
        return get(s, "refuse")->type == 'n' ? NULL : "the refusal has no status";
    if (get(s, "turn"))
        return validate_turn(get(s, "turn"));
    v = get(s, "websocket");
    if (v->type != 'a' || !v->child)
        return "the WebSocket has no turns";
    for (v = v->child; v; v = v->next)
        if ((why = validate_turn(v)))
            return why;
    return NULL;
}
static struct value *conversation(const struct value *responses, const struct value *body,
                                  const char **why)
{
    const struct value *previous = get(body, "previous_response_id"), *input = get(body, "input"),
                       *part;
    struct value *items = value('a');
    *why = NULL;
    if (previous && previous->type != 'z') {
        const struct value *old = get(responses, text(previous));
        if (!old) {
            *why = reason("the previous response %s was not found", text(previous));
            destroy(items);
            return NULL;
        }
        for (part = old->child; part; part = part->next)
            add(items, NULL, clone(part));
    }
    if (input && input->type == 's') {
        struct value *message = typed("message"), *content = value('a'),
                     *text_part = typed("input_text");
        add(text_part, "text", clone(input));
        add(content, NULL, text_part);
        add(message, "role", string("user"));
        add(message, "content", content);
        add(items, NULL, message);
    } else if (!input || input->type == 'a') {
        for (part = first(input); part; part = part->next)
            add(items, NULL, clone(part));
    } else {
        *why = "the input is not a list of items";
        destroy(items);
        return NULL;
    }
    return items;
}
static char *text_of(const struct value *v)
{
    struct buffer b = {0};
    const struct value *p;
    if (v && v->type == 's')
        return strdup(text(v));
    if (v && v->type == 'a')
        for (p = v->child; p; p = p->next) {
            if (b.len)
                putsb(&b, "\n");
            putsb(&b, field(p, "text"));
        }
    if (!b.data)
        putsb(&b, "");
    return b.data;
}
static int fragments(const struct value *pattern, const char *text_value)
{
    const struct value *p;
    if (pattern && pattern->type == 's')
        return strstr(text_value, text(pattern)) != NULL;
    for (p = first(pattern); p; p = p->next)
        if (!strstr(text_value, text(p)))
            return 0;
    return 1;
}
static const char *find(const struct value *items, const struct value *pattern)
{
    const struct value *i;
    int found = 0;
    for (i = first(items); i; i = i->next) {
        char *actual;
        int ok;
        if (get(pattern, "role")) {
            const char *type = field(i, "type");
            if ((*type && strcmp(type, "message")) ||
                strcmp(field(i, "role"), field(pattern, "role")))
                continue;
            actual = text_of(get(i, "content"));
        } else {
            if (!result_type(i) || strcmp(field(i, "call_id"), field(pattern, "output_of")))
                continue;
            actual = text_of(get(i, "output"));
        }
        found = 1;
        ok = fragments(get(pattern, "text"), actual);
        free(actual);
        if (ok)
            return NULL;
    }
    if (get(pattern, "output_of"))
        return reason(found ? "the output of %s differs from the expected text"
                            : "the conversation has no output of %s",
                      field(pattern, "output_of"));
    return reason("a %s message with the expected text is not in the conversation",
                  field(pattern, "role"));
}
static int offered_in(const struct value *tools, const struct value *item)
{
    const struct value *tool;
    for (tool = first(tools); tool; tool = tool->next)
        if (!strcmp(field(tool, "type"), get(item, "call") ? "function" : "custom") &&
            !strcmp(field(tool, "name"),
                    get(item, "call") ? field(item, "call") : field(item, "custom")))
            return 1;
    return 0;
}
static int offered(const struct value *body, const struct value *item)
{
    const struct value *tools = get(body, "tools"), *space;
    if (!get(item, "namespace"))
        return offered_in(tools, item);
    for (space = first(tools); space; space = space->next)
        if (!strcmp(field(space, "type"), "namespace") &&
            !strcmp(field(space, "name"), field(item, "namespace")) &&
            offered_in(get(space, "tools"), item))
            return 1;
    return 0;
}
static const char *check_turn(const struct value *turn, const struct value *body,
                              const struct value *items)
{
    const struct value *i, *r;
    const char *why;
    struct value *expected;
    if (!get(body, "model") || get(body, "model")->type != 's')
        return "the request names no model";
    expected = clone(get(turn, "request"));
    if (expected->type == 'o') {
        struct value **q = &expected->child;
        while (*q) {
            if (!strcmp((*q)->key, "input")) {
                struct value *old = *q;
                *q = old->next;
                old->next = NULL;
                destroy(old);
                break;
            }
            q = &(*q)->next;
        }
        why = subset(expected, body);
        if (why) {
            why = reason("in the body, %s", why);
            destroy(expected);
            return why;
        }
    }
    destroy(expected);
    for (i = first(items); i; i = i->next)
        if (call_type(i) && *field(i, "call_id")) {
            int answered = 0;
            for (r = first(items); r; r = r->next)
                if (result_type(r) && !strcmp(field(r, "call_id"), field(i, "call_id")))
                    answered = 1;
            if (!answered)
                return reason("no tool output found for the call %s", field(i, "call_id"));
        }
    for (i = first(get(turn, "input")); i; i = i->next)
        if ((why = find(items, i)))
            return why;
    for (i = first(get(turn, "output")); i; i = i->next)
        if (!get(i, "message") && !offered(body, i))
            return reason("the request offers no tool %s",
                          get(i, "call") ? field(i, "call") : field(i, "custom"));
    return NULL;
}
const char *responses_check(const struct value *s, const struct request *r)
{
    const char *path = *field(s, "path") ? field(s, "path") : "/v1/responses", *why;
    struct value *items;
    if (strcmp(r->path, path))
        return reason("the path is %s, not %s", r->path, path);
    if (!get(s, "turn")) {
        if (strcmp(r->method, "GET") || strcasecmp(header(r, "upgrade"), "websocket"))
            return "the request does not ask for a WebSocket";
        return NULL;
    }
    if (strcmp(r->method, "POST") || !strcasecmp(header(r, "upgrade"), "websocket"))
        return "the request is not the POST of a response";
    if (!r->json)
        return "the body is not JSON";
    if (r->json->type != 'o')
        return "the body is not an object";
    if (!flag(r->json, "stream", 0))
        return "the request does not ask for a stream of events";
    if (get(r->json, "generate"))
        return "a warmup over HTTPS, which only WebSocket mode has";
    items = conversation(NULL, r->json, &why);
    if (items) {
        why = check_turn(get(s, "turn"), r->json, items);
        destroy(items);
    }
    return why;
}
static void ident(struct peer *p, struct value *v, const char *key, const char *prefix)
{
    char *id = new_id(p->server, prefix);
    add(v, key, string(id));
    free(id);
}
static struct value *output_items(const struct value *turn, struct peer *p)
{
    struct value *output = value('a');
    const struct value *item;
    for (item = first(get(turn, "output")); item; item = item->next) {
        struct value *v;
        if (get(item, "message")) {
            struct value *content = value('a'), *part = typed("output_text");
            v = typed("message");
            ident(p, v, "id", "msg");
            add(v, "status", string("completed"));
            add(v, "role", string("assistant"));
            add(part, "text", clone(get(item, "message")));
            add(part, "annotations", value('a'));
            add(content, NULL, part);
            add(v, "content", content);
        } else {
            int custom = get(item, "custom") != NULL;
            v = typed(custom ? "custom_tool_call" : "function_call");
            ident(p, v, "id", custom ? "ctc" : "fc");
            add(v, "status", string("completed"));
            add(v, "call_id", clone(get(item, "id")));
            add(v, "name", clone(get(item, custom ? "custom" : "call")));
            if (custom)
                add(v, "input", clone(get(item, "input")));
            else {
                const struct value *arguments = get(item, "arguments");
                char *encoded;
                struct value *empty = NULL;
                if (!arguments)
                    arguments = empty = value('o');
                encoded = encode(arguments, 0);
                add(v, "arguments", string(encoded));
                free(encoded);
                destroy(empty);
                if (get(item, "namespace"))
                    add(v, "namespace", clone(get(item, "namespace")));
            }
        }
        add(output, NULL, v);
    }
    return output;
}
static struct value *where(const char *type, const struct value *item, size_t index)
{
    struct value *v = typed(type);
    add(v, "item_id", clone(get(item, "id")));
    add(v, "output_index", integer((long long)index));
    return v;
}
static void stream_add(struct value *stream, struct value *event_value)
{
    add(event_value, "sequence_number", integer((long long)count(stream)));
    add(stream, NULL, event_value);
}
static struct value *events(struct peer *p, const struct value *body, const struct value *output)
{
    struct value *response_value = value('o'), *stream = value('a'), *v;
    const struct value *item;
    size_t index = 0;
    ident(p, response_value, "id", "resp");
    add(response_value, "object", string("response"));
    add(response_value, "status", string("in_progress"));
    add(response_value, "model", clone(get(body, "model")));
    add(response_value, "output", value('a'));
    v = typed("response.created");
    add(v, "response", clone(response_value));
    stream_add(stream, v);
    v = typed("response.in_progress");
    add(v, "response", clone(response_value));
    stream_add(stream, v);
    for (item = first(output); item; item = item->next, index++) {
        struct value *started = clone(item);
        int message = !strcmp(field(item, "type"), "message");
        int function = !strcmp(field(item, "type"), "function_call");
        set(started, "status", string("in_progress"));
        set(started,
            message    ? "content"
            : function ? "arguments"
                       : "input",
            message ? value('a') : string(""));
        v = typed("response.output_item.added");
        add(v, "output_index", integer((long long)index));
        add(v, "item", started);
        stream_add(stream, v);
        if (message) {
            const struct value *part = first(get(item, "content"));
            struct value *initial = typed("output_text");
            add(initial, "text", string(""));
            add(initial, "annotations", value('a'));
            v = where("response.content_part.added", item, index);
            add(v, "content_index", integer(0));
            add(v, "part", initial);
            stream_add(stream, v);
            v = where("response.output_text.delta", item, index);
            add(v, "content_index", integer(0));
            add(v, "delta", clone(get(part, "text")));
            stream_add(stream, v);
            v = where("response.output_text.done", item, index);
            add(v, "content_index", integer(0));
            add(v, "text", clone(get(part, "text")));
            stream_add(stream, v);
            v = where("response.content_part.done", item, index);
            add(v, "content_index", integer(0));
            add(v, "part", clone(part));
            stream_add(stream, v);
        } else if (function) {
            v = where("response.function_call_arguments.delta", item, index);
            add(v, "delta", clone(get(item, "arguments")));
            stream_add(stream, v);
            v = where("response.function_call_arguments.done", item, index);
            add(v, "arguments", clone(get(item, "arguments")));
            stream_add(stream, v);
        } else {
            v = where("response.custom_tool_call_input.delta", item, index);
            add(v, "call_id", clone(get(item, "call_id")));
            add(v, "delta", clone(get(item, "input")));
            stream_add(stream, v);
            v = where("response.custom_tool_call_input.done", item, index);
            add(v, "call_id", clone(get(item, "call_id")));
            add(v, "input", clone(get(item, "input")));
            stream_add(stream, v);
        }
        v = typed("response.output_item.done");
        add(v, "output_index", integer((long long)index));
        add(v, "item", clone(item));
        stream_add(stream, v);
    }
    set(response_value, "status", string("completed"));
    set(response_value, "output", clone(output));
    {
        struct value *usage = value('o'), *in = value('o'), *out = value('o');
        add(in, "cached_tokens", integer(0));
        add(out, "reasoning_tokens", integer(0));
        add(usage, "input_tokens", integer(0));
        add(usage, "input_tokens_details", in);
        add(usage, "output_tokens", integer(0));
        add(usage, "output_tokens_details", out);
        add(usage, "total_tokens", integer(0));
        add(response_value, "usage", usage);
    }
    v = typed("response.completed");
    add(v, "response", response_value);
    stream_add(stream, v);
    return stream;
}
static struct value *error(const char *why)
{
    struct value *v = typed("invalid_request_error");
    struct buffer message = {0};
    putsb(&message, "fakeapi: ");
    putsb(&message, why);
    add(v, "code", string("fakeapi_unexpected"));
    add(v, "message", string(message.data));
    add(v, "param", value('z'));
    clear(&message);
    return v;
}
static int refuse(struct peer *p, unsigned index, const char *why)
{
    struct value *v = typed("error");
    char *encoded;
    fail_peer(p, "turn %u: %s", index, why);
    add(v, "status", integer(400));
    add(v, "error", error(why));
    encoded = encode(v, 0);
    websocket_send(p, encoded);
    free(encoded);
    destroy(v);
    return websocket_close(p, 1008);
}
int responses_reply(const struct value *s, struct peer *p)
{
    if (get(s, "refuse")) {
        struct value *v = value('o');
        char why[100];
        int ok;
        int status = (int)number(s, "refuse", 426);
        snprintf(why, sizeof(why), "this step answers the WebSocket with %d", status);
        add(v, "error", error(why));
        ok = json_response(p, status, v);
        destroy(v);
        return ok;
    }
    if (get(s, "turn")) {
        struct value *output = output_items(get(s, "turn"), p),
                     *stream = events(p, p->request->json, output);
        const struct value *e;
        int ok = events_start(p, 200, NULL);
        for (e = stream->child; e && ok; e = e->next)
            ok = event(p, field(e, "type"), NULL, e);
        destroy(output);
        destroy(stream);
        return ok && events_end(p);
    }
    {
        struct value *responses = value('o');
        const struct value *turn = first(get(s, "websocket"));
        unsigned index = 1;
        if (!websocket_start(p)) {
            destroy(responses);
            return 0;
        }
        while (turn) {
            char *message = websocket_receive(p);
            struct value *body, *items, *output, *stream;
            const struct value *e;
            const char *why = NULL;
            int ok = 1, warmup;
            if (!message) {
                fail_peer(p, "the WebSocket closed where the script has turn %u", index);
                destroy(responses);
                return 0;
            }
            body = parse(message);
            free(message);
            if (!body || body->type != 'o' || strcmp(field(body, "type"), "response.create")) {
                destroy(body);
                destroy(responses);
                return refuse(p, index, "a message that is not a response.create");
            }
            items = conversation(responses, body, &why);
            warmup = get(body, "generate") && !flag(body, "generate", 1);
            if (items && !warmup)
                why = check_turn(turn, body, items);
            if (why) {
                int refused = refuse(p, index, why);
                destroy(items);
                destroy(body);
                destroy(responses);
                return refused;
            }
            output = warmup ? value('a') : output_items(turn, p);
            stream = events(p, body, output);
            for (e = stream->child; e && ok; e = e->next) {
                char *encoded = encode(e, 0);
                ok = websocket_send(p, encoded);
                free(encoded);
            }
            for (e = output->child; e; e = e->next)
                add(items, NULL, clone(e));
            set(responses, field(get(stream->child, "response"), "id"), items);
            destroy(output);
            destroy(stream);
            destroy(body);
            if (!ok) {
                destroy(responses);
                return 0;
            }
            if (!warmup) {
                turn = turn->next;
                index++;
            }
        }
        destroy(responses);
        return websocket_close(p, 1000);
    }
}
