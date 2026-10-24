/* Serialize the enrolled endpoint before policy reads and owned claims. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "authority_internal.h"
#include "include/etude/ledger.h"
#ifdef _WIN32
#include "win/files.h"
#else
#include <unistd.h>
#endif

static int same_file(const char *one, const char *two)
{
    struct authority_file_id a, b;
    return !authority_file_id(one, &a) && !authority_file_id(two, &b) &&
           a.device == b.device && a.inode == b.inode;
}

static struct etude_authority *binding_open(int exclusive)
{
    struct etude_authority *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->gate.value = -1;
    if (authority_binding_read(&a->binding) ||
        authority_gate_open(a->binding.gate, exclusive, &a->gate)) {
        etude_authority_close(a);
        return NULL;
    }
    a->exclusive = exclusive;
    return a;
}

int etude_authority_open(const char *ledger, struct etude_authority **out)
{
    *out = NULL;
    const char *binding = authority_installed_binding();
    if (access(binding, F_OK))
        return errno == ENOENT ? 0 : -1;
    struct authority_binding b;
    if (authority_binding_read(&b))
        return -1;
    /* A fixed shared ledger may be reached through another site or alias.
     * Unrelated private fixture ledgers do not join this authority. */
    if (!same_file(b.ledger, ledger)) {
        if (!strcmp(b.ledger, ledger)) {
            errno = ENOENT;
            return -1;
        }
        return 0;
    }
    struct etude_authority *a = binding_open(0);
    if (!a)
        return -1;
    if (!same_file(a->binding.ledger, ledger)) {
        etude_authority_close(a);
        errno = ESTALE;
        return -1;
    }
    *out = a;
    return 0;
}

void etude_authority_close(struct etude_authority *a)
{
    if (a) {
        authority_gate_close(&a->gate);
        free(a);
    }
}

static int phase_authorized(const struct etude_authority *a)
{
    const char *nonce = getenv("ETUDE_AUTHORITY_NONCE"), *run = getenv("ETUDE_AUTHORITY_RUN");
    const char *phase = getenv("ETUDE_AUTHORITY_PHASE");
    return nonce && run && phase && authority_operator(a->binding.operator_id) &&
           !strcmp(nonce, a->state.nonce) && !strcmp(run, a->state.run) &&
           !strcmp(phase, a->state.phase) && strcmp(phase, "blocked") &&
           authority_owner_live(&a->state.owner);
}

int etude_authority_claim(struct etude_authority *a, const char *id,
                          const struct etude_ledger_res *capacity, size_t count)
{
    (void)id;
    if (!a)
        return 0;
    uint64_t actual[3];
    int enabled;
    char *bytes = NULL;
    size_t size;
    const char *policy = a->binding.site;
    int authorized = 0;
    if (access(a->binding.state, F_OK) == 0) {
        if (authority_state_read(&a->binding, &a->state) || !phase_authorized(a))
            goto denied;
        policy = a->state.snapshot;
        authorized = 1;
    } else if (errno != ENOENT)
        goto denied;
    if (authority_policy(&a->binding, policy, &bytes, &size, actual, &enabled))
        return -1;
    free(bytes);
    if (!enabled && !authorized)
        goto denied;
    if (!authorized && (access(a->binding.physical_block, F_OK) == 0 || errno != ENOENT))
        goto denied;
    const char *names[] = {"cpu_millis", "memory_bytes", "disk_bytes"};
    for (size_t i = 0; i < count; i++)
        for (size_t j = 0; j < 3; j++)
            if (!strcmp(capacity[i].name, names[j]) && capacity[i].amount > actual[j])
                goto denied;
    return 0;
denied:
    errno = EACCES;
    return -1;
}

static int empty(const struct etude_authority *a)
{
    char path[AUTHORITY_PATH], *bytes = NULL;
    size_t size;
    int n = snprintf(path, sizeof path, "%s/claims", a->binding.ledger);
    if (n < 0 || (size_t)n >= sizeof path || authority_read(path, &bytes, &size))
        return 0;
    int ok = size == 15 && !memcmp(bytes, "etude-ledger 1\n", 15);
    free(bytes);
    return ok;
}

static int phase_valid(const struct etude_authority_phase *p)
{
    return p && authority_token(p->run_id) && p->commit && strlen(p->commit) == 40 &&
           strspn(p->commit, "0123456789abcdef") == 40 && p->nonce && strlen(p->nonce) == 64 &&
           strspn(p->nonce, "0123456789abcdef") == 64 && authority_owner_live(&p->owner);
}

static int disable_policy(struct etude_authority *a, char *bytes, size_t size)
{
    char *endpoint = strstr(bytes, "\nendpoint ") + 10;
    char *end = strchr(endpoint, '\n');
    if (!end)
        return -1;
    size_t before = (size_t)(endpoint - bytes), after = size - (size_t)(end - bytes);
    char *disabled = malloc(before + 8 + after);
    if (!disabled)
        return -1;
    memcpy(disabled, bytes, before);
    memcpy(disabled + before, "disabled", 8);
    memcpy(disabled + before + 8, end, after);
    int error = authority_publish(a->binding.site, disabled, before + 8 + after, 0);
    free(disabled);
    return error;
}

