/* Revalidate live issue protection before removing any registered evidence. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "retention.h"
#include "../../utils/sha256.h"

/* Match the existing Python snapshot digest: sorted keys, ASCII and spaces. */
void ret_python_json(const struct json *value, struct jbuf *out)
{
    struct jbuf canonical = {0};
    json_canonical(value, &canonical);
    int quoted = 0, escaped = 0;
    for (size_t i = 0; i < canonical.len; i++) {
        unsigned char ch = (unsigned char)canonical.data[i];
        if (ch >= 127) {
            size_t n = ch >= 0xf0 ? 4 : ch >= 0xe0 ? 3 : ch >= 0xc0 ? 2 : 1;
            unsigned code = n == 1 ? ch : ch & (0x7fu >> n);
            for (size_t k = 1; k < n; k++)
                code = (code << 6) | ((unsigned char)canonical.data[i + k] & 63);
            char text[16];
            if (code < 65536)
                snprintf(text, sizeof text, "\\u%04x", code);
            else {
                code -= 65536;
                snprintf(text, sizeof text, "\\u%04x\\u%04x", 0xd800 + (code >> 10),
                         0xdc00 + (code & 1023));
            }
            jbuf_puts(out, text);
            i += n - 1;
            escaped = 0;
            continue;
        }
        jbuf_put(out, canonical.data + i, 1);
        if (!quoted && (ch == ',' || ch == ':'))
            jbuf_puts(out, " ");
        if (ch == '"' && !escaped)
            quoted = !quoted;
        if (quoted && ch == '\\' && !escaped)
            escaped = 1;
        else
            escaped = 0;
    }
    jbuf_free(&canonical);
}

static void issue_digest(const struct json *value, int python, char out[65])
{
    struct jbuf text = {0};
    if (python)
        ret_python_json(value, &text);
    else
        json_canonical(value, &text);
    struct sha256 hash;
    unsigned char digest[32];
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)text.data, text.len);
    sha256_final(&hash, digest);
    sha256_hex(digest, out);
    jbuf_free(&text);
}

static int issue_repository(const char *repository)
{
    if (!repository)
        return 0;
    unsigned slashes = 0, component = 0;
    for (const unsigned char *p = (const unsigned char *)repository; *p; p++) {
        if (*p == '/') {
            if (!component || slashes++)
                return 0;
            component = 0;
        } else if ((isalnum(*p) && *p < 128) || strchr("._-", *p))
            component++;
        else
            return 0;
    }
    return slashes == 1 && component;
}

static struct json *issue_query(const char *endpoint, volatile sig_atomic_t *stop)
{
    char *argv[] = {"gh", "api", "--paginate", "--slurp", (char *)endpoint, NULL}, *bytes = NULL;
    if (ret_execute(argv, NULL, NULL, &bytes, stop, 30000000))
        return NULL;
    struct json *pages = NULL;
    char error[256];
    int rc = json_parse_data(bytes, strlen(bytes), 1, &pages, error, sizeof error);
    free(bytes);
    if (rc || !pages || pages->type != JSON_ARRAY) {
        json_free(pages);
        ret_fail("GitHub issue response is unavailable or malformed");
        return NULL;
    }
    return pages;
}

static struct json *issue_open(const char *repository, volatile sig_atomic_t *stop)
{
    if (!issue_repository(repository)) {
        ret_fail("invalid issue repository");
        return NULL;
    }
    char *endpoint = ret_format("repos/%s/issues?state=open&per_page=100", repository);
    struct json *pages = issue_query(endpoint, stop), *issues = ret_new(JSON_ARRAY);
    free(endpoint);
    int rc = !pages;
    for (size_t i = 0; !rc && i < pages->count; i++) {
        const struct json *page = pages->item[i];
        if (page->type != JSON_ARRAY) {
            rc = -1;
            break;
        }
        for (size_t j = 0; j < page->count; j++) {
            const struct json *item = page->item[j];
            if (item->type != JSON_OBJECT) {
                rc = -1;
                break;
            }
            if (!json_member(item, "pull_request") && ret_push(issues, ret_clone(item)))
                rc = -1;
        }
    }
    json_free(pages);
    if (rc) {
        json_free(issues);
        issues = NULL;
    }
    return issues;
}

static int issue_order(const void *a, const void *b)
{
    const struct json *const *x = a, *const *y = b;
    int64_t one = ret_number(*x, "number"), two = ret_number(*y, "number");
    return (one > two) - (one < two);
}

