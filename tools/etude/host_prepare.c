/* Build-only inventories for host checks, over the ordinary Make rules. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "host_prepare.h"

struct inventory {
    struct hp_check *checks;
    size_t count;
};

int hp_windows_host(void)
{
    const char *target = getenv("HOST_TEST_PLATFORM");
    if (target)
        return !strcmp(target, "windows");
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}

static int member(const char *words, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = words; *p;) {
        size_t length = strcspn(p, " ");
        if (length == n && !strncmp(p, word, n))
            return 1;
        p += length;
        if (*p)
            p++;
    }
    return 0;
}

static void append_words(struct hp_words *to, const struct hp_words *from)
{
    for (size_t i = 0; i < from->n; i++)
        hp_add(to, from->v[i]);
}

static void append_sources(struct hp_words *to, const struct hp_words *from)
{
    for (size_t i = 0; i < from->n; i++) {
        int found = 0;
        for (size_t j = 0; j < to->n; j++)
            if (!strcmp(to->v[j], from->v[i]))
                found = 1;
        if (!found)
            hp_add(to, from->v[i]);
    }
}

static int suffix(const char *text, const char *end)
{
    size_t a = strlen(text), b = strlen(end);
    return a >= b && !strcmp(text + a - b, end);
}

static struct hp_check *new_check(struct inventory *inventory, const char *id, const char *label,
                                  const char *program, int alone)
{
    struct hp_check *checks = realloc(inventory->checks, (inventory->count + 1) * sizeof(*checks));
    if (!checks)
        exit(1);
    inventory->checks = checks;
    struct hp_check *check = &checks[inventory->count++];
    memset(check, 0, sizeof(*check));
    check->entry = hp_value(JSON_OBJECT);
    hp_member(check->entry, "assertion_id", hp_string(id));
    hp_member(check->entry, "label", hp_string(label));
    hp_member(check->entry, "mode", hp_string(alone ? "alone" : "shared"));
    hp_member(check->entry, "program", hp_string(program));
    hp_member(check->entry, "arguments", hp_value(JSON_ARRAY));
    hp_member(check->entry, "inputs", hp_value(JSON_ARRAY));
    hp_member(check->entry, "sha256", hp_value(JSON_NULL));
    hp_member(check->entry, "unsupported", hp_value(JSON_NULL));
    return check;
}

/* Source metadata lists its files once, beside the check they belong to. */
static int metadata(const char *text, const char *tag, struct hp_words *words)
{
    char key[64];
    if (snprintf(key, sizeof(key), "%s:", tag) >= (int)sizeof(key))
        return -1;
    const char *p = text;
    while ((p = strstr(p, key))) {
        p += strlen(key);
        size_t n = strcspn(p, "\n*");
        char *line = malloc(n + 1);
        if (!line)
            return -1;
        memcpy(line, p, n);
        line[n] = 0;
        int bad = hp_split(words, line);
        free(line);
        if (bad)
            return -1;
        const char *next = strchr(p, '\n');
        if (next) {
            next++;
            while (*next == ' ' || *next == '\t')
                next++;
            if (*next++ == '*') {
                while (*next == ' ' || *next == '\t')
                    next++;
                size_t length = strcspn(next, " \t\r\n");
                if (length > 2 && next[length - 2] == '.' &&
                    (next[length - 1] == 'c' || next[length - 1] == 'h'))
                    return -1;
            }
        }
        p += n;
    }
    return 0;
}

static void architecture_words(struct hp_words *words, const char *arch)
{
    if (strcmp(arch, "x86_64"))
        return;
    for (size_t i = 0; i < words->n; i++)
        if (!strncmp(words->v[i], "vendor/", 7)) {
            struct jbuf mapped = {0};
            jbuf_puts(&mapped, "build/x86_64/vendor-work/");
            jbuf_puts(&mapped, words->v[i] + 7);
            free(words->v[i]);
            words->v[i] = mapped.data;
        }
}

static void unique_sources(struct hp_words *sources)
{
    struct hp_words unique = {0};
    append_sources(&unique, sources);
    hp_words_free(sources);
    *sources = unique;
}

