/* Check grouped maker rules by running the actual make database and requests. */
#include "build_support.h"
#include <fnmatch.h>

#define RULES_MAX 128
struct maker_rule {
    char *key, *targets, *prereqs;
    unsigned pattern, explicit;
};
static struct maker_rule rules[RULES_MAX];
static size_t nrules;
static const char *maker_names[] = {
    "loopdev_disks.sh", "krffsvol.sh volumes",      "krffsvol.sh disks",   "usbdisk_sticks.sh",
    "metal_sticks.sh",  "metal_installed_disks.sh", "parts_disks.sh arch", "parts_disks.sh shared"};

static void extend(char **out, const char *text)
{
    size_t n = *out ? strlen(*out) : 0, z = strlen(text);
    char *p = realloc(*out, n + z + 2);
    if (!p)
        abort();
    p[n] = ' ';
    memcpy(p + n + 1, text, z + 1);
    *out = p;
}
static void release_rules(void)
{
    for (size_t i = 0; i < nrules; i++) {
        free(rules[i].key);
        free(rules[i].targets);
        free(rules[i].prereqs);
    }
    memset(rules, 0, sizeof(rules));
    nrules = 0;
}
static struct maker_rule *find_rule(const char *key)
{
    for (size_t i = 0; i < nrules; i++)
        if (!strcmp(key, rules[i].key))
            return &rules[i];
    if (nrules == RULES_MAX)
        return NULL;
    struct maker_rule *r = &rules[nrules++];
    r->key = strdup(key);
    return r;
}
static int parse_rules(const char *database)
{
    char *copy = strdup(database), *save, *head = NULL;
    release_rules();
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (*line == '\t') {
            if (head && !strncmp(line, "\tsh tests/system/", 17)) {
                char script[512], mode[128] = "", key[1024];
                sscanf(line + 1, "sh tests/system/%511s %127s", script, mode);
                int lower = *mode && strspn(mode, "abcdefghijklmnopqrstuvwxyz") == strlen(mode);
                snprintf(key, sizeof(key), "%s%s%s", script, lower ? " " : "", lower ? mode : "");
                struct maker_rule *r = find_rule(key);
                char *colon = strchr(head, ':');
                if (!r || !colon) {
                    free(copy);
                    free(head);
                    return 0;
                }
                *colon++ = 0;
                for (char *p = colon; *p; p++)
                    if (*p == '|')
                        *p = ' ';
                if (!strncmp(head, "%/", 2)) {
                    r->pattern++;
                    char expanded[16384] = "";
                    size_t n = 0;
                    for (const char *p = head; *p; p++) {
                        const char *w = *p == '%' ? "build" : NULL;
                        size_t z = w ? 5 : 1;
                        if (n + z >= sizeof(expanded))
                            abort();
                        memcpy(expanded + n, w ? w : p, z);
                        n += z;
                    }
                    expanded[n] = 0;
                    extend(&r->targets, expanded);
                } else if (strcmp(head, "&")) {
                    r->explicit ++;
                    extend(&r->targets, head);
                }
                extend(&r->prereqs, colon);
            }
            free(head);
            head = NULL;
            continue;
        }
        if (*line == '#' || !*line)
            continue;
        free(head);
        head = strchr(line, ':') && !fx_regex(line, " [:?+!]?= ") ? strdup(line) : NULL;
    }
    free(head);
    free(copy);
    return 1;
}
static int copy_pattern(struct fixture *f, const char *directory, const char *pattern,
                        const char *target)
{
    DIR *d = opendir(bp(f->tree, directory));
    struct dirent *e;
    int ok = d != NULL;
    while (d && (e = readdir(d)))
        if (!fnmatch(pattern, e->d_name, 0))
            ok &= fx_copy(bp(bp(f->tree, directory), e->d_name),
                          bp(bp(target, directory), e->d_name));
    if (d)
        closedir(d);
    return ok;
}
static int together_tree(struct fixture *f)
{
    const char *files[] = {"Makefile",
                           "tests/registry",
                           "tests/Makefile",
                           "krffs/Makefile",
                           "krffs/manifest_deps.sh",
                           "krffs/programs.sh",
                           "rach2/Makefile",
                           "rach2/sources.mk",
                           "ish/Makefile",
                           "ish/sources.mk",
                           "utils/Makefile",
                           "utils/programs.mk",
                           "ports/doom/Makefile",
                           "tools/etude/sources.mk",
                           "tests/system/suites.mk",
                           NULL};
    const char *target = bp(f->work, "tree");
    for (size_t i = 0; files[i]; i++)
        if (!fx_copy(bp(f->tree, files[i]), bp(target, files[i])))
            return 0;
    if (!fx_tree(bp(f->tree, "mk"), bp(target, "mk")) ||
        !copy_pattern(f, "krffs/manifests", "*.manifest", target))
        return 0;
    const char *arches[] = {"i686", "x86", "x86_64"};
    for (size_t i = 0; i < 3; i++) {
        char path[256];
        snprintf(path, sizeof(path), "rach2/sys/arch/%s/sources.mk", arches[i]);
        if (fx_exists(bp(f->tree, path)) && !fx_copy(bp(f->tree, path), bp(target, path)))
            return 0;
    }
    return fx_text(bp(f->work, "noop.mk"), "together-noop: ; @:\n");
}
static int no_grouped(const char *path)
{
    char *text = fx_read(path);
    int ok = text && !fx_regex(text, "^[^#]*&:");
    free(text);
    return ok;
}
static int named_output(const char *name)
{
    for (size_t i = 0; i < nrules; i++) {
        char *text = strdup(rules[i].targets ? rules[i].targets : ""), *save;
        for (char *p = strtok_r(text, " \t", &save); p; p = strtok_r(NULL, " \t", &save))
            if (!strcmp(p, name)) {
                free(text);
                return 1;
            }
        free(text);
    }
    return 0;
}
static void old_add(struct fixture *f, const char *tree, const char *name, char ***old, size_t *n)
{
    if (named_output(name))
        return;
    for (size_t i = 0; i < *n; i++)
        if (!strcmp((*old)[i], name))
            return;
    char **next = realloc(*old, (*n + 1) * sizeof(*next));
    if (!next)
        abort();
    *old = next;
    next[(*n)++] = strdup(name);
    if (!fx_exists(bp(tree, name)))
        fx_check(f, fx_text(bp(tree, name), ""), "make prerequisite fixture");
}
static void explicit_prereqs(struct fixture *f, const char *tree, const char *database,
                             const char *target, char ***old, size_t *n)
{
    char *copy = strdup(database), *save, *previous = NULL;
    size_t z = strlen(target);
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if ((!previous || strcmp(previous, "# Not a target:")) && !strncmp(line, target, z) &&
            line[z] == ':') {
            char *parts;
            for (char *p = strtok_r(line + z + 1, " \t", &parts); p;
                 p = strtok_r(NULL, " \t", &parts))
                if (strcmp(p, "|"))
                    old_add(f, tree, p, old, n);
            break;
        }
        previous = line;
    }
    free(copy);
}
static char **old_prereqs(struct fixture *f, const char *tree, const char *database, size_t *n)
{
    char **old = NULL;
    *n = 0;
    for (size_t i = 0; i < nrules; i++) {
        char *copy = strdup(rules[i].prereqs ? rules[i].prereqs : ""), *save;
        for (char *p = strtok_r(copy, " \t", &save); p; p = strtok_r(NULL, " \t", &save))
            old_add(f, tree, p, &old, n);
        free(copy);
        copy = strdup(rules[i].targets ? rules[i].targets : "");
        for (char *p = strtok_r(copy, " \t", &save); p; p = strtok_r(NULL, " \t", &save))
            explicit_prereqs(f, tree, database, p, &old, n);
        free(copy);
    }
    return old;
}

