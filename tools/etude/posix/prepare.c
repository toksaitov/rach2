/*
 * prepare.c: etude's preparation graph on POSIX systems. Each node's recipe
 * runs in a work directory of its own through the executor, and its product
 * is published in the store only after the recipe and its cleanup succeed.
 */
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "etude/artifact.h"
#include "posix.h"
#include "sha256.h"
#include "store.h"

extern char **environ;

/* How often a node waits to retry the lock another process holds, in ms. */
#define LOCK_RETRY_MS 20

/* Where a node is in a run. */
enum phase {
    PH_WAIT,     /* its inputs are not all done, or no job was free */
    PH_LOCKWAIT, /* another process prepares its key */
    PH_RUNNING,  /* its recipe runs */
    PH_DONE      /* its state is decided */
};

struct pnode {
    char *name;
    char **argv; /* NULL for a file of the tree */
    char **envp; /* NULL for the controller's */
    char *output;
    char **inputs;
    size_t ninputs;
    char **key;
    int fresh;
    uint64_t bound_us;

    size_t *in; /* the inputs' indices, found by a run */
    enum phase phase;
    int lock;          /* the key's lock, or -1 */
    uint64_t since_us; /* when the lock wait or the recipe began */
    int cancelled;     /* the recipe was cancelled at its bound */
    struct etude_proc *proc;
    char *why;
    char *work;
    char *err;
    struct etude_prep_result r;
};

struct etude_prep {
    struct pnode *n;
    size_t len, cap;
};

/* One run of a graph, as each pass over its nodes finds it. */
struct run {
    struct etude_prep *g;
    struct etude_store *s;
    struct etude_exec *ex;
    const struct etude_prep_opts *o;
    unsigned running; /* recipes that run */
    size_t left;      /* nodes not yet decided */
    int waiting;      /* a node waits for a lock, or has one and waits for a job */
    uint64_t now;
    uint64_t soonest; /* the earliest bound of a recipe that runs, or 0 */
};

/* ---- Strings and lists. ---- */

/** Free a list ended by NULL. */
static void list_free(char **v)
{
    size_t i;

    if (v == NULL)
        return;
    for (i = 0; v[i] != NULL; i++)
        free(v[i]);
    free(v);
}

/** Copy a list ended by NULL, giving its length in `*len`; NULL stays NULL. */
static int list_copy(char *const *from, char ***to, size_t *len)
{
    size_t n = 0, i;

    *to = NULL;
    if (len != NULL)
        *len = 0;
    if (from == NULL)
        return 0;
    while (from[n] != NULL)
        n++;
    *to = calloc(n + 1, sizeof(**to));
    if (*to == NULL)
        return -1;
    for (i = 0; i < n; i++) {
        (*to)[i] = strdup(from[i]);
        if ((*to)[i] == NULL) {
            list_free(*to);
            *to = NULL;
            return -1;
        }
    }
    if (len != NULL)
        *len = n;
    return 0;
}

