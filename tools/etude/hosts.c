/*
 * hosts.c: the host system etude runs on, and the test of a case's
 * `requirements.hosts` that every choice of the cases a run makes shares.
 */
#include "hosts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A case this process has said it leaves out, so that it says so once. */
struct left_out {
    struct left_out *next;
    char *case_id;
};

static struct left_out *said;

/** Give the host system etude runs on: `linux`, `macos` or `windows`. */
const char *etude_host_system(void)
{
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

/**
 * Say whether case `c` runs on the host system `host`: 1 when its
 * `requirements.hosts` lists `host` or the case has no `hosts`, and 0
 * otherwise, a NULL `host` among them.
 */
int etude_case_hosted(const struct json *c, const char *host)
{
    const struct json *hosts = json_member(json_member(c, "requirements"), "hosts");
    size_t i;

    if (hosts == NULL)
        return 1;
    for (i = 0; host != NULL && hosts->type == JSON_ARRAY && i < hosts->count; i++)
        if (hosts->item[i]->type == JSON_STRING && strcmp(hosts->item[i]->string, host) == 0)
            return 1;
    return 0;
}

/** Say once on stderr that case `case_id` does not run on this host. */
static void say_left_out(const char *case_id)
{
    struct left_out *l;

    for (l = said; l != NULL; l = l->next)
        if (strcmp(l->case_id, case_id) == 0)
            return;
    fprintf(stderr, "etude: case %s: not run on %s\n", case_id, etude_host_system());
    if ((l = malloc(sizeof(*l))) == NULL)
        return;
    if ((l->case_id = malloc(strlen(case_id) + 1)) == NULL) {
        free(l);
        return;
    }
    strcpy(l->case_id, case_id);
    l->next = said;
    said = l;
}

/**
 * Say whether case `c` runs on the host system etude runs on, as
 * `etude_case_hosted` does. A run calls it only for a case it would
 * otherwise run, since the first call that leaves a case out says so on
 * stderr, as `etude: case ID: not run on HOST`.
 */
int etude_case_runs_here(const struct json *c)
{
    const char *case_id = json_text(c, "case_id");

    if (etude_case_hosted(c, etude_host_system()))
        return 1;
    say_left_out(case_id != NULL ? case_id : "?");
    return 0;
}