void hp_common_flags(struct hp_words *flags)
{
    hp_split(flags, "-std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0");
    if (hp_windows_host())
        hp_split(flags, "-g -O1 -D_WIN32_WINNT=0x0A00 -static");
    else
        hp_split(flags, "-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1");
}

void hp_windows_flags(struct hp_words *flags)
{
    hp_split(flags, "-std=c99 -O2 -D_DEFAULT_SOURCE -D_WIN32_WINNT=0x0A00 -static "
                    "-Itools/etude/include -Itools/etude -Itools/etude/win -Iutils "
                    "-Itests/host/compat");
}

int hp_source_list(struct hp_context *c, struct hp_words *sources, const char *name)
{
    char option[128], output[4096], error[4096];
    if (hp_path(option, sizeof(option), "HOST_CHECK_SOURCE_LIST=%s", name) ||
        hp_work_files(c, "sources", output, error))
        return -1;
    char *argv[] = {getenv("MAKE") ? getenv("MAKE") : "make",
                    "--no-print-directory",
                    "-f",
                    "mk/host-check.mk",
                    option,
                    "print-sources",
                    NULL};
    if (hp_run(c, argv, output, error))
        return -1;
    char *text = hp_read(output);
    int result = text ? hp_split(sources, text) : -1;
    free(text);
    return result;
}

static int windows_check(struct hp_context *c, struct hp_check *check, const char *id)
{
    struct hp_words sources = {0}, flags = {0};
    int platform = !strcmp(id, "etude_platform");
    if (hp_source_list(c, &sources, platform ? "ETUDE_WIN_FIXTURE_SRC" : "ETUDE_WIN_EXEC_SRC"))
        return -1;
    hp_windows_flags(&flags);
    if (!platform) {
        hp_add(&flags, "-DRACH2_HOSTTEST");
        struct hp_words metadata_flags = check->flags;
        memset(&check->flags, 0, sizeof(check->flags));
        append_words(&flags, &metadata_flags);
        hp_words_free(&metadata_flags);
        struct hp_words replaced = {0};
        for (size_t i = 0; i < check->sources.n; i++) {
            const char *path = check->sources.v[i];
            if (member("tools/etude/posix/exec.c tools/etude/posix/keeper.c "
                       "tools/etude/posix/process.c tools/etude/capture.c",
                       path))
                continue;
            if (!strcmp(id, "etude_scenario") &&
                member("tools/etude/guest/guest.c tools/etude/guest/fixture.c "
                       "tools/etude/guest/owner.c",
                       path))
                continue;
            hp_add(&replaced,
                   !strcmp(path, "tools/etude/posix/bridge.c") ? "tools/etude/win/bridge.c" : path);
        }
        if (!strcmp(id, "etude_scenario"))
            hp_add(&replaced, "tests/host/compat/scenario_guest.c");
        append_sources(&replaced, &sources);
        hp_words_free(&sources);
        sources = replaced;
    } else {
        hp_member(check->entry, "mode", hp_string("alone"));
        hp_member(check->entry, "label",
                  hp_string("native Windows execution and platform contracts"));
        hp_words_free(&check->flags);
    }
    hp_split(&check->libraries, "-lws2_32 -lbcrypt");
    hp_words_free(&check->sources);
    check->sources = sources;
    check->flags = flags;
    hp_split(&check->data, "tools/etude/sources.mk");
    return 0;
}

