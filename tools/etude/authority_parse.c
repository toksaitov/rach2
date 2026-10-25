/* Read only the fixed, protected admission binding and its endpoint state. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "authority_internal.h"
#ifdef _WIN32
#include "win/files.h"
#endif

int authority_read(const char *path, char **bytes, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    char *out = malloc(32769);
    if (!out) {
        fclose(file);
        return -1;
    }
    size_t n = fread(out, 1, 32769, file);
    int error = ferror(file) || n > 32768 || memchr(out, 0, n) != NULL;
    if (fclose(file))
        error = 1;
    if (error || !n || out[n - 1] != '\n') {
        free(out);
        errno = EBADMSG;
        return -1;
    }
    out[n] = 0;
    *bytes = out;
    *size = n;
    return 0;
}

int authority_absolute(const char *text)
{
#ifdef _WIN32
    return etude_win_absolute(text);
#else
    return text && text[0] == '/';
#endif
}

int authority_token(const char *text)
{
    return text && *text && strlen(text) < 256 &&
           strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-") ==
               strlen(text);
}

struct field {
    const char *name;
    char *value;
    size_t size;
};

static int fields(char *bytes, const char *header, struct field *fields, size_t count)
{
    char *line = strchr(bytes, '\n');
    if (!line)
        return -1;
    *line++ = 0;
    if (strcmp(bytes, header))
        return -1;
    unsigned seen = 0;
    while (*line) {
        char *next = strchr(line, '\n'), *value = strchr(line, ' ');
        if (!next || !value || value >= next)
            return -1;
        *next++ = 0;
        *value++ = 0;
        size_t i;
        for (i = 0; i < count && strcmp(line, fields[i].name); i++)
            ;
        if (i == count || seen & (1u << i) || !*value || strlen(value) >= fields[i].size)
            return -1;
        strcpy(fields[i].value, value);
        seen |= 1u << i;
        line = next;
    }
    return seen == (1u << count) - 1 ? 0 : -1;
}

int authority_binding_read(struct authority_binding *b)
{
    struct field f[] = {{"ledger", b->ledger, sizeof b->ledger},
                        {"site", b->site, sizeof b->site},
                        {"gate", b->gate, sizeof b->gate},
                        {"state", b->state, sizeof b->state},
                        {"archive", b->archive, sizeof b->archive},
                        {"operator", b->operator_id, sizeof b->operator_id},
                        {"program", b->program, sizeof b->program},
                        {"launcher_body", b->launcher_body, sizeof b->launcher_body},
                        {"physical_block", b->physical_block, sizeof b->physical_block},
                        {"route", b->route, sizeof b->route}};
    char *bytes;
    size_t size;
    const char *path = authority_installed_binding();
    if (authority_read(path, &bytes, &size))
        return -1;
    int error = fields(bytes, "etude-authority 1", f, sizeof f / sizeof *f);
    free(bytes);
    for (size_t i = 0; !error && i < sizeof f / sizeof *f; i++)
        if (i != 5 && !authority_absolute(f[i].value))
            error = 1;
    if (!error)
        error = authority_protected(path, b->operator_id, 0) ||
                authority_protected(b->gate, b->operator_id, 0) ||
                authority_protected(b->site, b->operator_id, 0) ||
                authority_protected(b->program, b->operator_id, 0) ||
                authority_protected(b->launcher_body, b->operator_id, 0) ||
                authority_protected(b->route, b->operator_id, 0) ||
                authority_protected(b->archive, b->operator_id, -1);
    if (error)
        errno = EACCES;
    return error ? -1 : 0;
}

/* Match protected route and site aliases by identity, before policy access. */
int authority_route(const char *site, const char *descriptor, struct authority_binding *binding)
{
    struct authority_file_id expected, requested, route, configured;
    if (!site || !descriptor || authority_binding_read(binding) ||
        !authority_operator(binding->operator_id) || authority_file_id(binding->site, &expected) ||
        authority_file_id(site, &requested) || expected.device != requested.device ||
        expected.inode != requested.inode || authority_file_id(binding->route, &route) ||
        authority_file_id(descriptor, &configured) || route.device != configured.device ||
        route.inode != configured.inode) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static int positive(const char *text, uint64_t *value)
{
    if (*text < '1' || *text > '9' || strspn(text, "0123456789") != strlen(text))
        return -1;
    uint64_t n = 0;
    for (; *text; text++) {
        unsigned digit = (unsigned)(*text - '0');
        if (n > (UINT64_MAX - digit) / 10)
            return -1;
        n = n * 10 + digit;
    }
    *value = n;
    return 0;
}

int authority_state_read(const struct authority_binding *b, struct authority_state *s)
{
    char *bytes, pid[32], creation[32];
    size_t size;
    if (authority_protected(b->state, b->operator_id, 1) || authority_read(b->state, &bytes, &size))
        return -1;
    struct field f[] = {{"run", s->run, sizeof s->run},
                        {"commit", s->commit, sizeof s->commit},
                        {"phase", s->phase, sizeof s->phase},
                        {"nonce", s->nonce, sizeof s->nonce},
                        {"snapshot", s->snapshot, sizeof s->snapshot},
                        {"owner_pid", pid, sizeof pid},
                        {"owner_creation", creation, sizeof creation}};
    int error = fields(bytes, "etude-authority-state 1", f, sizeof f / sizeof *f);
    uint64_t p = 0;
    free(bytes);
    if (!error)
        error = positive(pid, &p) || p > 0x7fffffff || positive(creation, &s->owner.start) ||
                !authority_token(s->run) || strlen(s->commit) != 40 ||
                strspn(s->commit, "0123456789abcdef") != 40 || strlen(s->nonce) != 64 ||
                strspn(s->nonce, "0123456789abcdef") != 64 || !authority_absolute(s->snapshot) ||
                (strcmp(s->phase, "blocked") && strcmp(s->phase, "producer") &&
                 strcmp(s->phase, "consumer"));
    s->owner.pid = (long)p;
    s->owner.start_known = 1;
    if (error)
        errno = EBADMSG;
    return error ? -1 : 0;
}

int authority_owner_live(const struct etude_pid *id)
{
    struct etude_pid now;
    return id->start_known && etude_process_state(id->pid, &now) == 1 && now.start_known &&
           now.start == id->start;
}

int authority_state_write(const struct authority_binding *b, const struct authority_state *s)
{
    char bytes[8704];
    int n = snprintf(bytes, sizeof bytes,
                     "etude-authority-state 1\nrun %s\ncommit %s\nphase %s\nnonce %s\n"
                     "snapshot %s\nowner_pid %ld\nowner_creation %llu\n",
                     s->run, s->commit, s->phase, s->nonce, s->snapshot, s->owner.pid,
                     (unsigned long long)s->owner.start);
    if (n < 0 || (size_t)n >= sizeof bytes) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return authority_publish(b->state, bytes, (size_t)n, 1);
}

/* The capacity is read from the installed policy, never a caller projection. */
int authority_policy(const struct authority_binding *b, const char *path, char **bytes,
                     size_t *size, uint64_t capacity[3], int *enabled)
{
    if (authority_protected(path, b->operator_id, 0) || authority_read(path, bytes, size))
        return -1;
    const char *p = *bytes;
    if (strncmp(p, "etude-site 2\n", 13))
        goto bad;
    const char *endpoint = strstr(p, "\nendpoint "), *cap = strstr(p, "\ncapacity ");
    if (!endpoint || !cap || strstr(endpoint + 1, "\nendpoint ") || strstr(cap + 1, "\ncapacity "))
        goto bad;
    endpoint += 10;
    if (strncmp(endpoint, "disabled\n", 9) && strncmp(endpoint, "linux\n", 6) &&
        strncmp(endpoint, "macos\n", 6) && strncmp(endpoint, "windows\n", 8))
        goto bad;
    *enabled = strncmp(endpoint, "disabled\n", 9) != 0;
    cap += 10;
    for (size_t i = 0; i < 3; i++) {
        char number[32];
        size_t n = strspn(cap, "0123456789");
        if (!n || n >= sizeof number || cap[n] != (i == 2 ? '\n' : ','))
            goto bad;
        memcpy(number, cap, n);
        number[n] = 0;
        if (positive(number, &capacity[i]))
            goto bad;
        cap += n + 1;
    }
    return 0;
bad:
    free(*bytes);
    *bytes = NULL;
    errno = EBADMSG;
    return -1;
}