static int issue_signature(const struct json *issues, char digest[65])
{
    const struct json **ordered = json_alloc((issues->count + 1) * sizeof(*ordered));
    for (size_t i = 0; i < issues->count; i++)
        ordered[i] = issues->item[i];
    qsort(ordered, issues->count, sizeof(*ordered), issue_order);
    struct json *triples = ret_new(JSON_ARRAY);
    int rc = 0;
    int64_t previous = 0;
    for (size_t i = 0; i < issues->count; i++) {
        int64_t number = ret_number(ordered[i], "number"),
                comments = ret_number(ordered[i], "comments");
        const char *updated = json_text(ordered[i], "updated_at");
        if (number <= previous || comments < 0 || !updated) {
            rc = -1;
            break;
        }
        struct json *row = ret_new(JSON_ARRAY);
        ret_push(row, ret_integer(number));
        ret_push(row, ret_string(updated));
        ret_push(row, ret_integer(comments));
        ret_push(triples, row);
        previous = number;
    }
    if (!rc)
        issue_digest(triples, 0, digest);
    free(ordered);
    json_free(triples);
    return rc ? ret_fail("invalid live open-issue set") : 0;
}

int ret_issue_live(const struct json *snapshot, volatile sig_atomic_t *stop)
{
    const char *expected = json_text(snapshot, "issue_set_sha256");
    struct json *issues = issue_open(json_text(snapshot, "repository"), stop);
    char actual[65];
    int rc = !issues || !expected || issue_signature(issues, actual) || strcmp(expected, actual);
    json_free(issues);
    return rc ? ret_fail("open issues changed or cannot be revalidated") : 0;
}

static struct json *issue_comments(const char *repository, int64_t number,
                                   volatile sig_atomic_t *stop)
{
    char *endpoint =
        ret_format("repos/%s/issues/%lld/comments?per_page=100", repository, (long long)number);
    struct json *pages = issue_query(endpoint, stop), *comments = ret_new(JSON_ARRAY);
    free(endpoint);
    int rc = !pages;
    for (size_t i = 0; !rc && i < pages->count; i++) {
        const struct json *page = pages->item[i];
        if (page->type != JSON_ARRAY) {
            rc = -1;
            break;
        }
        for (size_t j = 0; j < page->count; j++)
            if (ret_push(comments, ret_clone(page->item[j])))
                rc = -1;
    }
    json_free(pages);
    if (rc) {
        json_free(comments);
        comments = NULL;
    }
    return comments;
}

int ret_issue_snapshot(const char *repository, const char *mapping, const char *output,
                       volatile sig_atomic_t *stop)
{
    struct json *map = ret_document(mapping), *current = issue_open(repository, stop);
    struct json *snapshot = ret_new(JSON_OBJECT), *issues = ret_new(JSON_ARRAY),
                *unknown = ret_new(JSON_ARRAY);
    char signature[65], captured[64];
    int rc = !map || map->type != JSON_OBJECT || !current || issue_signature(current, signature);
    for (size_t i = 0; !rc && i < current->count; i++) {
        struct json *issue = current->item[i];
        int64_t number = ret_number(issue, "number");
        char name[32];
        snprintf(name, sizeof name, "%lld", (long long)number);
        const struct json *roots = json_member(map, name);
        if (!roots) {
            ret_push(unknown, ret_integer(number));
            continue;
        }
        struct json *comments = issue_comments(repository, number, stop),
                    *proofs = ret_closure(roots);
        if (!comments || !proofs) {
            json_free(comments);
            json_free(proofs);
            rc = -1;
            break;
        }
        json_free(proofs);
        ret_set(issue, "retained_comments", comments);
        char digest[65];
        issue_digest(issue, 1, digest);
        struct json *retained = ret_new(JSON_OBJECT);
        ret_set(retained, "number", ret_integer(number));
        ret_set(retained, "snapshot_sha256", ret_string(digest));
        ret_set(retained, "evidence", ret_clone(roots));
        ret_set(retained, "snapshot", ret_clone(issue));
        ret_push(issues, retained);
    }
    if (!rc) {
        ret_stamp(captured);
        ret_set(snapshot, "format", ret_string("etude-retention-open-issues"));
        ret_set(snapshot, "schema_version", ret_integer(1));
        ret_set(snapshot, "repository", ret_string(repository));
        ret_set(snapshot, "captured_utc", ret_string(captured));
        ret_set(snapshot, "complete", ret_boolean(1));
        ret_set(snapshot, "issue_set_sha256", ret_string(signature));
        ret_set(snapshot, "issues", issues);
        issues = NULL;
        ret_set(snapshot, "unknown", unknown);
        unknown = NULL;
        rc = ret_issue_live(snapshot, stop) || ret_write(output, snapshot, 0);
    }
    if (!rc) {
        struct json *ref = ret_descriptor(output);
        struct jbuf text = {0};
        if (!ref)
            rc = -1;
        else {
            json_canonical(ref, &text);
            printf("%s\n", text.data);
        }
        json_free(ref);
        jbuf_free(&text);
    }
    json_free(map);
    json_free(current);
    json_free(snapshot);
    json_free(issues);
    json_free(unknown);
    return rc;
}