static int word_order(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int names(struct hp_words *words, const char *directory, const char *end)
{
    DIR *dir = opendir(directory);
    if (!dir)
        return -1;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (suffix(entry->d_name, end))
            hp_add(words, entry->d_name);
    closedir(dir);
    if (words->n > 1)
        qsort(words->v, words->n, sizeof(*words->v), word_order);
    return 0;
}

static int boot_inputs(struct hp_context *c, struct hp_check *check, const char *id)
{
    char path[4096];
    const char *build = !strcmp(c->arch, "x86_64") ? "build/x86_64" : "build";
    if (!member("etude_legacyset etude_qemu etude_scenario etude_watchdog", id))
        return 0;
    snprintf(path, sizeof(path), "%s/rach2/kernel", build);
    hp_add(&check->data, path);
    snprintf(path, sizeof(path), "%s/images/test.krffs", build);
    hp_add(&check->data, path);
    if (!strcmp(id, "etude_scenario")) {
        snprintf(path, sizeof(path), "%s/images/host-etude-scenario.disk", build);
        hp_add(&check->data, path);
        snprintf(path, sizeof(path), "%s/tests/hello", build);
        hp_add(&check->data, path);
    }
    if (strcmp(id, "etude_legacyset"))
        return 0;
    hp_add(&check->data, "build/tools/etude/etude");
    snprintf(path, sizeof(path), "%s/tools/etude/guest/etude-guest", build);
    hp_add(&check->data, path);
    snprintf(path, sizeof(path), "%s/tests/rngprobe", build);
    hp_add(&check->data, path);
    struct hp_words schemas = {0};
    if (names(&schemas, "tools/etude/schemas", ".schema.json"))
        return -1;
    for (size_t i = 0; i < schemas.n; i++) {
        snprintf(path, sizeof(path), "tools/etude/schemas/%s", schemas.v[i]);
        hp_add(&check->data, path);
        snprintf(path, sizeof(path), "build/tests/host-suite/schemas/%s", schemas.v[i]);
        hp_add(&check->data, path);
    }
    hp_words_free(&schemas);
    return 0;
}

static int host_checks(struct hp_context *c, struct inventory *inventory, const char *base)
{
    struct hp_words files = {0};
    int bad = names(&files, "tests/host", "_hosttest.c");
    for (size_t i = 0; !bad && i < files.n; i++) {
        char path[4096], program[4096], id[256];
        char *text =
            !hp_path(path, sizeof(path), "tests/host/%s", files.v[i]) ? hp_read(path) : NULL;
        if (!text) {
            bad = 1;
            break;
        }
        const char *group = strstr(text, "host-arch:")          ? "boots"
                            : !strncmp(files.v[i], "etude_", 6) ? "etude"
                                                                : "tests";
        if (strcmp(group, c->group)) {
            free(text);
            continue;
        }
        size_t n = strlen(files.v[i]) - strlen("_hosttest.c");
        if (n >= sizeof(id)) {
            free(text);
            bad = 1;
            break;
        }
        memcpy(id, files.v[i], n);
        id[n] = 0;
        char label[256];
        int windows = hp_windows_host() && !strcmp(id, "etude_platform");
        if ((windows ? hp_path(program, sizeof(program), "%s/platform/winfixture.exe", base)
                     : hp_path(program, sizeof(program), "%s/%.*s", base,
                               (int)strlen(files.v[i]) - 2, files.v[i])) ||
            hp_path(label, sizeof(label), "%s_hosttest", id)) {
            free(text);
            bad = 1;
            break;
        }
        struct hp_check *check =
            new_check(inventory, id, label, program, strstr(text, "host-alone:") != NULL);
        check->first = strstr(text, "host-first:") != NULL;
        hp_add(&check->sources, path);
        bad = metadata(text, "host-deps", &check->sources) ||
              metadata(text, "host-tools", &check->tools) ||
              metadata(text, "host-inputs", &check->data);
        struct hp_words flags = {0};
        bad = bad || metadata(text, "host-cflags", &flags);
        if (hp_windows_host() && member("etude_platform etude_context etude_observe etude_qemu "
                                        "etude_scenario etude_watchdog etude_legacyset",
                                        id)) {
            check->flags = flags;
            bad = bad || windows_check(c, check, id);
        } else {
            hp_common_flags(&check->flags);
            hp_split(&check->flags, "-DRACH2_HOSTTEST -Wno-empty-body -ffreestanding");
            if (!strcmp(group, "tests"))
                hp_split(&check->flags, "-iquote rach2/sys -iquote rach2/sys/arch/x86 "
                                        "-iquote rach2/sys/arch/i686 -iquote rach2/sys/lib "
                                        "-iquote rach2/sys/proc");
            hp_split(&check->flags, "-I utils -I krffs -I tests/host/compat");
            for (size_t j = 0; j < flags.n; j++)
                hp_add(&check->flags, flags.v[j]);
            hp_words_free(&flags);
        }
        architecture_words(&check->sources, c->arch);
        architecture_words(&check->flags, c->arch);
        architecture_words(&check->data, c->arch);
        architecture_words(&check->tools, c->arch);
        unique_sources(&check->sources);
        if (!strcmp(group, "boots"))
            bad = bad || boot_inputs(c, check, id);
        free(text);
    }
    hp_words_free(&files);
    return bad ? -1 : 0;
}

/* Ignore comments, retaining strings, as the declared backend checks do. */
static void uncomment(const char *text, struct jbuf *code)
{
    int quote = 0;
    for (const char *p = text; *p; p++) {
        if (!quote && p[0] == '/' && (p[1] == '/' || p[1] == '*')) {
            int block = p[1] == '*';
            p += 2;
            while (*p && (block ? !(p[0] == '*' && p[1] == '/') : *p != '\n'))
                p++;
            if (!*p)
                break;
            if (block)
                p++;
            jbuf_puts(code, " ");
        } else {
            jbuf_put(code, p, 1);
            if (*p == '\\' && quote && p[1]) {
                jbuf_put(code, ++p, 1);
            } else if (*p == quote)
                quote = 0;
            else if (!quote && (*p == '\'' || *p == '"'))
                quote = *p;
        }
    }
}

static int interpreter_call(const char *code)
{
    int found = code && strstr(code, "ETUDE_SHELL");
    for (const char *p = code; p && *p && !found; p++) {
        if ((p == code || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) &&
            (!strncmp(p, "system", 6) || !strncmp(p, "popen", 5))) {
            const char *after = p + (*p == 's' ? 6 : 5);
            while (isspace((unsigned char)*after))
                after++;
            found = *after == '(';
        }
    }
    return found;
}

static int interpreter_string(const char *code, const char *path)
{
    int found = 0;
    for (const char *p = code; p && *p && !found; p++) {
        if (*p != '\'' && *p != '"')
            continue;
        char delimiter = *p++;
        const char *end = p;
        while (*end && *end != delimiter) {
            if (*end == '\\' && end[1])
                end++;
            end++;
        }
        if (!*end)
            break;
        char *word = malloc((size_t)(end - p) + 1);
        if (!word) {
            found = 1;
            break;
        }
        memcpy(word, p, (size_t)(end - p));
        word[end - p] = 0;
        const char *program = !strncmp(word, "/bin/", 5) ? word + 5 : word;
        found = member("sh bash python python2 python3 perl", program);
        if (!member("tests/host/etude_collection_hosttest.c "
                    "tests/host/etude_hostdriver_hosttest.c",
                    path) &&
            strstr(word, "#!"))
            found = found || strstr(word, "sh") || strstr(word, "python") || strstr(word, "perl");
        free(word);
        p = end;
    }
    return found;
}

static int interpreter(const char *text, const char *path)
{
    struct jbuf code = {0};
    uncomment(text, &code);
    int found = interpreter_call(code.data) || interpreter_string(code.data, path);
    jbuf_free(&code);
    return found;
}

static const char *backend(struct hp_context *c, struct hp_check *check)
{
    const char *id = json_text(check->entry, "assertion_id");
    if (member("etude_qemu etude_scenario etude_watchdog etude_legacyset etude_platform "
               "etude_context etude_observe",
               id) ||
        (!c->gnu && !strcmp(id, "usbstamp")))
        return "native";
    if (c->gnu || hp_windows_host() ||
        member("ext2 fat32 etude_volume etude_compare etude_step etude_native doom_platform", id))
        return "gnu";
    for (size_t i = 0; i < check->sources.n; i++) {
        if (!suffix(check->sources.v[i], ".c"))
            continue;
        char *text = hp_read(check->sources.v[i]);
        int needs = text && interpreter(text, check->sources.v[i]);
        free(text);
        if (needs)
            return "gnu";
    }
    return "native";
}

static int marker_source(const char *line, const struct hp_words *resolved)
{
    const char *at = line + 1;
    while (*at == ' ' || *at == '\t')
        at++;
    if (!isdigit((unsigned char)*at))
        return -2;
    struct hp_words words = {0};
    int found = -1;
    if (!hp_split(&words, at) && words.n >= 2) {
        char *path = realpath(words.v[1], NULL);
        if (path) {
            for (size_t i = 0; i < resolved->n; i++)
                if (!strcmp(path, resolved->v[i]))
                    found = (int)i;
        }
        free(path);
    }
    hp_words_free(&words);
    return found;
}

static int source_chunks(struct hp_check *check, const char *program,
                         const struct hp_words *resolved, struct jbuf *chunks)
{
    int bad = 0;
    for (size_t i = 0; !bad && i < check->sources.n; i++) {
        char path[4096];
        if (!suffix(check->sources.v[i], ".c"))
            continue;
        int n = snprintf(path, sizeof(path), "%s-sources/scan.%ld/%s.i", program, (long)getpid(),
                         check->sources.v[i]);
        char *text = n > 0 && n < (int)sizeof(path) ? hp_read(path) : NULL;
        if (!text) {
            bad = 1;
            break;
        }
        int active = -1;
        for (char *line = text; line;) {
            char *next = strchr(line, '\n');
            if (next)
                *next++ = 0;
            int marker = *line == '#' ? marker_source(line, resolved) : -2;
            if (marker >= -1)
                active = marker;
            else if (active >= 0) {
                jbuf_puts(&chunks[active], line);
                jbuf_puts(&chunks[active], "\n");
            }
            line = next;
        }
        free(text);
    }
    return bad;
}

static int native_guard(struct hp_context *c, struct hp_check *check, const char *program)
{
    if (c->gnu || c->print ||
        (hp_windows_host() && !strcmp(json_text(check->entry, "assertion_id"), "etude_platform")))
        return 0;
    struct hp_words candidates = {0}, resolved = {0};
    const struct json *inputs = json_member(check->entry, "inputs");
    int bad = 0;
    for (size_t i = 0; inputs && !bad && i < inputs->count; i++) {
        char decoded[4096];
        const char *path = hp_input_path(inputs->item[i], decoded);
        if (!path || !suffix(path, ".c") ||
            (hp_windows_host() && !strcmp(path, "tools/etude/win/files.c")))
            continue;
        char *text = hp_read(path);
        int needs = text && interpreter(text, path);
        free(text);
        if (!needs)
            continue;
        char *absolute = realpath(path, NULL);
        if (!absolute) {
            bad = 1;
            break;
        }
        hp_add(&candidates, path);
        hp_add(&resolved, absolute);
        free(absolute);
    }
    if (!bad && candidates.n) {
        bad = hp_preprocess(c, check, program);
        struct jbuf *chunks = calloc(candidates.n, sizeof(*chunks));
        if (!chunks)
            bad = 1;
        if (!bad)
            bad = source_chunks(check, program, &resolved, chunks);
        for (size_t i = 0; chunks && i < candidates.n; i++) {
            if (!bad && chunks[i].data && interpreter(chunks[i].data, candidates.v[i])) {
                struct jbuf why = {0};
                jbuf_puts(&why, "a compiled source still requires an interpreter; "
                                "a native fixture adapter is required: ");
                jbuf_puts(&why, candidates.v[i]);
                hp_member(check->entry, "unsupported", hp_string(why.data));
                jbuf_free(&why);
                bad = 2;
            }
            jbuf_free(&chunks[i]);
        }
        free(chunks);
    }
    hp_words_free(&candidates);
    hp_words_free(&resolved);
    return bad == 1 ? -1 : 0;
}

static int check_order(const void *a, const void *b)
{
    const struct hp_check *x = a, *y = b;
    int alone = strcmp(json_text(x->entry, "mode"), "alone") == 0;
    int other = strcmp(json_text(y->entry, "mode"), "alone") == 0;
    if (alone != other)
        return alone - other;
    if (x->first != y->first)
        return y->first - x->first;
    return strcmp(json_text(x->entry, "assertion_id"), json_text(y->entry, "assertion_id"));
}

int hp_files(struct hp_words *files, const char *root)
{
    DIR *directory = opendir(root);
    struct dirent *entry;
    if (!directory)
        return -1;
    int bad = 0;
    while (!bad && (entry = readdir(directory))) {
        char path[4096];
        struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        int n = snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        if (n < 0 || n >= (int)sizeof(path) || stat(path, &st)) {
            bad = 1;
            break;
        }
        if (S_ISDIR(st.st_mode))
            bad = hp_files(files, path);
        else if (S_ISREG(st.st_mode))
            hp_add(files, path);
    }
    closedir(directory);
    return bad ? -1 : 0;
}

static int extra_checks(struct hp_context *c, struct inventory *inventory, const char *base)
{
    if (!strcmp(c->group, "checkers")) {
        static const char *const sources[] = {
            "tools/inspect/elfcheck.c", "tools/inspect/cfgcheck.c", "tools/inspect/setidcheck.c",
            "tools/verify/prosecheck.c", "tools/etude/win/pecheck.c"};
        for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
            char id[128], program[4096], label[256];
            const char *name = strrchr(sources[i], '/') + 1;
            if (hp_path(id, sizeof(id), "%.*s", (int)strlen(name) - 2, name) ||
                hp_path(program, sizeof(program), "%s/%s_selftest", base, id) ||
                hp_path(label, sizeof(label), "%s selftest under the sanitizers", id))
                return -1;
            struct hp_check *check = new_check(inventory, id, label, program, 0);
            hp_add(&check->sources, sources[i]);
            hp_common_flags(&check->flags);
            hp_split(&check->flags, "-Wall -Wextra -Werror");
            struct json *arguments = hp_value(JSON_ARRAY);
            hp_item(arguments, hp_string("--selftest"));
            hp_member(check->entry, "arguments", arguments);
            if (!strcmp(id, "prosecheck"))
                hp_split(&check->data, "CLAUDE.md AGENTS.md");
            if (!strcmp(id, "setidcheck")) {
                hp_split(&check->tools, "mke2fs debugfs");
                hp_split(&check->data, "tools/inspect/setid.allow tools/setup/e2fsprogs_path.sh");
            }
        }
        return 0;
    }
    struct hp_words files = {0};
    int bad = names(&files, "tests/fuzz", "_fuzz.c");
    for (size_t i = 0; !bad && i < files.n; i++) {
        char source[4096], id[256], program[4096], corpus[4096], label[512];
        if (hp_path(id, sizeof(id), "%.*s", (int)strlen(files.v[i]) - 7, files.v[i]) ||
            hp_path(source, sizeof(source), "tests/fuzz/%s", files.v[i]) ||
            hp_path(program, sizeof(program), "%s/%s", base, id) ||
            hp_path(corpus, sizeof(corpus), "tests/fuzz/corpus/%s", id) ||
            hp_path(label, sizeof(label), "%s corpus replayed", id)) {
            bad = 1;
            break;
        }
        struct hp_check *check = new_check(inventory, id, label, program, 0);
        hp_add(&check->sources, source);
        char *text = hp_read(source);
        struct hp_words flags = {0}, inputs = {0};
        bad = !text || metadata(text, "fuzz-deps", &check->sources) ||
              metadata(text, "fuzz-cflags", &flags) || hp_files(&inputs, corpus) || !inputs.n;
        if (inputs.n > 1)
            qsort(inputs.v, inputs.n, sizeof(*inputs.v), word_order);
        hp_member(check->entry, "arguments", hp_array(&inputs));
        hp_member(check->entry, "corpus", hp_string(corpus));
        append_words(&check->data, &inputs);
        hp_common_flags(&check->flags);
        hp_split(&check->flags, "-DRACH2_HOSTTEST -DFIXTURE_NO_MAIN -DFUZZ_REPLAY "
                                "-Wno-empty-body -ffreestanding -iquote rach2/sys "
                                "-iquote rach2/sys/arch/x86 -iquote rach2/sys/arch/i686 "
                                "-iquote rach2/sys/lib -iquote rach2/sys/proc "
                                "-I utils -I krffs -I tests/host/compat");
        append_words(&check->flags, &flags);
        hp_words_free(&flags);
        hp_words_free(&inputs);
        architecture_words(&check->sources, c->arch);
        architecture_words(&check->flags, c->arch);
        unique_sources(&check->sources);
        free(text);
    }
    hp_words_free(&files);
    return bad ? -1 : 0;
}

