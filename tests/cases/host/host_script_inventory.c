/* Classify every host.list check and validate all partition declarations. */
#include "host_script_inventory.h"
#include "history_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *kernel[] = {"chown",     "krffs_open", "krffs_root_lock", "krffs_stat",
                               "pagecache", "kstack",     "endothers",       NULL};

static const char *image[] = {"mkext2_overlay", "krffs_tools",      "install_accounts",
                              "tooling_image",  "collection_tools", NULL};

static const char *build[] = {"doom_libc",       "run_scripts",    "commit_msg",  "prereq",
                              "check_asm",       "check_ldscript", "together",    "gnu_cmake_jobs",
                              "gnu_port_resume", "host_suite",     "case_select", NULL};

static const char *flow[] = {"machine_measure",
                             "ci",
                             "ci_run",
                             "board",
                             "progress",
                             "limited",
                             "worker_park",
                             "release_resume",
                             "tidy",
                             "leftovers",
                             "agents_guest",
                             "testing_run",
                             "space",
                             "etude_version",
                             "etude_deploy",
                             "etude_launch",
                             "test_status",
                             "etude_tooling",
                             "agents_credentials",
                             NULL};

static const char *tools[] = {"etude_version",
                              "etude_deploy",
                              "etude_launch",
                              "test_status",
                              "etude_tooling",
                              "agents_credentials",
                              NULL};
static const char *parts[HOST_SCRIPT_PARTS] = {
    "host.host.scripts_native", "host.host.scripts_images", "host.host.scripts_build",
    "host.host.scripts", "host.host.scripts_tools"};

const char *host_script_case(unsigned part)
{
    return part < HOST_SCRIPT_PARTS ? parts[part] : NULL;
}

int host_script_case_part(const char *id)
{
    for (unsigned i = 0; id && i < HOST_SCRIPT_PARTS; i++)
        if (!strcmp(id, parts[i]))
            return (int)i;
    return -1;
}

static int in(const char *const *list, const char *id)
{
    while (*list)
        if (!strcmp(*list++, id))
            return 1;
    return 0;
}

static int classify(struct host_script_row *row)
{
    const char *id = row->id;
    if (in(kernel, id)) {
        row->kind = SCRIPT_KERNEL;
        row->part = SCRIPT_NATIVE_PART;
    } else if (!strcmp(id, "anthropic") || !strcmp(id, "gemini") || !strcmp(id, "fakeapi")) {
        row->kind = SCRIPT_PROTOCOL;
        row->part = SCRIPT_NATIVE_PART;
    } else if (in(image, id)) {
        row->kind = SCRIPT_IMAGE;
        row->part = SCRIPT_IMAGE_PART;
    } else if (in(build, id)) {
        row->kind = SCRIPT_BUILD;
        row->part = SCRIPT_BUILD_PART;
    } else if (in(flow, id)) {
        row->kind = SCRIPT_FLOW;
        row->part = in(tools, id) ? SCRIPT_TOOLS_PART : SCRIPT_FLOW_PART;
    } else {
        return 0;
    }
    return 1;
}

static int list_rows(const char *path, struct host_script_row rows[HOST_SCRIPT_ROWS], size_t *count)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    char line[4096];
    size_t n = 0;
    int ok = 1;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *at = line + strspn(line, " \t\r\n"), *end;
        if (!*at || *at == '#')
            continue;
        end = strchr(at, '|');
        if (!end || end == at || (size_t)(end - at) >= sizeof(rows[0].id) ||
            n == HOST_SCRIPT_ROWS) {
            ok = 0;
            break;
        }
        *end = 0;
        for (size_t i = 0; i < n; i++)
            if (!strcmp(rows[i].id, at))
                ok = 0;
        strcpy(rows[n].id, at);
        if (!classify(&rows[n++]))
            ok = 0;
    }
    ok &= !ferror(f);
    fclose(f);
    *count = n;
    return ok && n == HOST_SCRIPT_ROWS;
}

static struct eh_json *document(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    struct eh_json *doc = NULL;
    char *bytes = NULL;
    long length;
    if (f && !fseek(f, 0, SEEK_END) && (length = ftell(f)) > 0 && (size_t)length <= EH_MAX_LINE &&
        !fseek(f, 0, SEEK_SET) && (bytes = malloc((size_t)length + 1)) != NULL &&
        fread(bytes, 1, (size_t)length, f) == (size_t)length && !ferror(f)) {
        bytes[length] = 0;
        doc = eh_json_parse(bytes);
    }
    free(bytes);
    if (f)
        fclose(f);
    return doc;
}

static const char *string(const struct eh_json *value, const char *key)
{
    const struct eh_json *field = eh_json_get(value, key);
    return field && field->type == 's' ? field->string : NULL;
}

static int declarations(const struct eh_json *kase, int part, const struct host_script_row *rows,
                        unsigned seen[HOST_SCRIPT_ROWS])
{
    const struct eh_json *array = eh_json_get(kase, "assertions");
    unsigned listed = 0;
    if (!array || array->type != 'a')
        return 0;
    for (const struct eh_json *a = array->child; a; a = a->next) {
        const char *id = string(a, "assertion_id");
        size_t i;
        if (!id)
            return 0;
        if (!strcmp(id, "listed")) {
            if (listed++)
                return 0;
            continue;
        }
        for (i = 0; i < HOST_SCRIPT_ROWS && strcmp(rows[i].id, id); i++) {
        }
        if (i == HOST_SCRIPT_ROWS || (int)rows[i].part != part || seen[i]++)
            return 0;
    }
    return listed == 1;
}

int host_script_inventory(const char *list, const char *manifest,
                          struct host_script_row rows[HOST_SCRIPT_ROWS], size_t *count)
{
    unsigned seen[HOST_SCRIPT_ROWS] = {0}, parts_seen[HOST_SCRIPT_PARTS] = {0};
    struct eh_json *doc;
    const struct eh_json *cases;
    int ok = list_rows(list, rows, count);
    if (!ok)
        return 0;
    doc = document(manifest);
    cases = eh_json_get(doc, "cases");
    ok = cases && cases->type == 'a';
    for (const struct eh_json *k = ok ? cases->child : NULL; k && ok; k = k->next) {
        const char *id = string(k, "case_id");
        int part = host_script_case_part(id);
        if (part >= 0)
            ok = !parts_seen[part]++ && declarations(k, part, rows, seen);
        else if (id && !strncmp(id, "host.host.scripts", 17))
            ok = 0;
    }
    for (size_t i = 0; i < HOST_SCRIPT_ROWS; i++)
        ok &= seen[i] == 1;
    for (size_t i = 0; i < HOST_SCRIPT_PARTS; i++)
        ok &= parts_seen[i] == 1;
    eh_json_free(doc);
    return ok;
}