int etude_authority_freeze(const struct etude_authority_phase *p, struct etude_authority **out)
{
    *out = NULL;
    if (!phase_valid(p)) {
        errno = EINVAL;
        return -1;
    }
    struct etude_authority *a = binding_open(1);
    if (!a)
        return -1;
    char *bytes = NULL;
    size_t size;
    uint64_t capacity[3];
    int enabled, error = 1;
    if (!authority_operator(a->binding.operator_id) || !empty(a) ||
        access(a->binding.physical_block, F_OK) == 0 || errno != ENOENT ||
        access(a->binding.state, F_OK) == 0 || errno != ENOENT)
        goto done;
    if (authority_policy(&a->binding, a->binding.site, &bytes, &size, capacity, &enabled))
        goto done;
    int n = snprintf(a->state.snapshot, sizeof a->state.snapshot, "%s/%s.policy",
                     a->binding.archive, p->run_id);
    if (n < 0 || (size_t)n >= sizeof a->state.snapshot ||
        access(a->state.snapshot, F_OK) == 0 || errno != ENOENT)
        goto done;
    strcpy(a->state.run, p->run_id);
    strcpy(a->state.commit, p->commit);
    strcpy(a->state.phase, "blocked");
    strcpy(a->state.nonce, p->nonce);
    a->state.owner = p->owner;
    if (authority_publish(a->state.snapshot, bytes, size, 1) ||
        authority_state_write(&a->binding, &a->state))
        goto done;
    /* State is durable first. A crash before policy replacement still denies
     * direct admission through a previously read enabled site projection. */
    error = disable_policy(a, bytes, size);
    if (!error) {
        *out = a;
        a = NULL;
    }
done:
    free(bytes);
    etude_authority_close(a);
    if (error && !errno)
        errno = EACCES;
    return error ? -1 : 0;
}

int etude_authority_allow(struct etude_authority *a, const struct etude_authority_phase *p)
{
    if (!a || !a->exclusive || !phase_valid(p) || !authority_operator(a->binding.operator_id) ||
        !p->phase || (strcmp(p->phase, "producer") && strcmp(p->phase, "consumer")) ||
        strcmp(p->run_id, a->state.run) || strcmp(p->commit, a->state.commit) ||
        strcmp(p->nonce, a->state.nonce) || !empty(a) ||
        (strcmp(a->state.phase, "blocked") &&
         (a->state.owner.pid != p->owner.pid || a->state.owner.start != p->owner.start))) {
        errno = EACCES;
        return -1;
    }
    strcpy(a->state.phase, p->phase);
    a->state.owner = p->owner;
    return authority_state_write(&a->binding, &a->state);
}

int etude_authority_block(struct etude_authority *a)
{
    if (!a || !a->exclusive || !authority_operator(a->binding.operator_id) || !empty(a)) {
        errno = EACCES;
        return -1;
    }
    strcpy(a->state.phase, "blocked");
    return authority_state_write(&a->binding, &a->state);
}

int etude_authority_restore(struct etude_authority *a)
{
    if (!a || !a->exclusive || strcmp(a->state.phase, "blocked") ||
        !authority_operator(a->binding.operator_id) || !empty(a)) {
        errno = EACCES;
        return -1;
    }
    char *bytes;
    size_t size;
    if (authority_read(a->state.snapshot, &bytes, &size))
        return -1;
    int error = authority_publish(a->binding.site, bytes, size, 0);
    free(bytes);
    /* Keep the snapshot as evidence. A failure leaves the durable block. */
    return error || unlink(a->binding.state) ? -1 : 0;
}


int etude_authority_reopen(const struct etude_authority_phase *p, struct etude_authority **out)
{
    *out = NULL;
    if (!phase_valid(p)) {
        errno = EINVAL;
        return -1;
    }
    struct etude_authority *a = binding_open(1);
    if (!a)
        return -1;
    if (!authority_operator(a->binding.operator_id) ||
        authority_state_read(&a->binding, &a->state) || strcmp(p->run_id, a->state.run) ||
        strcmp(p->commit, a->state.commit) || strcmp(p->nonce, a->state.nonce) || !empty(a) ||
        (strcmp(a->state.phase, "blocked") &&
         (a->state.owner.pid != p->owner.pid || a->state.owner.start != p->owner.start))) {
        etude_authority_close(a);
        errno = EACCES;
        return -1;
    }
    *out = a;
    return 0;
}


int etude_authority_physical(const struct etude_authority_phase *p, int release)
{
    struct etude_authority *a = binding_open(1);
    if (!a)
        return -1;
    int error = !phase_valid(p) || !authority_operator(a->binding.operator_id) || !empty(a);
    char expected[768];
    int n = snprintf(expected, sizeof expected,
                     "etude-physical-block 1\nrun %s\ncommit %s\nowner_pid %ld\nowner_creation %llu\n",
                     p->run_id, p->commit, p->owner.pid, (unsigned long long)p->owner.start);
    if (n < 0 || (size_t)n >= sizeof expected)
        error = 1;
    if (!error && release) {
        char *bytes = NULL;
        size_t size;
        error = authority_protected(a->binding.physical_block, a->binding.operator_id, 0) ||
                authority_read(a->binding.physical_block, &bytes, &size);
        if (!error)
            error = size != (size_t)n || memcmp(bytes, expected, size);
        free(bytes);
        if (!error)
            error = unlink(a->binding.physical_block);
    } else if (!error) {
        error = access(a->binding.physical_block, F_OK) == 0 || errno != ENOENT ||
                authority_state_read(&a->binding, &a->state) || strcmp(a->state.phase, "blocked") ||
                strcmp(a->state.run, p->run_id) || strcmp(a->state.nonce, p->nonce);
        if (!error)
            error = authority_publish(a->binding.physical_block, expected, (size_t)n, 0);
    }
    etude_authority_close(a);
    if (error)
        errno = EACCES;
    return error ? -1 : 0;
}