/* A maker is a native command mock reached by the product make recipe's sh.
 * This narrow shebang wrapper only execs that mock; it contains no test logic. */
static int write_makers(struct fixture *f, const char *tree)
{
    for (size_t i = 0; i < nrules; i++) {
        char script[512];
        sscanf(rules[i].key, "%511s", script);
        char path[1024], text[8192];
        snprintf(path, sizeof(path), "tests/system/%s", script);
        snprintf(text, sizeof(text), "#!/bin/sh\nexec '%s' --maker '%s' \"$@\"\n", f->self, script);
        if (!fx_write(bp(tree, path), text, strlen(text), 0755))
            return 0;
    }
    FILE *out = fopen(bp(f->work, "maker-rules"), "w");
    if (!out)
        return 0;
    for (size_t i = 0; i < nrules; i++)
        fprintf(out, "%s|%s\n", rules[i].key, rules[i].targets ? rules[i].targets : "");
    return fclose(out) == 0;
}
int build_maker_process(int argc, char **argv)
{
    if (argc < 1)
        return 90;
    char key[1024], candidate[1024];
    snprintf(candidate, sizeof(candidate), "%s%s%s", argv[0], argc > 1 ? " " : "",
             argc > 1 ? argv[1] : "");
    char *text = fx_read(getenv("TOGETHER_RULES")), *save;
    int found = 0;
    for (char *line = text ? strtok_r(text, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        char *bar = strchr(line, '|');
        if (!bar)
            continue;
        *bar++ = 0;
        if (strcmp(line, candidate) && strcmp(line, argv[0]))
            continue;
        snprintf(key, sizeof(key), "%s", line);
        FILE *log = fopen(getenv("TOGETHER_LOG"), "a");
        if (!log) {
            free(text);
            return 91;
        }
        fprintf(log, "%s\n", key);
        fclose(log);
        char *parts;
        for (char *path = strtok_r(bar, " \t", &parts); path; path = strtok_r(NULL, " \t", &parts))
            if (!fx_text(path, "")) {
                free(text);
                return 92;
            }
        found = 1;
        break;
    }
    free(text);
    return found ? 0 : 93;
}

static int maker_run(struct fixture *f, const char *tree, const char *arch, const char *key,
                     char **old, size_t n)
{
    struct maker_rule *r = NULL;
    for (size_t i = 0; i < nrules; i++)
        if (!strcmp(rules[i].key, key))
            r = &rules[i];
    if (!r || !r->targets)
        return fx_check(f, 0, key);
    for (size_t i = 0; i < nrules; i++) {
        char *text = strdup(rules[i].targets ? rules[i].targets : ""), *save;
        for (char *p = strtok_r(text, " \t", &save); p; p = strtok_r(NULL, " \t", &save))
            fx_remove(bp(tree, p));
        free(text);
    }
    fx_text(bp(f->work, "maker-log"), "");
    char setting[64];
    snprintf(setting, sizeof(setting), "ARCH=%s", arch);
    const char **args = calloc(2 * n + 4096, sizeof(*args));
    size_t used = 0;
    args[used++] = "make";
    args[used++] = "--no-print-directory";
    args[used++] = "-r";
    args[used++] = "-j8";
    args[used++] = setting;
    for (size_t i = 0; i < n; i++) {
        args[used++] = "-o";
        args[used++] = old[i];
    }
    char *text = strdup(r->targets), *save;
    for (char *p = strtok_r(text, " \t", &save); p; p = strtok_r(NULL, " \t", &save)) {
        if (used >= 2 * n + 4095)
            abort();
        args[used++] = p;
    }
    int ok = fx_run(f, args, tree, NULL, NULL) == 0;
    free(args);
    free(text);
    char *log = fx_read(bp(f->work, "maker-log"));
    int runs = 0;
    unsigned seen[RULES_MAX] = {0};
    for (char *line = log ? strtok_r(log, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        runs += !strcmp(line, key);
        struct maker_rule *made = NULL;
        for (size_t i = 0; i < nrules; i++)
            if (!strcmp(rules[i].key, line))
                made = &rules[i];
        if (made && ++seen[made - rules] > 1)
            ok = 0;
        if (!made) {
            ok = 0;
            continue;
        }
        char *targets = strdup(made->targets ? made->targets : ""), *parts;
        for (char *p = strtok_r(targets, " \t", &parts); p; p = strtok_r(NULL, " \t", &parts))
            ok &= fx_exists(bp(tree, p));
        free(targets);
    }
    free(log);
    return fx_check(f, ok && runs == 1, key);
}
int build_together(struct fixture *f)
{
    if (!together_tree(f))
        return 0;
    const char *tree = bp(f->work, "tree");
    fx_check(f, no_grouped(bp(tree, "Makefile")) && no_grouped(bp(tree, "tests/system/suites.mk")),
             "makefiles contain no grouped targets");
    DIR *d = opendir(bp(tree, "mk"));
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (!fnmatch("*.mk", e->d_name, 0))
            fx_check(f, no_grouped(bp(bp(tree, "mk"), e->d_name)), e->d_name);
    if (d)
        closedir(d);
    fx_env("ETUDE_SITE", "/nonexistent");
    fx_env("ETUDE_ADMITTED", "/nonexistent");
    fx_env("TOGETHER_LOG", bp(f->work, "maker-log"));
    fx_env("TOGETHER_RULES", bp(f->work, "maker-rules"));
    const char *arches[] = {"i686", "x86_64"};
    for (size_t a = 0; a < 2; a++) {
        char arch[64];
        snprintf(arch, sizeof(arch), "ARCH=%s", arches[a]);
        const char *args[] = {
            "make", "--no-print-directory", "-r", "-pn",           "-f", "Makefile",
            "-f",   bp(f->work, "noop.mk"), arch, "together-noop", NULL};
        if (!fx_check(f, fx_run(f, args, tree, NULL, NULL) == 0, "make database query"))
            continue;
        char *database = strdup(f->out ? f->out : "");
        fx_check(f, !fx_regex(database, "^&:"), "make database has no target named &");
        if (!parse_rules(database)) {
            free(database);
            return 0;
        }
        for (size_t i = 0; i < 8; i++) {
            struct maker_rule *r = NULL;
            for (size_t j = 0; j < nrules; j++)
                if (!strcmp(rules[j].key, maker_names[i]))
                    r = &rules[j];
            fx_check(f, r && r->pattern == 1 && r->explicit == 0, maker_names[i]);
        }
        size_t n;
        char **old = old_prereqs(f, tree, database, &n);
        free(database);
        if (!write_makers(f, tree))
            return 0;
        for (size_t i = 0; i < 8; i++)
            maker_run(f, tree, arches[a], maker_names[i], old, n);
        for (size_t i = 0; i < n; i++)
            free(old[i]);
        free(old);
    }
    release_rules();
    return !f->failed;
}