static void check_free(struct hp_check *check)
{
    json_free(check->entry);
    hp_words_free(&check->sources);
    hp_words_free(&check->flags);
    hp_words_free(&check->libraries);
    hp_words_free(&check->data);
    hp_words_free(&check->tools);
}

static int inactive(struct hp_context *c, struct hp_check *check, const char *route)
{
    hp_member(check->entry, "backend", hp_string(route));
    if ((!c->gnu && strcmp(route, "gnu")) || (c->gnu && strcmp(route, "native")))
        return 0;
    hp_member(check->entry, "program", hp_value(JSON_NULL));
    hp_member(check->entry, "instrumentation", hp_value(JSON_NULL));
    hp_member(check->entry, "command", hp_value(JSON_NULL));
    hp_member(check->entry, "arguments", hp_value(JSON_ARRAY));
    if (c->gnu)
        hp_member(check->entry, "unsupported",
                  hp_string("this assertion executes on the collection host"));
    else {
        struct json *inputs = hp_value(JSON_ARRAY);
        for (size_t i = 0; i < check->sources.n; i++) {
            struct stat st;
            if (!stat(check->sources.v[i], &st) && hp_input(c, check->sources.v[i], inputs, 0)) {
                json_free(inputs);
                return -1;
            }
        }
        hp_member(check->entry, "inputs", inputs);
    }
    return 1;
}

