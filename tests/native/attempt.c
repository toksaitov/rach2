/* attempt.c: translate inventory fixtures into the shared native API. */
#define _POSIX_C_SOURCE 200809L
#include "collection.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
char *nc_text(struct nc_attempt *a, const char *format, ...)
{
    char data[16384], *copy;
    va_list ap;
    int n;
    if (a->nallocated == 256)
        return NULL;
    va_start(ap, format);
    n = vsnprintf(data, sizeof(data), format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(data))
        return NULL;
    copy = strdup(data);
    if (copy)
        a->allocated[a->nallocated++] = copy;
    return copy;
}
static int fail(struct nc_attempt *a, const char *reason)
{
    snprintf(a->why, sizeof(a->why), "%s", reason);
    return 0;
}
static int file(struct nc_attempt *a, const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) || !S_ISREG(st.st_mode))
        return fail(a, "a declared prepared file is missing or not regular");
    for (size_t n = 0; n < a->spec.nfiles; n++)
        if (!strcmp(a->files[n], path))
            return 1;
    if (a->spec.nfiles == 16)
        return fail(a, "too many invocation files");
    a->files[a->spec.nfiles++] = (char *)path;
    return 1;
}
static int fixture(struct nc_attempt *a, const char *value)
{
    if (!value || a->spec.nfixtures == 16)
        return fail(a, "too many or invalid fixtures");
    a->fixtures[a->spec.nfixtures++] = (char *)value;
    return 1;
}
static const char *program(struct nc_attempt *a, const struct nc_config *c, const char *name)
{
    if (strstr(name, "..") || *name == '/')
        return NULL;
    if (!strncmp(name, "native/", 7) || !strncmp(name, "system/", 7) ||
        !strncmp(name, "graphics/", 9))
        return nc_text(a, "%s/tests/%s", c->tree, name);
    return nc_text(a, "%s/%s/tests/%s", c->tree,
                   !strcmp(c->arch, "x86_64") ? "build/x86_64" : "build", name);
}
static int staged(struct nc_attempt *a, const struct nc_config *conf, char *names)
{
    char *save = NULL, *word = strtok_r(names, ",", &save), list[4096] = "stage";
    size_t used = 5;
    while (word) {
        const char *name = strrchr(word, '/');
        name = name ? name + 1 : word;
        if (!file(a, program(a, conf, word)) || used + strlen(name) + 2 >= sizeof(list))
            return 0;
        list[used++] = ' ';
        strcpy(list + used, name);
        used += strlen(name);
        word = strtok_r(NULL, ",", &save);
    }
    return used > 5 && fixture(a, nc_text(a, "%s", list));
}
static int mapped(struct nc_attempt *a, const struct nc_inventory *inventory, char *names, int ids)
{
    char list[4096], *save = NULL, *word = strtok_r(names, ",", &save);
    size_t used = (size_t)snprintf(list, sizeof(list), "%s", ids ? "ids" : "ports");
    while (word) {
        unsigned old, value;
        char tail;
        if (sscanf(word, "%u%c", &old, &tail) != 1 ||
            !nc_map_value(inventory, ids ? "idmap" : "portmap", old, &value))
            return fail(a, "maps has no unique native value for an id or port");
        int n = snprintf(list + used, sizeof(list) - used, " %u:%u", old, value);
        if (n < 0 || (size_t)n >= sizeof(list) - used)
            return 0;
        used += (size_t)n;
        word = strtok_r(NULL, ",", &save);
    }
    return fixture(a, nc_text(a, "%s", list));
}
static int peer(struct nc_attempt *a, const struct nc_config *c, const char *role)
{
    const char *colon = c->peer ? strrchr(c->peer, ':') : NULL;
    if (!strcmp(role, "dgram-outside"))
        return c->outside ? fixture(a, nc_text(a, "peer %s 9", c->outside))
                          : fail(a, "the external datagram peer was not configured");
    if (!colon || colon == c->peer || !colon[1])
        return fail(a, "the external echo peer was not configured");
    if (!strcmp(role, "echo"))
        return fixture(a, nc_text(a, "peer %.*s %s", (int)(colon - c->peer), c->peer, colon + 1));
    if (strcmp(role, "network"))
        return fail(a, "unknown external peer role");
    if (!c->interface || !c->address || !c->gateway || !c->far || !c->empty || !c->silent)
        return fail(a, "the machine network addresses were not all configured");
    return fixture(a,
                   nc_text(a, "peer %.*s %s ifname=%s addr=%s gateway=%s far=%s empty=%s silent=%s",
                           (int)(colon - c->peer), c->peer, colon + 1, c->interface, c->address,
                           c->gateway, c->far, c->empty, c->silent));
}
static int image_files(struct nc_attempt *a, const struct nc_config *c,
                       const struct nc_inventory *i, char *names)
{
    char *save = NULL, *word = strtok_r(names, ",", &save);
    if (!word)
        return 0;
    while (word) {
        if (strchr(word, '/') || strstr(word, "..") || !nc_image_has(i, word, NULL) ||
            !file(a, nc_text(a, "%s/%s", c->images, word)))
            return 0;
        word = strtok_r(NULL, ",", &save);
    }
    return 1;
}
static int volume(struct nc_attempt *a, const struct nc_config *c, const struct nc_inventory *i,
                  char *value)
{
    char *type = value, *name = strchr(type, ':'), *how;
    if (!name)
        return 0;
    *name++ = 0;
    how = strchr(name, ':');
    if (how)
        *how++ = 0;
    if (strchr(name, '/') || strstr(name, "..") || !nc_image_has(i, name, type))
        return 0;
    return file(a, nc_text(a, "%s/%s", c->images, name)) &&
           fixture(a, nc_text(a, "volume %s %s %s", type, name, how ? how : "mounted"));
}
static int root(struct nc_attempt *a, const struct nc_config *conf, const struct nc_inventory *i,
                const struct nc_case *c, char *type)
{
    char *colon = strchr(type, ':');
    size_t n;
    if (colon)
        *colon = 0;
    for (n = 0; n < i->nroots; n++)
        if (!strcmp(i->roots[n].name, c->name))
            return file(a, nc_text(a, "%s/%s", conf->images, i->roots[n].image)) &&
                   fixture(a, nc_text(a, "root %s %s /%s", type, i->roots[n].image, c->argv[0]));
    return fail(a, "the prepared roots index has no root for the case");
}
static int size_fixture(struct nc_attempt *a, const char *value)
{
    char *end;
    unsigned long long bytes = strtoull(value, &end, 10), scale = 1;
    if (!bytes || end == value)
        return 0;
    if (*end == 'K' && !end[1])
        scale = 1024;
    else if (*end == 'M' && !end[1])
        scale = 1048576;
    else if (*end)
        return 0;
    if (bytes > UINT64_MAX / scale)
        return 0;
    return fixture(a, nc_text(a, "tmpfs %llu", bytes * scale));
}
static int terminal(struct nc_attempt *a, const struct nc_config *c, const char *value)
{
    if (!fixture(a, "pty"))
        return 0;
    if (*value) {
        if (strchr(value, '/') || strstr(value, "..") ||
            !file(a, nc_text(a, "%s/%s", c->inventory, value)))
            return 0;
        a->spec.input = nc_text(a, "%s", value);
    }
    return 1;
}
static int end(struct nc_attempt *a, const char *value)
{
    unsigned code;
    char tail;
    if (!strncmp(value, "signal:", 7)) {
        if (sscanf(value + 7, "%u%c", &code, &tail) != 1 || !code || code > 255)
            return 0;
        a->spec.expected_end = ETUDE_END_SIGNALED;
    } else if (!strncmp(value, "exit:", 5)) {
        if (sscanf(value + 5, "%u%c", &code, &tail) != 1 || code > 255)
            return 0;
        a->spec.expected_end = ETUDE_END_EXITED;
    } else
        return 0;
    a->spec.expected_value = (int)code;
    return 1;
}
static int flag_fixture(struct nc_attempt *a, const struct nc_config *conf, const char *value)
{
    static const char *flags[] = {"mountns", "ipcns", "utsns", "log", "alone", "console", NULL};
    if (!strcmp(value, "pty"))
        return terminal(a, conf, "");
    for (size_t n = 0; flags[n]; n++)
        if (!strcmp(value, flags[n]))
            return fixture(a, value);
    return fail(a, "unknown fixture kind");
}
static int one_fixture(struct nc_attempt *a, const struct nc_config *conf,
                       const struct nc_inventory *i, const struct nc_case *c, const char *text)
{
    char *value = nc_text(a, "%s", text), *colon;
    if (!value)
        return 0;
    colon = strchr(value, ':');
    if (colon)
        *colon++ = 0;
    if (!colon)
        return flag_fixture(a, conf, value);
    if (!strcmp(value, "stage"))
        return staged(a, conf, colon);
    if (!strcmp(value, "image"))
        return image_files(a, conf, i, colon);
    if (!strcmp(value, "volume"))
        return volume(a, conf, i, colon);
    if (!strcmp(value, "root"))
        return root(a, conf, i, c, colon);
    if (!strcmp(value, "tmpfs"))
        return size_fixture(a, colon);
    if (!strcmp(value, "ids") || !strcmp(value, "ports"))
        return mapped(a, i, colon, !strcmp(value, "ids"));
    if (!strcmp(value, "pty"))
        return terminal(a, conf, colon);
    if (!strcmp(value, "peer"))
        return peer(a, conf, colon);
    if (!strcmp(value, "file"))
        return *colon != '/' && !strstr(colon, "..") &&
               file(a, nc_text(a, "%s/%s", conf->tree, colon));
    if (!strcmp(value, "end"))
        return end(a, colon);
    if (!strcmp(value, "env")) {
        if (strchr(colon, '=')) {
            if (a->spec.nenvironment == 32)
                return 0;
            a->environment[a->spec.nenvironment++] = colon;
        } else {
            if (a->spec.nunset == 32)
                return 0;
            a->unset[a->spec.nunset++] = colon;
        }
        return 1;
    }
    return fail(a, "unknown fixture kind");
}
static char *version(struct nc_attempt *a, const struct nc_config *conf, const char *name)
{
    char path[4096], line[8192];
    FILE *fp;
    char *result = NULL;
    if (strchr(name, '/') || strstr(name, ".."))
        return NULL;
    snprintf(path, sizeof(path), "%s/vendor/gnu/agents/packages/%s.sh", conf->tree, name);
    fp = fopen(path, "r");
    if (!fp)
        return NULL;
    while (fgets(line, sizeof(line), fp))
        if (!strncmp(line, "VERSION=", 8)) {
            line[strcspn(line, "\r\n")] = 0;
            result = nc_text(a, "%s", line + 8);
            break;
        }
    fclose(fp);
    return result && *result ? result : NULL;
}
static int arguments(struct nc_attempt *a, const struct nc_config *conf, const struct nc_case *c)
{
    const char *slash = strrchr(c->argv[0], '/');
    a->argv[a->argc++] = nc_text(a, "%s", slash ? slash + 1 : c->argv[0]);
    for (size_t n = 1; n < c->argc; n++) {
        const char *v = c->argv[n];
        if (!strcmp(v, "@net")) {
            if (a->argc + 3 >= NC_WORD_MAX || !conf->interface || !conf->address || !conf->gateway)
                return 0;
            a->argv[a->argc++] = nc_text(a, "ifname=%s", conf->interface);
            a->argv[a->argc++] = nc_text(a, "addr=%s", conf->address);
            a->argv[a->argc++] = nc_text(a, "gateway=%s", conf->gateway);
        } else {
            if (a->argc + 1 >= NC_WORD_MAX)
                return 0;
            a->argv[a->argc++] =
                !strncmp(v, "@agents:", 8) ? version(a, conf, v + 8) : nc_text(a, "%s", v);
        }
    }
    for (size_t n = 0; n < a->argc; n++)
        if (!a->argv[n])
            return 0;
    return 1;
}
int nc_attempt_make(struct nc_attempt *a, const struct nc_config *conf,
                    const struct nc_inventory *i, const struct nc_case *c)
{
    memset(a, 0, sizeof(*a));
    a->spec.files = (const char *const *)a->files;
    a->spec.fixtures = (const char *const *)a->fixtures;
    a->spec.environment = (const char *const *)a->environment;
    a->spec.unset = (const char *const *)a->unset;
    a->spec.argv = a->argv;
    a->spec.run = conf->run;
    a->spec.case_id = nc_text(a, "native.%s", c->name);
    a->spec.attempt = nc_text(a, "%s.%s.1", conf->run, c->name);
    a->spec.expected_end = ETUDE_END_EXITED;
    a->spec.staged_only = 1;

    a->spec.grace_ms = 5000;

    a->spec.output_max = 16777216;
    a->spec.reconnects = 3;
    a->transport[0] = (char *)conf->guest;
    a->transport[1] = "--stdio";
    a->transport[2] = "--state";
    a->transport[3] = (char *)conf->state;
    for (unsigned n = 0; n < 8; n++) {
        a->transport[4 + n * 2] = "--stage";
        a->transport[5 + n * 2] = n ? nc_text(a, "%s/%u", conf->stage, n) : (char *)conf->stage;
        if (!a->transport[5 + n * 2])
            return fail(a, "cannot allocate the prepared stage paths");
    }
    a->spec.transport_argv = a->transport;
    if (!file(a, program(a, conf, c->argv[0])) || !arguments(a, conf, c))
        return 0;
    for (size_t n = 0; n < c->nfixtures; n++)
        if (!one_fixture(a, conf, i, c, c->fixtures[n])) {
            if (!*a->why)
                snprintf(a->why, sizeof(a->why), "invalid or missing fixture: %s", c->fixtures[n]);
            return 0;
        }
    return 1;
}
void nc_attempt_free(struct nc_attempt *a)
{
    for (size_t n = 0; n < a->nallocated; n++)
        free(a->allocated[n]);
    memset(a, 0, sizeof(*a));
}
