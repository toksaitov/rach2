/* server.c: HTTP/1.1, chunked events and WebSocket for scripted API tests. */
#include "api.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <strings.h>
#else
#define strcasecmp _stricmp
#include <io.h>
#include <fcntl.h>
#define read  _read
#define write _write
#endif
#define BODY_MAX (64U * 1024U * 1024U)
static volatile sig_atomic_t stopped;
static void stop(int sig)
{
    (void)sig;
    stopped = 1;
}
/** Keep stop handlers installed through thread joins and result publication. */
static int stop_handlers(void)
{
#ifdef _WIN32
    return signal(SIGTERM, stop) == SIG_ERR || signal(SIGINT, stop) == SIG_ERR ? -1 : 0;
#else
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGTERM);
    sigaddset(&action.sa_mask, SIGINT);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL))
        return -1;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
#endif
}
#ifdef _WIN32
static BOOL WINAPI console_stop(DWORD sig)
{
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT || sig == CTRL_CLOSE_EVENT) {
        stopped = 1;
        return TRUE;
    }
    return FALSE;
}
#endif
void log_line(const char *format, ...)
{
    va_list ap;
    struct buffer b = {0};
    char message[8192];
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    putsb(&b, "fakeapi: ");
    putsb(&b, message);
    putsb(&b, "\n");
    fputs(b.data, stdout);
    fflush(stdout);
    clear(&b);
}
void record(struct server *s, struct value *v)
{
    char *line = encode(v, 0);
    service_lock(&s->lock);
    fprintf(s->record, "%s\n", line);
    fflush(s->record);
    service_unlock(&s->lock);
    free(line);
    destroy(v);
}
void note(struct peer *p, const char *key, struct value *v)
{
    struct value *r = value('o');
    add(r, "n", integer(p->n));
    add(r, key, v);
    record(p->server, r);
}
void fail_peer(struct peer *p, const char *format, ...)
{
    va_list ap;
    char why[2048];
    struct server *s = p->server;
    va_start(ap, format);
    vsnprintf(why, sizeof(why), format, ap);
    va_end(ap);
    p->failed = 1;
    service_lock(&s->lock);
    if (!s->failures++)
        snprintf(s->first, sizeof(s->first), "request %u: %s", p->n, why);
    service_unlock(&s->lock);
    note(p, "failed", string(why));
    log_line("request %u: fail: %s", p->n, why);
}
void finish(struct peer *p)
{
    if (p->finished || p->failed)
        return;
    p->finished = 1;
    service_lock(&p->server->lock);
    if (p->step && !flag(p->server->steps[p->step - 1].script, "repeat", 0))
        p->server->steps[p->step - 1].played = 1;
    service_unlock(&p->server->lock);
}
char *new_id(struct server *s, const char *prefix)
{
    char id[100];
    unsigned n;
    service_lock(&s->lock);
    n = ++s->serial;
    service_unlock(&s->lock);
    snprintf(id, sizeof(id), "%s_%04u", prefix, n);
    return strdup(id);
}
const char *header(const struct request *r, const char *name)
{
    const struct value *p;
    for (p = r->headers ? r->headers->child : NULL; p; p = p->next)
        if (!strcasecmp(text(p->child), name))
            return text(p->child->next);
    return "";
}
int write_peer(struct peer *p, const void *data, size_t n)
{
    const char *bytes = data;
    while (n) {
        int sent = SSL_write(p->tls, bytes, (int)(n > INT_MAX ? INT_MAX : n));
        if (sent <= 0)
            return 0;
        bytes += sent;
        n -= (size_t)sent;
    }
    return 1;
}
static int read_peer(struct peer *p, void *data, size_t n)
{
    char *bytes = data;
    while (n) {
        int got = SSL_read(p->tls, bytes, (int)(n > INT_MAX ? INT_MAX : n));
        if (got <= 0)
            return 0;
        bytes += got;
        n -= (size_t)got;
    }
    return 1;
}
static char *line(struct peer *p)
{
    struct buffer b = {0};
    char c;
    while (read_peer(p, &c, 1)) {
        if (c == '\n') {
            if (b.len && b.data[b.len - 1] == '\r')
                b.data[--b.len] = 0;
            if (!b.data)
                putsb(&b, "");
            return b.data;
        }
        if (b.len >= 65536 || !c)
            break;
        append(&b, &c, 1);
    }
    clear(&b);
    return NULL;
}
static int request_line(struct request *r, const char *s)
{
    char *part;
    r->method = strdup(s);
    part = strchr(r->method, ' ');
    if (!part)
        return 0;
    *part++ = 0;
    r->target = strdup(part);
    part = strchr(r->target, ' ');
    if (!part)
        return 0;
    *part++ = 0;
    r->version = strdup(part);
    if (strncmp(r->version, "HTTP/1.", 7) || strchr(r->version, ' '))
        return 0;
    r->path = strdup(r->target);
    part = strchr(r->path, '?');
    if (part) {
        *part++ = 0;
        r->query = strdup(part);
    } else
        r->query = strdup("");
    return 1;
}
static int request_headers(struct peer *p, struct request *r)
{
    char *s = NULL, *part, *end;
    while ((s = line(p)) && *s) {
        struct value *pair;
        char *name = s;
        part = strchr(s, ':');
        if (!part || part == s)
            goto bad;
        *part++ = 0;
        for (end = name; *end; end++)
            if (isspace((unsigned char)*end))
                goto bad;
        while (*part == ' ' || *part == '\t')
            part++;
        end = part + strlen(part);
        while (end > part && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        pair = value('a');
        add(pair, NULL, string(name));
        add(pair, NULL, string(part));
        add(r->headers, NULL, pair);
        free(s);
        s = NULL;
    }
    if (!s)
        goto bad;
    free(s);
    s = NULL;
    return 1;
bad:
    free(s);
    return 0;
}
static int chunked(const struct request *r)
{
    const char *encoding = header(r, "transfer-encoding");
    char *lower = allocate(strlen(encoding) + 1);
    size_t i;
    int found;
    strcpy(lower, encoding);
    for (i = 0; lower[i]; i++)
        lower[i] = (char)tolower((unsigned char)lower[i]);
    found = strstr(lower, "chunked") != NULL;
    free(lower);
    return found;
}
static int request_chunks(struct peer *p, struct buffer *body)
{
    char *s = NULL, *end;
    unsigned long long length;
    for (;;) {
        char *block;
        char crlf[2];
        s = line(p);
        if (!s || !isxdigit((unsigned char)*s))
            goto bad;
        errno = 0;
        length = strtoull(s, &end, 16);
        if (errno || (*end && *end != ';') || length > BODY_MAX - body->len)
            goto bad;
        free(s);
        s = NULL;
        if (!length) {
            do {
                free(s);
                s = line(p);
            } while (s && *s);
            if (!s)
                goto bad;
            free(s);
            s = NULL;
            break;
        }
        block = allocate((size_t)length);
        if (!read_peer(p, block, (size_t)length)) {
            free(block);
            goto bad;
        }
        append(body, block, (size_t)length);
        free(block);
        if (!read_peer(p, crlf, 2) || memcmp(crlf, "\r\n", 2))
            goto bad;
    }
    return 1;
bad:
    free(s);
    return 0;
}
static int request_length(struct peer *p, struct request *r, struct buffer *body)
{
    char *end;
    unsigned long long length;
    const char *len = header(r, "content-length");
    if (!isdigit((unsigned char)*len))
        return 0;
    errno = 0;
    length = strtoull(len, &end, 10);
    if (*end || errno || length > BODY_MAX)
        return 0;
    body->data = allocate((size_t)length + 1);
    body->len = body->cap = (size_t)length;
    if (!read_peer(p, body->data, body->len))
        return 0;
    return 1;
}
static void free_request(struct request *r);
static struct request *request(struct peer *p, int *malformed)
{
    struct request *r = allocate(sizeof(*r));
    struct buffer body = {0};
    char *s = NULL;
    r->headers = value('a');
    *malformed = 0;
    do {
        free(s);
        s = line(p);
    } while (s && !*s);
    if (!s)
        goto bad;
    if (!request_line(r, s))
        goto invalid;
    free(s);
    s = NULL;
    if (!request_headers(p, r))
        goto invalid;
    p->request = r;
    p->keep = !strcmp(r->version, "HTTP/1.1") && strcasecmp(header(r, "connection"), "close");
    if (!strcasecmp(header(r, "expect"), "100-continue") &&
        !write_peer(p, "HTTP/1.1 100 Continue\r\n\r\n", 25))
        goto bad;
    if (chunked(r)) {
        if (!request_chunks(p, &body))
            goto invalid;
    } else if (*header(r, "content-length") && !request_length(p, r, &body))
        goto invalid;
    if (!body.data)
        putsb(&body, "");
    r->body = body.data;
    r->json = parse(r->body);
    return r;
invalid:
    *malformed = 1;
bad:
    free(s);
    clear(&body);
    free_request(r);
    return NULL;
}

static void free_request(struct request *r)
{
    free(r->method);
    free(r->target);
    free(r->path);
    free(r->query);
    free(r->version);
    free(r->body);
    destroy(r->headers);
    destroy(r->json);
    free(r);
}
static void headers(struct buffer *b, const struct value *given)
{
    const struct value *p;
    for (p = given ? given->child : NULL; p; p = p->next) {
        const char *name = given->type == 'o' ? p->key : text(p->child);
        const char *v = given->type == 'o' ? text(p) : text(p->child->next);
        putsb(b, name);
        putsb(b, ": ");
        putsb(b, v);
        putsb(b, "\r\n");
    }
}
static int has_header(const struct value *given, const char *name)
{
    const struct value *p;
    for (p = given ? given->child : NULL; p; p = p->next)
        if (!strcasecmp(given->type == 'o' ? p->key : text(p->child), name))
            return 1;
    return 0;
}
static const char *phrase(int status)
{
    switch (status) {
    case 101:
        return "Switching Protocols";
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 426:
        return "Upgrade Required";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    default:
        return "Status";
    }
}
int response(struct peer *p, int status, const struct value *given, const char *type,
             const char *body)
{
    char first[160];
    struct buffer b = {0};
    int ok;
    snprintf(first, sizeof(first), "HTTP/1.1 %d %s\r\n", status, phrase(status));
    putsb(&b, first);
    headers(&b, given);
    if (type && !has_header(given, "content-type")) {
        putsb(&b, "Content-Type: ");
        putsb(&b, type);
        putsb(&b, "\r\n");
    }
    snprintf(first, sizeof(first), "Content-Length: %lu\r\n", (unsigned long)strlen(body));
    putsb(&b, first);
    if (!p->keep)
        putsb(&b, "Connection: close\r\n");
    putsb(&b, "\r\n");
    putsb(&b, body);
    ok = write_peer(p, b.data, b.len);
    clear(&b);
    return ok;
}
int json_response(struct peer *p, int status, const struct value *v)
{
    char *body = encode(v, 0);
    int ok = response(p, status, NULL, "application/json", body);
    free(body);
    return ok;
}
int events_start(struct peer *p, int status, const struct value *given)
{
    struct buffer b = {0};
    char first[100];
    int ok;
    snprintf(first, sizeof(first), "HTTP/1.1 %d %s\r\n", status, phrase(status));
    putsb(&b, first);
    headers(&b, given);
    if (!has_header(given, "content-type"))
        putsb(&b, "Content-Type: text/event-stream\r\n");
    putsb(&b, "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n");
    if (!p->keep)
        putsb(&b, "Connection: close\r\n");
    putsb(&b, "\r\n");
    ok = write_peer(p, b.data, b.len);
    clear(&b);
    return ok;
}
int event(struct peer *p, const char *name, const char *id, const struct value *data)
{
    struct buffer b = {0};
    char size[32], *encoded, *at, *next;
    int ok;
    if (name) {
        putsb(&b, "event: ");
        putsb(&b, name);
        putsb(&b, "\n");
    }
    if (id) {
        putsb(&b, "id: ");
        putsb(&b, id);
        putsb(&b, "\n");
    }
    encoded = data && data->type == 's' ? strdup(text(data)) : data ? encode(data, 1) : strdup("");
    for (at = encoded;; at = next + 1) {
        next = strchr(at, '\n');
        putsb(&b, "data: ");
        append(&b, at, next ? (size_t)(next - at) : strlen(at));
        putsb(&b, "\n");
        if (!next)
            break;
    }
    putsb(&b, "\n");
    snprintf(size, sizeof(size), "%lx\r\n", (unsigned long)b.len);
    ok = write_peer(p, size, strlen(size)) && write_peer(p, b.data, b.len) &&
         write_peer(p, "\r\n", 2);
    free(encoded);
    clear(&b);
    return ok;
}
int events_end(struct peer *p)
{
    return write_peer(p, "0\r\n\r\n", 5);
}
static int frame(struct peer *p, unsigned opcode, const void *data, size_t n)
{
    unsigned char head[10];
    unsigned i;
    size_t len = 2;
    head[0] = 0x80 | opcode;
    if (n < 126)
        head[1] = (unsigned char)n;
    else if (n <= 65535) {
        head[1] = 126;
        head[2] = (unsigned char)(n >> 8);
        head[3] = (unsigned char)n;
        len = 4;
    } else {
        head[1] = 127;
        for (i = 0; i < 8; i++)
            head[2 + i] = (unsigned char)((uint64_t)n >> (56 - i * 8));
        len = 10;
    }
    return write_peer(p, head, len) && write_peer(p, data, n);
}
int websocket_start(struct peer *p)
{
    unsigned char digest[SHA_DIGEST_LENGTH], accept[32];
    struct buffer b = {0};
    int ok;
    putsb(&b, header(p->request, "sec-websocket-key"));
    putsb(&b, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    SHA1((const unsigned char *)b.data, b.len, digest);
    clear(&b);
    EVP_EncodeBlock(accept, digest, sizeof(digest));
    putsb(&b, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
              "Upgrade\r\nSec-WebSocket-Accept: ");
    putsb(&b, (const char *)accept);
    putsb(&b, "\r\n\r\n");
    p->keep = 0;
    p->websocket = 1;
    ok = write_peer(p, b.data, b.len);
    clear(&b);
    return ok;
}
struct incoming_frame {
    unsigned char *data;
    uint64_t n;
    unsigned op;
    int final;
};

static int read_frame(struct peer *p, struct incoming_frame *f, size_t used)
{
    unsigned char head[2], mask[4], extra[8], *data;
    uint64_t n;
    unsigned op, i;
    int final;
    if (!read_peer(p, head, 2))
        return 0;
    final = head[0] & 0x80;
    op = head[0] & 15;
    n = head[1] & 127;
    if (n == 126) {
        if (!read_peer(p, extra, 2))
            return 0;
        n = (extra[0] << 8) | extra[1];
    } else if (n == 127) {
        if (!read_peer(p, extra, 8))
            return 0;
        n = 0;
        for (i = 0; i < 8; i++)
            n = (n << 8) | extra[i];
    }
    if (!(head[1] & 128) || (head[0] & 0x70) || n > BODY_MAX || used + n > BODY_MAX ||
        ((op & 8) && (!final || n > 125)))
        return 0;
    if (!read_peer(p, mask, 4))
        return 0;
    data = allocate((size_t)n + 1);
    if (!read_peer(p, data, (size_t)n)) {
        free(data);
        return 0;
    }
    for (i = 0; i < n; i++)
        data[i] ^= mask[i % 4];
    f->data = data;
    f->n = n;
    f->op = op;
    f->final = final;
    return 1;
}
static char *receive_message(struct peer *p, int closing)
{
    struct buffer message = {0};
    unsigned kind = 0;
    for (;;) {
        struct incoming_frame incoming;
        unsigned char *data;
        uint64_t n;
        unsigned op;
        int final;
        if (!read_frame(p, &incoming, message.len))
            goto bad;
        data = incoming.data;
        n = incoming.n;
        op = incoming.op;
        final = incoming.final;
        if (op == 8) {
            note(p, "received_close", n >= 2 ? integer((data[0] << 8) | data[1]) : value('z'));
            if (!closing)
                frame(p, 8, data, (size_t)n);
            free(data);
            goto bad;
        }
        if (op == 9) {
            frame(p, 10, data, (size_t)n);
            free(data);
            continue;
        }
        if (op == 10) {
            free(data);
            continue;
        }
        if (closing) {
            free(data);
            continue;
        }
        if ((op != 0 && op != 1 && op != 2) || (op == 0 && !kind) || (op != 0 && kind)) {
            free(data);
            goto bad;
        }
        if (op)
            kind = op;
        append(&message, data, (size_t)n);
        free(data);
        if (final) {
            if (kind == 1)
                note(p, "received", string(message.data));
            else {
                unsigned char *base64 = allocate(4 * ((message.len + 2) / 3) + 1);
                EVP_EncodeBlock(base64, (unsigned char *)message.data, (int)message.len);
                note(p, "received_binary", string((char *)base64));
                free(base64);
            }
            return message.data;
        }
    }
bad:
    clear(&message);
    return NULL;
}
char *websocket_receive(struct peer *p)
{
    return receive_message(p, 0);
}
int websocket_send(struct peer *p, const char *message)
{
    note(p, "sent", string(message));
    return frame(p, 1, message, strlen(message));
}
int websocket_close(struct peer *p, unsigned code)
{
    unsigned char bytes[2];
    char *ignored;
    finish(p);
    bytes[0] = (unsigned char)(code >> 8);
    bytes[1] = (unsigned char)code;
    note(p, "sent_close", integer(code));
    if (!frame(p, 8, bytes, 2))
        return 1;
    ignored = receive_message(p, 1);
    free(ignored);
    return 1;
}
static int control_request(struct peer *p)
{
    struct server *s = p->server;
    size_t i;
    int holds = 0;
    for (i = 0; i < s->nsteps; i++)
        if (flag(s->steps[i].script, "hold", 0))
            holds = 1;
    if (holds && (!strcmp(p->request->path, "/fakeapi/held") ||
                  !strcmp(p->request->path, "/fakeapi/release"))) {
        char body[80];
        service_lock(&s->lock);
        if (!strcmp(p->request->path, "/fakeapi/release")) {
            s->released = 1;
            service_wake(&s->condition);
            snprintf(body, sizeof(body), "released\n");
        } else {
            while (!s->held && !s->stopping)
                service_wait(&s->condition, &s->lock);
            snprintf(body, sizeof(body), "held %u\n", s->held);
        }
        service_unlock(&s->lock);
        response(p, 200, NULL, "text/plain", body);
        return 1;
    }
    return 0;
}
static const struct value *select_step(struct peer *p, char reason[2048])
{
    struct server *s = p->server;
    const struct value *selected = NULL;
    const char *why = NULL;
    size_t i;
    int ordered = 0;
    service_lock(&s->lock);
    p->n = ++s->requests;
    /* Ordered candidates precede unordered candidates, even earlier ones. */
    for (i = 0; i < s->nsteps; i++) {
        if (s->steps[i].taken || flag(s->steps[i].script, "unordered", 0))
            continue;
        ordered = 1;
        why = protocol_check(s->steps[i].protocol, s->steps[i].script, p->request);
        if (!why) {
            selected = s->steps[i].script;
            p->step = (unsigned)i + 1;
        } else
            snprintf(reason, 2048, "step %lu: %s", (unsigned long)i + 1, why);
        break;
    }
    for (i = 0; !selected && i < s->nsteps; i++) {
        if (s->steps[i].taken || !flag(s->steps[i].script, "unordered", 0))
            continue;
        why = protocol_check(s->steps[i].protocol, s->steps[i].script, p->request);
        if (!why) {
            selected = s->steps[i].script;
            p->step = (unsigned)i + 1;
        } else if (!ordered) {
            snprintf(reason, 2048, "step %lu: %s", (unsigned long)i + 1, why);
            ordered = 1;
        }
    }
    if (selected && !flag(selected, "repeat", 0))
        s->steps[p->step - 1].taken = 1;
    if (!selected && !s->failures++)
        snprintf(s->first, sizeof(s->first), "request %u: %s %s: %s", p->n, p->request->method,
                 p->request->target, reason);
    service_unlock(&s->lock);
    return selected;
}
static void record_request(struct peer *p, const struct value *selected, const char *reason)
{
    struct server *s = p->server;
    struct value *entry;
    entry = value('o');
    add(entry, "n", integer(p->n));
    add(entry, "method", string(p->request->method));
    add(entry, "path", string(p->request->path));
    add(entry, "query", string(p->request->query));
    add(entry, "headers", clone(p->request->headers));
    add(entry, "body", string(p->request->body));
    if (selected)
        add(entry, "step", integer(p->step));
    else
        add(entry, "unexpected", string(reason));
    record(s, entry);
}
static void unexpected_request(struct peer *p, const char *reason)
{
    struct value *body = value('o'), *error = value('o');
    struct buffer message = {0};
    log_line("%u: %s %s: unexpected: %s", p->n, p->request->method, p->request->target, reason);
    putsb(&message, "fakeapi: ");
    putsb(&message, reason);
    add(error, "type", string("unexpected_request"));
    add(error, "message", string(message.data));
    add(body, "error", error);
    json_response(p, 400, body);
    destroy(body);
    clear(&message);
}
static int held_reply(struct peer *p)
{
    struct server *s = p->server;

    service_lock(&s->lock);
    s->held = p->n;
    service_wake(&s->condition);
    while (!s->released && !s->stopping)
        service_wait(&s->condition, &s->lock);
    if (s->stopping && !s->released) {
        service_unlock(&s->lock);
        p->keep = 0;
        return 0;
    }
    service_unlock(&s->lock);
    return 1;
}
static void serve_request(struct peer *p)
{
    struct server *s = p->server;
    const struct value *selected;
    char reason[2048] = "the script has no step left";
    if (control_request(p))
        return;
    selected = select_step(p, reason);
    record_request(p, selected, reason);
    if (!selected) {
        unexpected_request(p, reason);
        return;
    }
    log_line("%u: %s %s: step %u", p->n, p->request->method, p->request->target, p->step);
    if (flag(selected, "hold", 0) && !held_reply(p))
        return;
    if (!protocol_reply(s->steps[p->step - 1].protocol, selected, p)) {
        if (p->websocket && !p->failed && !p->finished)
            fail_peer(p, "the connection ended in the WebSocket exchange");
        p->keep = 0;
    }
    finish(p);
}
struct connection {
    struct server *server;
    service_socket fd;
    service_thread thread;
    struct connection *next;
};
static void connection(void *arg)
{
    struct connection *c = arg;
    struct peer p;
    memset(&p, 0, sizeof(p));
    p.server = c->server;
    p.tls = SSL_new(c->server->tls);
    if (!p.tls) {
        service_close(c->fd);
        c->fd = SERVICE_INVALID;
        return;
    }
    SSL_set_fd(p.tls, (int)c->fd);
    if (SSL_accept(p.tls) != 1) {
        log_line("tls: the handshake failed");
        goto done;
    }
    for (;;) {
        int malformed;
        struct request *r;
        p.n = p.step = 0;
        p.failed = p.finished = p.websocket = 0;
        r = request(&p, &malformed);
        if (!r) {
            if (malformed && !c->server->stopping) {
                service_lock(&c->server->lock);
                if (!c->server->failures++)
                    snprintf(c->server->first, sizeof(c->server->first),
                             "connection: a request that cannot be read");
                service_unlock(&c->server->lock);
                {
                    struct value *entry = value('o');
                    add(entry, "connection", string("client"));
                    add(entry, "failed", string("a request that cannot be read"));
                    record(c->server, entry);
                }
                const char *bad =
                    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                write_peer(&p, bad, strlen(bad));
            }
            break;
        }
        p.request = r;
        serve_request(&p);
        free_request(r);
        p.request = NULL;
        if (!p.keep || c->server->stopping)
            break;
    }
done:
    SSL_free(p.tls);
    service_lock(&c->server->lock);
    service_close(c->fd);
    c->fd = SERVICE_INVALID;
    service_unlock(&c->server->lock);
}
static int verdict(struct server *s)
{
    size_t i, left = 0, owed = 0, first = 0;
    for (i = 0; i < s->nsteps; i++) {
        if (flag(s->steps[i].script, "repeat", 0))
            continue;
        owed++;
        if (!s->steps[i].played) {
            if (!left)
                first = i + 1;
            left++;
        }
    }
    if (s->failures)
        log_line("result: fail: %u failed, the first %s%s", s->failures, s->first,
                 left ? "; steps not played" : "");
    else if (left)
        log_line("result: fail: %lu of %lu steps not played, the first step %lu",
                 (unsigned long)left, (unsigned long)owed, (unsigned long)first);
    else
        log_line("result: pass: %lu steps played by %u requests", (unsigned long)owed, s->requests);
    return s->failures || left ? 1 : 0;
}
/** Publish the service's own result, apart from its supervisor's stop reason. */
static int write_status(const char *record_path, int result)
{
    struct buffer status = {0};
    FILE *file;

    putsb(&status, record_path);
    putsb(&status, ".status");
    file = fopen(status.data, "wb");
    if (file) {
        fprintf(file, "%d\n", result);
        if (fclose(file))
            result = 2;
    } else
        result = 2;
    clear(&status);
    return result;
}
static int prepare_steps(struct server *s, const struct value *steps)
{
    const struct value *step;
    size_t i = 0;
    s->nsteps = count(steps);
    s->steps = allocate(s->nsteps * sizeof(*s->steps));
    for (step = steps->child; step; step = step->next, i++) {
        const char *protocol = field(step, "protocol"), *why;
        if (!*protocol)
            protocol = field(s->script, "protocol");
        if (!*protocol)
            protocol = "plain";
        why = protocol_validate(protocol, step);
        if (why || (flag(step, "repeat", 0) && !flag(step, "unordered", 0))) {
            log_line("result: refused: step %lu: %s", (unsigned long)i + 1,
                     why ? why : "a repeated step is not unordered");
            return 0;
        }
        s->steps[i].protocol = protocol;
        s->steps[i].script = step;
    }
    return 1;
}
static int serve(const char *script, const char *record_path, const char *certdir, unsigned port)
{
    struct server s;
    struct connection *connections = NULL, *c;
    const struct value *steps;
    int result = 2;
    service_socket listener;
    memset(&s, 0, sizeof(s));
    service_mutex_init(&s.lock);
    service_condition_init(&s.condition);
    s.script = read_json(script);
    s.tls = server_context(certdir);
    steps = get(s.script, "steps");
    if (!s.script || !steps || steps->type != 'a' || !s.tls)
        goto done;
    if (!prepare_steps(&s, steps))
        goto done;
    s.record = fopen(record_path, "ab");
    if (!s.record)
        goto done;
    listener = service_bind(port, 0, &port);
    if (listener == SERVICE_INVALID)
        goto done;
    log_line("listening on 127.0.0.1:%u", port);
    result = 0;
    while (!stopped && service_parent_alive()) {
        service_socket fd;
        if (!service_ready(listener, 200))
            continue;
        fd = accept(listener, NULL, NULL);
        if (fd == SERVICE_INVALID)
            continue;
        c = allocate(sizeof(*c));
        c->server = &s;
        c->fd = fd;
        if (service_thread_start(&c->thread, connection, c)) {
            service_close(fd);
            free(c);
            result = 1;
            break;
        }
        c->next = connections;
        connections = c;
    }
    service_close(listener);
    service_lock(&s.lock);
    s.stopping = 1;
    service_wake(&s.condition);
    for (c = connections; c; c = c->next)
        if (c->fd != SERVICE_INVALID)
            service_shutdown(c->fd);
    service_unlock(&s.lock);
    while (connections) {
        c = connections;
        service_thread_join(c->thread);
        connections = c->next;
        free(c);
    }
    result |= verdict(&s);
done:
    if (result == 2)
        log_line("result: refused: the script, certificate or listening socket is unavailable");
    result = write_status(record_path, result);
    if (s.record)
        fclose(s.record);
    free(s.steps);
    destroy(s.script);
    SSL_CTX_free(s.tls);
    return result;
}
struct relay_job {
    service_socket socket;
};
static void relay_out(void *arg)
{
    struct relay_job *job = arg;
    char block[65536];
    int n;
    while ((n = read(0, block, sizeof(block))) > 0)
        if (service_write(job->socket, block, (size_t)n))
            break;
#ifdef _WIN32
    shutdown(job->socket, SD_SEND);
#else
    shutdown(job->socket, SHUT_WR);
#endif
}
int relay(unsigned port)
{
    struct sockaddr_in address;
    struct relay_job job;
    service_thread outward;
    char block[65536];
    int n;
    job.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (job.socket == SERVICE_INVALID)
        return 1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(UINT32_C(0x7f000001));
    address.sin_port = htons((unsigned short)port);
    if (connect(job.socket, (struct sockaddr *)&address, sizeof(address)) ||
        service_thread_start(&outward, relay_out, &job)) {
        service_close(job.socket);
        return 1;
    }
    while ((n = service_read(job.socket, block, sizeof(block))) > 0) {
        int at = 0;
        while (at < n) {
            int sent = write(1, block + at, n - at);
            if (sent <= 0)
                goto done;
            at += sent;
        }
    }
done:
    service_shutdown(job.socket);
    /* Closing stdin wakes the only other thread. The process owns it. */
#ifdef _WIN32
    CancelSynchronousIo(outward);
#else
    pthread_cancel(outward);
#endif
    service_thread_join(outward);
    service_close(job.socket);
    return 0;
}
int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (stop_handlers())
        return 2;
#ifdef _WIN32
    SetConsoleCtrlHandler(console_stop, TRUE);
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
#endif
    if (service_init())
        return 2;
    if (strstr(name, "agent_relay") && argc == 2)
        return relay(service_port(argv[1]));
    if (argc == 6 && !strcmp(argv[1], "accept-prepare"))
        return prepare_acceptance(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 4 && !strcmp(argv[1], "worker-proof"))
        return verify_worker_archive(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "unpack"))
        return unpack_assets(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "remove"))
        return remove_assets(argv[2]);
    if (argc == 5 && !strcmp(argv[1], "render"))
        return render_script(argv[2], argv[3], argv[4]);
    if (argc == 6 && !strcmp(argv[1], "verify"))
        return verify_record(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 4 && !strcmp(argv[1], "ca"))
        return make_ca(argv[2], argc - 3, argv + 3);
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "serve"))
        return serve(argv[2], argv[3], argv[4], argc == 6 ? service_port(argv[5]) : 0);
    if (argc == 3 && !strcmp(argv[1], "relay"))
        return relay(service_port(argv[2]));
    fputs("usage: fakeapi ca DIR NAME... | serve SCRIPT RECORD CERTDIR [PORT] | relay PORT\n",
          stderr);
    return 2;
}