static int instrumentation(struct hp_check *check)
{
    struct json *sanitizers = hp_value(JSON_ARRAY);
    for (size_t i = 0; i < check->flags.n; i++) {
        const char *flag = check->flags.v[i];
        if (!strncmp(flag, "-fsanitize=", 11)) {
            char *copy = strdup(flag + 11), *p = copy;
            while (p) {
                char *next = strchr(p, ',');
                if (next)
                    *next++ = 0;
                hp_item(sanitizers, hp_string(p));
                p = next;
            }
            free(copy);
        }
    }
    int instrumented = sanitizers->count != 0;
    hp_member(check->entry, "instrumentation", sanitizers);
    return instrumented;
}

static int inventories(struct hp_context *c, const char *base)
{
    struct inventory inventory = {0};
    struct json *checks = hp_value(JSON_ARRAY);
    int bad = !strcmp(c->group, "scripts")            ? hp_scripts(c, checks, base)
              : member("tests etude boots", c->group) ? host_checks(c, &inventory, base)
                                                      : extra_checks(c, &inventory, base);
    if (inventory.count > 1)
        qsort(inventory.checks, inventory.count, sizeof(*inventory.checks), check_order);
    for (size_t i = 0; !bad && i < inventory.count; i++) {
        struct hp_check *check = &inventory.checks[i];
        const char *id = json_text(check->entry, "assertion_id");
        if (c->only && strcmp(c->only, id))
            continue;
        int skip = inactive(c, check, backend(c, check));
        if (skip < 0) {
            bad = 1;
            break;
        }
        if (!skip) {
            int instrumented = instrumentation(check);
#ifdef __APPLE__
            if (instrumented) {
                hp_add(&check->tools, "atos");
                hp_member(check->entry, "sanitizer_symbolizer",
                          hp_string("build/tests/host-suite/sanitizer/atos"));
            }
#else
            (void)instrumented;
#endif
            const char *program = json_text(check->entry, "program");
            bad = !program || hp_build(c, check, program, 0) || hp_tools(c, check) ||
                  native_guard(c, check, program);
        }
        if (!bad) {
            hp_item(checks, check->entry);
            check->entry = NULL;
        }
    }
    for (size_t i = 0; i < inventory.count; i++)
        check_free(&inventory.checks[i]);
    free(inventory.checks);
    for (size_t i = 0; !bad && i < checks->count; i++)
        hp_sort_inputs((struct json *)json_member(checks->item[i], "inputs"));
    if (!bad && !c->print && !c->only) {
        char hash[65] = {0};
        struct json *record = hp_value(JSON_OBJECT);
        bad = hp_hash("tests/cases/host.host.json", hash);
        hp_member(record, "schema_version", hp_number(1));
        hp_member(record, "group", hp_string(c->group));
        hp_member(record, "architecture",
                  hp_string(member("tests boots", c->group) ? c->arch : "none"));
        hp_member(record, "manifest_sha256", hp_string(hash));
        hp_member(record, "compiler", hp_clone(c->identity));
        hp_member(record, "checks", checks);
        checks = NULL;
        if (!bad)
            bad = hp_publish(c->output, record);
        json_free(record);
    }
    json_free(checks);
    return bad ? -1 : 0;
}

