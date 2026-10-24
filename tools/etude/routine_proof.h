/* Create-once proof files from the actual native owner and its executor. */
#ifndef ETUDE_ROUTINE_PROOF_H
#define ETUDE_ROUTINE_PROOF_H
#include "json.h"
#include "include/etude/history.h"
void routine_quote(struct jbuf *buffer, const char *text);
void routine_reading(struct jbuf *buffer, const struct etude_reading *reading);
int routine_ref(struct jbuf *buffer, const char *path);
int routine_create(const char *path, struct jbuf *buffer);
int routine_json(const char *path, struct json **document);
int routine_copy(const char *from, const char *to);
#endif
