/* api.h: request checks and replies shared by the scripted protocols. */
#ifndef FAKEAPI_API_H
#define FAKEAPI_API_H
#include "json.h"
#include "platform.h"
#include <openssl/ssl.h>
#include <stdio.h>
struct request {
    char *method, *target, *path, *query, *version, *body;
    struct value *headers, *json;
};
struct step {
    const struct value *script;
    const char *protocol;
    unsigned taken, played;
};
struct server {
    struct value *script;
    struct step *steps;
    size_t nsteps;
    unsigned requests, failures, serial, held;
    int released, stopping;
    char first[4096];
    FILE *record;
    service_mutex lock;
    service_condition condition;
    SSL_CTX *tls;
};
struct peer {
    struct server *server;
    struct request *request;
    SSL *tls;
    unsigned n, step;
    int keep, websocket, finished, failed;
};
const char *header(const struct request *r, const char *name);
void log_line(const char *format, ...);
void record(struct server *s, struct value *v);
void note(struct peer *p, const char *key, struct value *v);
void fail_peer(struct peer *p, const char *format, ...);
void finish(struct peer *p);
char *new_id(struct server *s, const char *prefix);
int write_peer(struct peer *p, const void *data, size_t n);
int response(struct peer *p, int status, const struct value *headers, const char *type,
             const char *body);
int json_response(struct peer *p, int status, const struct value *body);
int events_start(struct peer *p, int status, const struct value *headers);
int event(struct peer *p, const char *name, const char *id, const struct value *data);
int events_end(struct peer *p);
int websocket_start(struct peer *p);
char *websocket_receive(struct peer *p);
int websocket_send(struct peer *p, const char *message);
int websocket_close(struct peer *p, unsigned code);
const char *protocol_validate(const char *name, const struct value *step);
const char *protocol_check(const char *name, const struct value *step, const struct request *r);
int protocol_reply(const char *name, const struct value *step, struct peer *p);
struct value *read_json_lines(const char *path);
int verify_worker_archive(const char *archive, const char *directory);
int prepare_acceptance(const char *agent_source, const char *gh_source, const char *directory,
                       const char *agent);
int unpack_assets(const char *archive, const char *destination);
int remove_assets(const char *path);
int render_script(const char *source, const char *destination, const char *row);
int verify_record(const char *script, const char *record_path, const char *row,
                  const char *version);
int make_ca(const char *directory, int count, char **names);
SSL_CTX *server_context(const char *directory);
int relay(unsigned port);
#endif