static int options(struct hp_context *c, int argc, char **argv)
{
    int rest = 0;
    c->arch = getenv("ARCH") ? getenv("ARCH") : "i686";
    for (int i = 1; i < argc; i++) {
        const char *word = argv[i];
        if (!strcmp(word, "--gnu"))
            c->gnu = 1;
        else if (!strcmp(word, "--print-commands"))
            c->print = 1;
        else if (member("--arch --only --previous", word)) {
            if (++i == argc)
                return -1;
            if (!strcmp(word, "--arch"))
                c->arch = argv[i];
            else if (!strcmp(word, "--only"))
                c->only = argv[i];
            else
                c->previous = argv[i];
        } else if (*word == '-' || rest == 2)
            return -1;
        else if (rest++ == 0)
            c->group = word;
        else
            c->output = word;
    }
    return rest != 2 || !member("i686 x86_64", c->arch) ||
                   (c->print && member("style tools", c->group)) ||
                   !member("tests etude boots checkers fuzz scripts style tools", c->group)
               ? -1
               : 0;
}

static int previous_inventory(struct hp_context *c)
{
    int bad = 0;
    c->old = hp_document(c->output);
    if (c->old && !hp_identity_equal(c, json_member(c->old, "compiler"), c->identity)) {
        json_free(c->old);
        c->old = NULL;
    }
    if (!c->old && c->previous) {
        char previous[4096];
        if (!c->gnu || c->output[0] == '/' || strchr(c->output, ':'))
            bad = 1;
        else {
            snprintf(previous, sizeof(previous), "%s/%s", c->previous, c->output);
            c->old = hp_document(previous);
            if (c->old)
                c->origin = c->previous;
        }
    }
    return bad;
}

