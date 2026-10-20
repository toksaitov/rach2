/*
 * etude_ledger.c: etude-ledger, which lists the claims of a machine's ledger
 * as recovery judges them, and drops a claim whose processes a person has
 * found ended.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "etude/ledger.h"

/* The words of enum etude_claim_status. */
static const char *const STATUS[] = {"owned", "live", "uncertain"};

/*
 * The claim that drop looks for in a listing, and once found, its fields as
 * struct etude_ledger_claim gives them.
 */
struct lookup {
    const char *id;
    int found;
    enum etude_claim_status status;
    long owner;
    long running;
    long group;
};

/** Print the usage and return its status. */
static int usage(void)
{
    fprintf(stderr, "etude-ledger: usage: etude-ledger list LEDGER\n"
                    "etude-ledger: usage: etude-ledger drop LEDGER ID\n");
    return 2;
}

/**
 * Say whether `dir` is a ledger: a directory with the `verdicts` directory
 * that etude_ledger_open and deployment make. A path that is not one is
 * refused, so that a mistyped path does not become a new, empty ledger.
 */
static int is_ledger(const char *dir)
{
    struct stat st;
    char *path = malloc(strlen(dir) + sizeof("/verdicts"));
    int ok;

    if (path == NULL)
        return 0;
    strcpy(path, dir);
    strcat(path, "/verdicts");
    ok = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    free(path);
    return ok;
}

/** Report the error in errno of the ledger `dir`, and return status 2. */
static int failed(const char *dir)
{
    if (errno == EBADMSG)
        fprintf(stderr, "etude-ledger: %s: its claims file does not parse; nothing was changed\n",
                dir);
    else
        fprintf(stderr, "etude-ledger: %s: %s\n", dir, strerror(errno));
    return 2;
}

/** Say whether claim `c` is a request that waits for admission. */
static int is_request(const struct etude_ledger_claim *c)
{
    return strcmp(c->state, "waiting") == 0 || strcmp(c->state, "blocked") == 0;
}

/** Print claim `c` on one line, and count its status in the array `ctx`. */
static int show(const struct etude_ledger_claim *c, void *ctx)
{
    size_t *count = ctx, i;

    printf("%s %s: %s", is_request(c) ? "request" : "claim", c->id, STATUS[c->status]);
    if (c->why[0] != '\0')
        printf(": %s", c->why);
    printf("; state %s, system %s, owner %ld", c->state, c->system, c->owner.pid);
    if (c->account[0] != '\0')
        printf(", account %s", c->account);
    if (!is_request(c))
        printf(", processes %lu", (unsigned long)c->nproc);
    printf("; %s", is_request(c) ? "asks" : "holds");
    for (i = 0; i < c->nres; i++)
        printf("%s %s %llu", i > 0 ? "," : "", c->res[i].name,
               (unsigned long long)c->res[i].amount);
    printf("%s\n", c->nres == 0 ? " nothing" : "");
    count[c->status]++;
    return 0;
}

/** List the claims and requests of ledger `l` in `dir`. */
static int list(struct etude_ledger *l, const char *dir)
{
    size_t count[3] = {0, 0, 0};

    if (etude_ledger_list(l, show, count) != 0)
        return failed(dir);
    printf("etude-ledger: result: owned %lu, live %lu, uncertain %lu\n", (unsigned long)count[0],
           (unsigned long)count[1], (unsigned long)count[2]);
    return 0;
}

/** Note the claim the lookup `ctx` names as the listing judges it, and stop. */
static int find(const struct etude_ledger_claim *c, void *ctx)
{
    struct lookup *k = ctx;

    if (strcmp(c->id, k->id) != 0)
        return 0;
    k->found = 1;
    k->status = c->status;
    k->owner = c->owner.pid;
    k->running = c->running;
    k->group = c->group;
    return 1;
}

/** Refuse claim `id`, live as the lookup `k` found it, naming what runs. */
static int refuse_live(const char *id, const struct lookup *k)
{
    if (k->running != 0)
        fprintf(stderr, "etude-ledger: result: refused: %s: its process %ld runs\n", id,
                k->running);
    else if (k->group != 0)
        fprintf(stderr, "etude-ledger: result: refused: %s: its process group %ld has a member\n",
                id, k->group);
    else
        fprintf(stderr, "etude-ledger: result: refused: %s: a process of it runs\n", id);
    return 1;
}

/**
 * Drop claim `id` of ledger `l` in `dir`, unless its owner or a process of it
 * runs.
 *
 * The listing judges the claim before the drop, under a lock of its own. A
 * claim that is not owned has no worker left to register a process, so none
 * of its processes can start in between.
 *
 * @return 0 once it is dropped, 1 when it is refused, or 2 for an error.
 */
static int drop(struct etude_ledger *l, const char *dir, const char *id)
{
    struct lookup k;

    memset(&k, 0, sizeof(k));
    k.id = id;
    if (etude_ledger_list(l, find, &k) < 0)
        return failed(dir);
    if (k.found && k.status == ETUDE_CLAIM_LIVE)
        return refuse_live(id, &k);
    if (etude_ledger_drop(l, id) == 0) {
        printf("etude-ledger: result: dropped: %s\n", id);
        return 0;
    }
    if (errno == ENOENT) {
        fprintf(stderr, "etude-ledger: result: refused: %s: the ledger has no such claim\n", id);
        return 1;
    }
    if (errno != EBUSY)
        return failed(dir);
    if (k.found)
        fprintf(stderr, "etude-ledger: result: refused: %s: its owner, process %ld, runs\n", id,
                k.owner);
    else
        fprintf(stderr, "etude-ledger: result: refused: %s: its owner runs\n", id);
    return 1;
}

/** List the ledger's claims, or drop one, as the first argument says. */
int main(int argc, char **argv)
{
    struct etude_ledger *l;
    int rc;

    if (!(argc == 3 && strcmp(argv[1], "list") == 0) &&
        !(argc == 4 && strcmp(argv[1], "drop") == 0))
        return usage();
    if (!is_ledger(argv[2])) {
        fprintf(stderr, "etude-ledger: %s: not a ledger: it has no verdicts directory\n", argv[2]);
        return 2;
    }
    l = etude_ledger_open(argv[2]);
    if (l == NULL)
        return failed(argv[2]);
    rc = argc == 3 ? list(l, argv[2]) : drop(l, argv[2], argv[3]);
    etude_ledger_close(l);
    return rc;
}
