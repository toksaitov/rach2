/*
 * casecheck.h: the parts of casecheck, the validator of etude's case
 * manifests, report records, results and runs, that its files share.
 */
#ifndef ETUDE_CASECHECK_H
#define ETUDE_CASECHECK_H

#include <stddef.h>

#include "json.h"

/* A manifest read and checked, and the file it came from. */
struct manifest {
    char *path;
    struct json *doc;
};

/* The manifests of one command, and the schemas their documents follow. */
struct cases {
    struct manifest *m;
    size_t n;
    const char *schemas; /* the directory of the schemas */
    const char *root;    /* the tree `where` refers to, or NULL */
};

/* The outcome of an assertion, and of an attempt, as tools/etude/Readme.md
 * defines them. */
enum outcome { OUTCOME_PASS, OUTCOME_FAIL, OUTCOME_NOT_RUN, OUTCOME_MISSING };
enum attempt { ATTEMPT_PASS, ATTEMPT_FAILURE, ATTEMPT_INCOMPLETE };

void finding(const char *file, int line, const char *fmt, ...);
void say(const char *fmt, ...);
int findings(void);
char *read_file(const char *path, size_t *len);
struct json *read_json(const char *path, int *status);
struct json *read_schema(const char *dir, const char *name);
int schema_findings(const struct json *schema, const struct json *doc, const char *file);

int load_manifests(struct cases *set, char **paths, int n);
void free_manifests(struct cases *set);
const struct json *find_case(const struct cases *set, const char *case_id, const char **file);
void inventory_digest(const struct json *c, char hex[65]);
int has_member(const struct json *array, const char *field, const char *value);
const struct json *group_of(const struct json *c, const char *assertion_id,
                            const struct json **alternative);
int program_reports(const struct json *c);
int print_timing(const struct cases *set);

int judge_report(struct cases *set, char **args, int n);
int judge_results(const struct cases *set, const char *path, const char *claim);
int judge_document(const struct cases *set, const struct json *doc, const char *file, char *why,
                   size_t n);
int judge_verdict(struct cases *set, char **args, int n);

#endif