int etude_host_prepare_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    struct hp_context c;
    char parent[4096], base[4096];
    memset(&c, 0, sizeof(c));
    c.stop = stop;
    if (options(&c, argc, argv) || strlen(c.output) > 1024 || !realpath(".", c.tree)) {
        fprintf(stderr, "usage: etude prepare -H [--arch ARCH] [--gnu] [--only ID] "
                        "[--previous TREE] [--print-commands] GROUP OUTPUT\n");
        return 2;
    }
    snprintf(parent, sizeof(parent), "%s", c.output);
    char *slash = strrchr(parent, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(parent, ".");
    snprintf(base, sizeof(base), "%s/%s", parent, c.group);
    snprintf(c.work, sizeof(c.work), "%s/.%s-prepare.%ld", parent, c.group, (long)getpid());
    int bad = hp_mkdir(parent) || hp_mkdir(base) || hp_mkdir(c.work);
    if (!bad && !strcmp(c.group, "style"))
        bad = hp_style(&c);
    else if (!bad && !strcmp(c.group, "tools"))
        bad = hp_tool_main(&c);
    else if (!bad) {
        const char *cc = getenv("CC");
        bad = hp_split(&c.compiler, cc ? cc : "cc") || !c.compiler.n;
        if (!bad && !c.print)
            bad = hp_compiler(&c);
        if (!bad && !c.print)
            bad = previous_inventory(&c);
        if (!bad)
            bad = inventories(&c, base);
    }
    if (bad)
        fprintf(stderr, "host preparation: %s failed; evidence: %s\n", c.group, c.work);
    hp_words_free(&c.compiler);
    free(c.compiler_requested);
    json_free(c.identity);
    json_free(c.old);
    return bad ? 1 : 0;
}
