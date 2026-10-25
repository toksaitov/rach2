/* Read the same installed policy used by ordinary native admission. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <pwd.h>
#else
#include "win/files.h"
#endif
#include "authority_internal.h"
#include "routine_entry.h"
#include "routine_proof.h"

static const char *routine_endpoint(void)
{
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

static int routine_positive(const char *text, uint64_t *value)
{
    if (!text || !*text || *text == '0' || strlen(text) > 15 ||
        strspn(text, "0123456789") != strlen(text))
        return -1;
    *value = strtoull(text, NULL, 10);
    return !*value ? -1 : 0;
}

int routine_route_mode(const char *path, char mode[24])
{
    struct json *doc = NULL;
    if (routine_json(path, &doc))
        return -1;
    const struct json *version = json_member(doc, "schema_version");
    const char *format = json_text(doc, "format"), *name = json_text(doc, "mode");
    int error = !format || strcmp(format, "etude-routine-route") || !version ||
                version->type != JSON_INTEGER || version->integer != 1 || !name ||
                (strcmp(name, "macos-import") && strcmp(name, "windows-import") &&
                 strcmp(name, "wsl-native"));
    if (!error && !strcmp(name, "macos-import")) {
#ifndef _WIN32
        struct stat st;
        error = strcmp(routine_endpoint(), "macos") || lstat(path, &st) || !S_ISREG(st.st_mode) ||
                (st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022);
#else
        error = 1;
#endif
    }
    if (!error)
        strcpy(mode, name);
    json_free(doc);
    return error ? -1 : 0;
}

static int routine_site_field(struct routine_site *site, unsigned *seen, char *line)
{
    static const char *const fields[] = {"etude-site", "endpoint",     "launcher", "ledger",
                                         "locks",      "etude",        "capacity", "wait",
                                         "seconds",    "boundary_lazy"};
    char *value = strchr(line, ' ');
    if (!value)
        return -1;
    *value++ = 0;
    value[strcspn(value, "\r\n")] = 0;
    size_t at;
    for (at = 0; at < sizeof fields / sizeof(*fields) && strcmp(line, fields[at]); at++)
        ;
    if (at == sizeof fields / sizeof(*fields) || (*seen & (1u << at)) || !*value)
        return -1;
    *seen |= 1u << at;
    char *destination = at == 1   ? site->endpoint
                        : at == 2 ? site->launcher
                        : at == 3 ? site->ledger
                        : at == 4 ? site->locks
                        : at == 5 ? site->program
                                  : site->capacity_text;
    size_t size = at == 1   ? sizeof site->endpoint
                  : at == 6 ? sizeof site->capacity_text
                            : ROUTINE_PATH;
    if (at == 0)
        return strcmp(value, "2") ? -1 : 0;
    if (at == 9)
        return strcmp(value, "bigmem-boundary-v1") ? -1 : 0;
    if (at == 7 || at == 8) {
        uint64_t bound;
        if (routine_positive(value, &bound) || bound > 86400)
            return -1;
        if (at == 7)
            site->wait = (unsigned)bound;
        else
            site->seconds = (unsigned)bound;
        return 0;
    }
    if (strlen(value) >= size)
        return -1;
    strcpy(destination, value);
    return 0;
}

static int routine_site_fields(const char *path, struct routine_site *site)
{
    FILE *file = fopen(path, "rb");
    char line[ROUTINE_PATH + 64];
    unsigned seen = 0;
    int error = !file;
    while (!error && fgets(line, sizeof line, file)) {
        error = strlen(line) == sizeof line - 1 || routine_site_field(site, &seen, line);
    }
    if (file) {
        if (ferror(file))
            error = 1;
        if (fclose(file))
            error = 1;
    }
    return error || (seen & 511) != 511 ? -1 : 0;
}

static int routine_site_resources(struct routine_site *site)
{
    char copy[128], *part, *end;
    strcpy(copy, site->capacity_text);
    part = copy;
    for (size_t i = 0; i < 3; i++) {
        end = strchr(part, ',');
        if ((i < 2) != (end != NULL))
            return -1;
        if (end)
            *end = 0;
        if (routine_positive(part, &site->capacity[i]))
            return -1;
        part = end ? end + 1 : NULL;
    }
    const char *given = getenv("JOBS");
    uint64_t jobs;
    if (routine_positive(given ? given : "4", &jobs) || jobs > 1024)
        return -1;
    site->jobs = (unsigned)jobs;
    uint64_t cpu = jobs * 1000, memory = (jobs + 1) * 1073741824ull, disk = 42949672960ull;
    if (cpu > site->capacity[0] || memory > site->capacity[1] || disk > site->capacity[2])
        return -1;
    snprintf(site->resources, sizeof site->resources, "%llu,%llu,%llu", (unsigned long long)cpu,
             (unsigned long long)memory, (unsigned long long)disk);
#ifdef _WIN32
    DWORD size = sizeof site->account;
    if (!GetUserNameA(site->account, &size))
        return -1;
#else
    struct passwd *user = getpwuid(geteuid());
    if (!user || strlen(user->pw_name) >= sizeof site->account)
        return -1;
    strcpy(site->account, user->pw_name);
#endif
    return !*site->account ? -1 : 0;
}

static int routine_site_paths(const char *path, const struct routine_site *site)
{
    const char *paths[] = {path, site->launcher, site->ledger, site->locks, site->program};
    struct stat st;
    for (size_t i = 0; i < sizeof paths / sizeof(*paths); i++)
        if (!authority_absolute(paths[i]) || stat(paths[i], &st) ||
            ((i == 2 || i == 3) ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)))
            return -1;
    if (access(site->launcher, X_OK) || access(site->program, X_OK))
        return -1;
    char claims[ROUTINE_PATH + 16], first[64];
    snprintf(claims, sizeof claims, "%s/claims", site->ledger);
    FILE *file = fopen(claims, "rb");
    int error = !file || !fgets(first, sizeof first, file) || strcmp(first, "etude-ledger 1\n");
    if (file && fclose(file))
        error = 1;
    static const char *const names[] = {"ETUDE_REFERENCE", "ETUDE_PREPARATION_IMPORT"};
    for (size_t i = 0; !error && i < sizeof names / sizeof(*names); i++) {
        const char *input = getenv(names[i]);
        if (input && *input)
            error = !authority_absolute(input) || stat(input, &st) || !S_ISREG(st.st_mode) ||
                    access(input, R_OK);
    }
    return error ? -1 : 0;
}

int routine_site_read(const char *path, const char *route, struct routine_site *site)
{
    struct authority_binding binding;
    memset(site, 0, sizeof *site);
    if (!authority_absolute(path) || (route && routine_route_mode(route, site->mode)))
        return -1;
    int routed = route && strcmp(site->mode, "macos-import");
    if (routed && authority_route(path, route, &binding))
        return -1;
    if (routine_site_fields(path, site) || (routed && strcmp(binding.program, site->program)))
        return -1;
    if (routed && !strcmp(site->endpoint, "disabled"))
        strcpy(site->endpoint, routine_endpoint());
    if (strcmp(site->endpoint, routine_endpoint()))
        return -1;
    return routine_site_paths(path, site) || routine_site_resources(site) ? -1 : 0;
}
