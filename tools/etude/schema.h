/*
 * schema.h: checks a document against one of the JSON Schemas of etude's
 * contracts, the files of tools/etude/schemas whose names end in .schema.json.
 */
#ifndef ETUDE_SCHEMA_H
#define ETUDE_SCHEMA_H

#include "json.h"

/* Called once for each fault found: the line of the value, its place in the
 * document as a JSON pointer, and what is wrong with it. */
typedef void (*schema_report)(void *ctx, int line, const char *path, const char *what);

int schema_check(const struct json *schema, const struct json *doc, schema_report report,
                 void *ctx);
int schema_pattern(const char *pattern, const char *text);

#endif