/** Give a string from malloc made as printf makes it, or NULL. */
static char *format(const char *fmt, ...)
{
    va_list ap;
    char *s;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return NULL;
    s = malloc((size_t)n + 1);
    if (s == NULL)
        return NULL;
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

/** Say whether `s` is a node's name: printable ASCII without a space. */
static int is_name(const char *s)
{
    size_t n;

    for (n = 0; s[n] != '\0'; n++)
        if (s[n] <= ' ' || s[n] > '~')
            return 0;
    return n > 0;
}

/* ---- The graph. ---- */

struct etude_prep *etude_prep_new(void)
{
    return calloc(1, sizeof(struct etude_prep));
}

/** Forget a node's result, keeping its definition. */
static void result_clear(struct pnode *p)
{
    free(p->why);
    free(p->work);
    free(p->err);
    free(p->in);
    p->why = p->work = p->err = NULL;
    p->in = NULL;
    memset(&p->r, 0, sizeof(p->r));
    p->r.name = p->name;
    p->r.why = "";
    p->r.state = ETUDE_PREP_PENDING;
    p->phase = PH_WAIT;
    p->lock = -1;
    p->cancelled = 0;
    p->proc = NULL;
}

void etude_prep_free(struct etude_prep *g)
{
    size_t i;

    if (g == NULL)
        return;
    for (i = 0; i < g->len; i++) {
        result_clear(&g->n[i]);
        free(g->n[i].name);
        list_free(g->n[i].argv);
        list_free(g->n[i].envp);
        free(g->n[i].output);
        list_free(g->n[i].inputs);
        list_free(g->n[i].key);
    }
    free(g->n);
    free(g);
}

/** Give the index of node `name`, or -1. */
static long find(const struct etude_prep *g, const char *name)
{
    size_t i;

    for (i = 0; i < g->len; i++)
        if (strcmp(g->n[i].name, name) == 0)
            return (long)i;
    return -1;
}

int etude_prep_add(struct etude_prep *g, const struct etude_prep_node *n)
{
    struct pnode *p, *grown;

    if (n->name == NULL || n->output == NULL || !is_name(n->name) || n->output[0] == '\0' ||
        (n->argv != NULL && n->argv[0] == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (find(g, n->name) >= 0) {
        errno = EEXIST;
        return -1;
    }
    if (g->len == g->cap) {
        grown = realloc(g->n, (g->cap ? 2 * g->cap : 8) * sizeof(*grown));
        if (grown == NULL) {
            errno = ENOMEM;
            return -1;
        }
        g->n = grown;
        g->cap = g->cap ? 2 * g->cap : 8;
    }
    p = &g->n[g->len];
    memset(p, 0, sizeof(*p));
    p->fresh = n->fresh;
    p->bound_us = n->bound_us;
    p->name = strdup(n->name);
    p->output = strdup(n->output);
    if (p->name == NULL || p->output == NULL || list_copy(n->argv, &p->argv, NULL) != 0 ||
        list_copy(n->envp, &p->envp, NULL) != 0 ||
        list_copy((char *const *)n->inputs, &p->inputs, &p->ninputs) != 0 ||
        list_copy((char *const *)n->key, &p->key, NULL) != 0) {
        free(p->name);
        free(p->output);
        list_free(p->argv);
        list_free(p->envp);
        list_free(p->inputs);
        list_free(p->key);
        errno = ENOMEM;
        return -1;
    }
    result_clear(p);
    g->len++;
    return 0;
}

const struct etude_prep_result *etude_prep_result(const struct etude_prep *g, const char *name)
{
    long i = find(g, name);

    return i < 0 ? NULL : &g->n[i].r;
}

/* ---- Order. ---- */

/** Give the index among `p`'s inputs of the `n` bytes at `name`, or -1. */
static long input_index(const struct pnode *p, const char *name, size_t n)
{
    size_t i;

    for (i = 0; i < p->ninputs; i++)
        if (strlen(p->inputs[i]) == n && strncmp(p->inputs[i], name, n) == 0)
            return (long)i;
    return -1;
}

/**
 * Say whether `${IN:NAME}` in `s` names an input of `p` each time it
 * appears.
 */
static int inputs_named(const struct pnode *p, const char *s)
{
    const char *at, *end;

    while ((at = strstr(s, "${IN:")) != NULL) {
        at += 5;
        end = strchr(at, '}');
        if (end == NULL || input_index(p, at, (size_t)(end - at)) < 0)
            return 0;
        s = end + 1;
    }
    return 1;
}

/**
 * Find each node's inputs, and refuse a missing one or a cycle, which a
 * depth-first walk finds: its mark is 1 for a node on the walk's path and 2
 * for one whose inputs have all been walked.
 */
static int resolve(struct etude_prep *g)
{
    unsigned char *mark;
    size_t *stack, *at;
    size_t i, j, top;
    long k;

    for (i = 0; i < g->len; i++) {
        struct pnode *p = &g->n[i];

        p->in = calloc(p->ninputs + 1, sizeof(*p->in));
        if (p->in == NULL) {
            errno = ENOMEM;
            return -1;
        }
        for (j = 0; j < p->ninputs; j++) {
            k = find(g, p->inputs[j]);
            if (k < 0) {
                errno = EINVAL;
                return -1;
            }
            p->in[j] = (size_t)k;
        }
        for (j = 0; p->argv != NULL && p->argv[j] != NULL; j++)
            if (!inputs_named(p, p->argv[j])) {
                errno = EINVAL;
                return -1;
            }
        for (j = 0; p->envp != NULL && p->envp[j] != NULL; j++)
            if (!inputs_named(p, p->envp[j])) {
                errno = EINVAL;
                return -1;
            }
    }
    mark = calloc(g->len + 1, 1);
    stack = calloc(g->len + 1, sizeof(*stack));
    at = calloc(g->len + 1, sizeof(*at));
    if (mark == NULL || stack == NULL || at == NULL) {
        free(mark);
        free(stack);
        free(at);
        errno = ENOMEM;
        return -1;
    }
    for (i = 0; i < g->len; i++) {
        if (mark[i] != 0)
            continue;
        top = 0;
        stack[top] = i;
        at[top] = 0;
        mark[i] = 1;
        for (;;) {
            struct pnode *p = &g->n[stack[top]];

            if (at[top] == p->ninputs) {
                mark[stack[top]] = 2;
                if (top == 0)
                    break;
                top--;
                continue;
            }
            j = p->in[at[top]++];
            if (mark[j] == 1) {
                free(mark);
                free(stack);
                free(at);
                errno = ELOOP;
                return -1;
            }
            if (mark[j] == 0) {
                mark[j] = 1;
                stack[++top] = j;
                at[top] = 0;
            }
        }
    }
    free(mark);
    free(stack);
    free(at);
    return 0;
}

/* ---- Keys and recipes. ---- */

/** Hash a field of a key as `tag LENGTH:TEXT` and a newline. */
static void key_field(struct sha256 *h, const char *tag, const char *text)
{
    char head[64];
    int n = snprintf(head, sizeof(head), "%s %zu:", tag, strlen(text));

    sha256_update(h, (const unsigned char *)head, (size_t)n);
    sha256_update(h, (const unsigned char *)text, strlen(text));
    sha256_update(h, (const unsigned char *)"\n", 1);
}

/** Compute node `p`'s key from its definition and its inputs' digests. */
static void key_of(const struct etude_prep *g, struct pnode *p)
{
    unsigned char sum[32];
    struct sha256 h;
    size_t i;

    sha256_init(&h);
    key_field(&h, "etude-node", "1");
    key_field(&h, "name", p->name);
    for (i = 0; p->argv[i] != NULL; i++)
        key_field(&h, "arg", p->argv[i]);
    key_field(&h, "output", p->output);
    for (i = 0; p->key != NULL && p->key[i] != NULL; i++)
        key_field(&h, "key", p->key[i]);
    for (i = 0; i < p->ninputs; i++) {
        key_field(&h, "input", p->inputs[i]);
        key_field(&h, "digest", g->n[p->in[i]].r.digest);
    }
    sha256_final(&h, sum);
    sha256_hex(sum, p->r.key);
}

/**
 * Give `s` with `${WORK}` and each `${IN:NAME}` replaced, from malloc, or
 * NULL.
 */
static char *expand(const struct etude_prep *g, const struct etude_store *s, const struct pnode *p,
                    const char *text)
{
    char obj[4096];
    const char *with, *end;
    size_t n, skip, len = 0, cap = strlen(text) + 1;
    char *out = malloc(cap), *grown;
    long k;

    if (out == NULL)
        return NULL;
    while (*text != '\0') {
        with = text;
        n = skip = 1;
        if (strncmp(text, "${WORK}", 7) == 0) {
            with = p->work;
            n = strlen(with);
            skip = 7;
        } else if (strncmp(text, "${IN:", 5) == 0 && (end = strchr(text, '}')) != NULL &&
                   (k = input_index(p, text + 5, (size_t)(end - text - 5))) >= 0 &&
                   etude_store_path(s, g->n[p->in[k]].r.digest, obj, sizeof(obj)) == 0) {
            with = obj;
            n = strlen(obj);
            skip = (size_t)(end - text) + 1;
        }
        if (len + n + 1 > cap) {
            cap = 2 * (len + n + 1);
            grown = realloc(out, cap);
            if (grown == NULL) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        memcpy(out + len, with, n);
        len += n;
        text += skip;
    }
    out[len] = '\0';
    return out;
}

/** Expand each string of list `v`; NULL when memory ran out. */
static char **expand_list(const struct etude_prep *g, const struct etude_store *s,
                          const struct pnode *p, char *const *v)
{
    size_t n = 0, i;
    char **out;

    while (v[n] != NULL)
        n++;
    out = calloc(n + 1, sizeof(*out));
    if (out == NULL)
        return NULL;
    for (i = 0; i < n; i++) {
        out[i] = expand(g, s, p, v[i]);
        if (out[i] == NULL) {
            list_free(out);
            return NULL;
        }
    }
    return out;
}

/** Remove the tree at `path`, not following links; 0, or -1. */
static int remove_tree(const char *path)
{
    struct dirent *d;
    struct stat st;
    char *sub;
    DIR *dir;
    int r = 0;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    dir = opendir(path);
    if (dir == NULL)
        return -1;
    while ((d = readdir(dir)) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;
        sub = store_join(path, d->d_name);
        if (sub == NULL || remove_tree(sub) != 0)
            r = -1;
        free(sub);
    }
    closedir(dir);
    if (rmdir(path) != 0)
        r = -1;
    return r;
}

/* ---- A run. ---- */

/** Decide node `p`'s state, with the reason of a failure. */
static void decide(struct pnode *p, enum etude_prep_state state, char *why)
{
    p->phase = PH_DONE;
    p->r.state = state;
    free(p->why);
    p->why = why;
    p->r.why = why != NULL ? why : "";
    p->r.ended_us = posix_now_us();
    if (p->lock >= 0) {
        close(p->lock);
        p->lock = -1;
    }
}

/** Fail node `p` with a reason that printf makes, and errno's text. */
static void fail(struct pnode *p, const char *what, int err)
{
    char *why = err != 0 ? format("%s: %s", what, strerror(err)) : strdup(what);

    decide(p, ETUDE_PREP_FAILED, why);
}

/** Publish a file of the tree as node `p`'s product. */
static void put_file(struct etude_store *s, struct pnode *p)
{
    if (etude_store_put(s, p->output, NULL, p->r.digest, &p->r.size) != 0) {
        fail(p, "the file was not published", errno);
        return;
    }
    decide(p, ETUDE_PREP_BUILT, NULL);
}

/** Start node `p`'s recipe in a new work directory; 0, or -1 once failed. */
static int launch(const struct run *r, struct pnode *p)
{
    const struct etude_prep_opts *o = r->o;
    struct etude_exec_spec spec;
    char **argv = NULL, **envp = NULL;
    char *attempt = NULL;
    int e;

    p->work = store_join(o->scratch, "etude-prep.XXXXXX");
    if (p->work == NULL || mkdtemp(p->work) == NULL) {
        e = p->work == NULL ? ENOMEM : errno;
        free(p->work);
        p->work = NULL;
        fail(p, "no work directory was made", e);
        return -1;
    }
    argv = expand_list(r->g, r->s, p, p->argv);
    envp = expand_list(r->g, r->s, p, p->envp != NULL ? p->envp : environ);
    attempt =
        format("%s/prep.%zu", o->attempt != NULL ? o->attempt : "prep", (size_t)(p - r->g->n));
    if (argv == NULL || envp == NULL || attempt == NULL) {
        e = ENOMEM;
        goto fail;
    }
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = attempt;
    spec.argv = argv;
    spec.envp = envp;
    spec.cwd = p->work;
    spec.output_max = o->output_max;
    spec.grace_us = o->grace_us;
    spec.reg = o->reg;
    spec.child = o->child;
    spec.reg_ctx = o->reg_ctx;
    p->since_us = posix_now_us();
    if (etude_exec_start(r->ex, &spec, &p->proc) != 0) {
        e = errno;
        goto fail;
    }
    list_free(argv);
    list_free(envp);
    free(attempt);
    p->r.ran = 1;
    p->phase = PH_RUNNING;
    return 0;
fail:
    list_free(argv);
    list_free(envp);
    free(attempt);
    p->r.work = p->work;
    fail(p, "the recipe did not start", e);
    return -1;
}

/**
 * Say how recipe `r` ended, or give NULL for an exit of 0 with its cleanup
 * confirmed.
 */
static char *recipe_failure(const struct pnode *p, const struct etude_exec_result *r)
{
    if (p->cancelled)
        return format("the recipe ran past its bound of %llu us", (unsigned long long)p->bound_us);
    if (r->end == ETUDE_END_EXITED && r->value != 0)
        return format("the recipe exited %d", r->value);
    if (r->end == ETUDE_END_SIGNALED)
        return format("the recipe was killed by signal %d", r->value);
    if (r->end != ETUDE_END_EXITED)
        return strdup("the recipe's end was not seen");
    if (r->cleanup != ETUDE_CLEANUP_CONFIRMED)
        return strdup("the recipe's processes were not all seen to end");
    return NULL;
}

/** Publish the product of node `p`, whose recipe has finished. */
static void finish(struct etude_store *s, struct pnode *p)
{
    const struct etude_exec_result *r = etude_proc_result(p->proc);
    struct stat st;
    char *why, *out;

    p->r.end = r->end;
    p->r.value = r->value;
    p->err = malloc(r->err.kept + 1);
    if (p->err != NULL) {
        memcpy(p->err, r->err.data != NULL ? r->err.data : "", r->err.kept);
        p->err[r->err.kept] = '\0';
        p->r.err = r->err;
        p->r.err.data = p->err;
    }
    why = recipe_failure(p, r);
    etude_proc_free(p->proc);
    p->proc = NULL;
    p->r.work = p->work;
    if (why != NULL) {
        decide(p, ETUDE_PREP_FAILED, why);
        return;
    }
    out = p->output[0] == '/' ? strdup(p->output) : store_join(p->work, p->output);
    if (out == NULL) {
        fail(p, "the product was not published", ENOMEM);
        return;
    }
    if (lstat(out, &st) != 0) {
        free(out);
        fail(p, "the recipe made no product", errno);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        free(out);
        fail(p, "the product is not a regular file", 0);
        return;
    }
    if (etude_store_put(s, out, NULL, p->r.digest, &p->r.size) != 0) {
        free(out);
        fail(p, "the product was not published", errno);
        return;
    }
    free(out);
    if (!p->fresh && store_record_write(s, p->r.key, p->name, p->r.digest, p->r.size) != 0) {
        fail(p, "the product's record was not written", errno);
        return;
    }
    if (remove_tree(p->work) == 0) {
        free(p->work);
        p->work = NULL;
    }
    p->r.work = p->work;
    decide(p, ETUDE_PREP_BUILT, NULL);
}

/**
 * Take node `p` on as far as it goes without waiting: reuse its product,
 * or take its key's lock and start its recipe when a job is free. A node
 * that gets the lock looks its key up again, since the process that had
 * the lock before it may have published the product.
 */
static void step(struct run *r, struct pnode *p)
{
    if (!p->fresh && p->lock < 0) {
        if (store_lookup(r->s, p->r.key, p->r.digest, &p->r.size) == 0) {
            decide(p, ETUDE_PREP_REUSED, NULL);
            return;
        }
        p->lock = store_lock(r->s, p->r.key);
        if (p->lock < 0 && errno != EAGAIN) {
            fail(p, "the key's lock was not taken", errno);
            return;
        }
        if (p->lock < 0) {
            if (p->phase != PH_LOCKWAIT) {
                p->phase = PH_LOCKWAIT;
                p->since_us = r->now;
            } else if (r->now - p->since_us > p->bound_us) {
                fail(p, "another preparation of the key ran past the bound", 0);
            }
            return;
        }
        if (store_lookup(r->s, p->r.key, p->r.digest, &p->r.size) == 0) {
            decide(p, ETUDE_PREP_REUSED, NULL);
            return;
        }
        p->phase = PH_WAIT;
    }
    if (r->running >= (r->o->jobs ? r->o->jobs : 1))
        return;
    if (launch(r, p) == 0)
        r->running++;
}

/** Say whether every input of node `p` is done, and how. */
static int inputs_ready(const struct etude_prep *g, const struct pnode *p, int *bad)
{
    size_t i;

    *bad = 0;
    for (i = 0; i < p->ninputs; i++) {
        const struct pnode *q = &g->n[p->in[i]];

        if (q->phase != PH_DONE)
            return 0;
        if (q->r.state != ETUDE_PREP_REUSED && q->r.state != ETUDE_PREP_BUILT)
            *bad = 1;
    }
    return 1;
}

/** Cancel node `p`'s recipe once it runs past its bound, and note the bound. */
static void watch(struct run *r, struct pnode *p)
{
    uint64_t until = p->since_us + p->bound_us;

    if (!p->cancelled && r->now > until) {
        p->cancelled = 1;
        etude_exec_cancel(p->proc);
    }
    if (!p->cancelled && (r->soonest == 0 || until < r->soonest))
        r->soonest = until;
}

/** Take node `p` on in one pass of the run. */
static void visit(struct run *r, struct pnode *p)
{
    int bad;

    if (p->phase == PH_DONE)
        return;
    r->left++;
    if (p->phase == PH_RUNNING) {
        watch(r, p);
        return;
    }
    if (!inputs_ready(r->g, p, &bad))
        return;
    if (p->r.started_us == 0)
        p->r.started_us = r->now;
    if (bad)
        decide(p, ETUDE_PREP_BLOCKED, strdup("an input was not prepared"));
    else if (p->argv == NULL)
        put_file(r->s, p);
    else {
        if (p->r.key[0] == '\0')
            key_of(r->g, p);
        step(r, p);
    }
    if (p->phase == PH_DONE)
        r->left--;
    if (p->phase == PH_LOCKWAIT || (p->phase == PH_WAIT && p->lock >= 0))
        r->waiting = 1;
}

/**
 * Wait until a recipe finishes, a recipe's bound passes, or a node that
 * waits for a lock may try it again, and finish the recipe that finished.
 */
static void await(struct run *r)
{
    struct etude_proc *done;
    int timeout = -1;
    size_t i;

    if (r->running == 0) {
        if (r->waiting)
            poll(NULL, 0, LOCK_RETRY_MS);
        return;
    }
    if (r->soonest != 0)
        timeout = r->soonest > r->now ? (int)((r->soonest - r->now) / 1000u) + 1 : 0;
    if (r->waiting && (timeout < 0 || timeout > LOCK_RETRY_MS))
        timeout = LOCK_RETRY_MS;
    done = etude_exec_wait(r->ex, timeout);
    for (i = 0; done != NULL && i < r->g->len; i++)
        if (r->g->n[i].proc == done) {
            r->running--;
            finish(r->s, &r->g->n[i]);
            break;
        }
}

int etude_prep_run(struct etude_prep *g, struct etude_store *s, struct etude_exec *ex,
                   const struct etude_prep_opts *o)
{
    struct run r;
    size_t i;

    for (i = 0; i < g->len; i++)
        result_clear(&g->n[i]);
    if (resolve(g) != 0)
        return -1;
    memset(&r, 0, sizeof(r));
    r.g = g;
    r.s = s;
    r.ex = ex;
    r.o = o;
    for (;;) {
        r.left = 0;
        r.waiting = 0;
        r.soonest = 0;
        r.now = posix_now_us();
        for (i = 0; i < g->len; i++)
            visit(&r, &g->n[i]);
        if (r.left == 0)
            break;
        await(&r);
    }
    for (i = 0; i < g->len; i++)
        if (g->n[i].r.state != ETUDE_PREP_REUSED && g->n[i].r.state != ETUDE_PREP_BUILT)
            return 1;
    return 0;
}
