/* Exercise the real collection result writer without starting a machine. */
#include "../../../tools/etude/posix/legacy_set.c"
#define fix_dir    capture_fix_dir
#define fix_path   capture_fix_path
#define fix_passed capture_fix_passed
#define fix_format capture_fix_format
#include "fixdir_host.c"

static int capture_failures, capture_tail_count;
#define CAPTURE_CHECK(test)                                                    \
    do {                                                                       \
        if (!(test)) {                                                         \
            fprintf(stderr, "legacy capture: line %d: %s\n", __LINE__, #test); \
            capture_failures++;                                                \
        }                                                                      \
    } while (0)

/* Read the actual stored blob and verify its recorded digest and length. */
static char *capture_blob(const struct eh_json *ref)
{
    const struct eh_json *locations = eh_json_get(ref, "locations");
    const struct eh_json *size = eh_json_get(ref, "size_bytes");
    const char *hex = cmp_text(ref, "sha256");
    char *bytes = NULL, actual[65];
    size_t length = 0;
    struct sha256 state;
    unsigned char digest[32];

    if (locations == NULL || locations->child == NULL || size == NULL || hex == NULL ||
        cmp_read_file(locations->child->string, EH_MAX_LINE, &bytes, &length)) {
        CAPTURE_CHECK(0);
        return NULL;
    }
    sha256_init(&state);
    sha256_update(&state, (const unsigned char *)bytes, length);
    sha256_final(&state, digest);
    sha256_hex(digest, actual);
    CAPTURE_CHECK(size->type == 'n' && size->number == (int64_t)length && !strcmp(hex, actual));
    return bytes;
}

static void capture_clear(struct command *c)
{
    free(c->captures.data);
    free(c->capture_errors.data);
    memset(c, 0, sizeof(*c));
}

/* Keep the existing output-tail evidence before the named QMP document. */
static const struct eh_json *capture_reference(const struct eh_json *evidence,
                                               const struct command *c)
{
    const struct eh_json *ref = evidence->child;
    const char *tails[] = {"retained output\n", "retained error\n"};
    for (int i = 0; c->ran && c->outcome != PASS && i < capture_tail_count; i++) {
        char *tail = capture_blob(ref);
        CAPTURE_CHECK(tail != NULL && !strcmp(tail, tails[i]));
        free(tail);
        ref = ref != NULL ? ref->next : NULL;
    }
    CAPTURE_CHECK(ref != NULL && ref->next == NULL);
    return ref;
}

static void capture_tail(struct command *c, int error_mode)
{
    const char *tails[] = {"retained output\n", "retained error\n"};
    snprintf(c->out, sizeof(c->out), "%s", capture_fix_path("output"));
    snprintf(c->err, sizeof(c->err), "%s", capture_fix_path(error_mode < 0 ? "absent" : "error"));
    const char *paths[] = {c->out, c->err};
    capture_tail_count = error_mode > 0 ? 2 : 1;
    for (int i = 0; i < (error_mode < 0 ? 1 : 2); i++) {
        FILE *file = fopen(paths[i], "wb");
        CAPTURE_CHECK(file != NULL);
        if (file != NULL) {
            if (i < capture_tail_count)
                CAPTURE_CHECK(fputs(tails[i], file) >= 0);
            CAPTURE_CHECK(fclose(file) == 0);
        }
    }
    c->ran = 1;
    c->outcome = INCOMPLETE;
}

static void capture_check_result(const struct set *x, const struct command *c,
                                 const char *const expected[], const char *error)
{
    const char *names[] = {"query-status", "query-cpus-fast", "info registers -a"};
    struct cmp_text text = {0};
    struct eh_json *record, *doc = NULL;
    const struct eh_json *evidence, *errors;
    char *bytes = NULL;

    CAPTURE_CHECK(result(x, c, &text) == 0);
    record = text.data ? eh_json_parse(text.data) : NULL;
    CAPTURE_CHECK(record != NULL);
    evidence = eh_json_get(record, "evidence");
    errors = eh_json_get(record, "capture_errors");
    CAPTURE_CHECK(evidence != NULL && errors != NULL);
    if (expected != NULL && evidence != NULL) {
        bytes = capture_blob(capture_reference(evidence, c));
        doc = bytes ? eh_json_parse(bytes) : NULL;
        CAPTURE_CHECK(doc != NULL);
        for (size_t i = 0; doc != NULL && i < 3; i++) {
            const struct eh_json *ref = eh_json_get(doc, names[i]);
            CAPTURE_CHECK(ref != NULL);
            if (expected[i] == NULL)
                CAPTURE_CHECK(ref != NULL && ref->type == 'z');
            else {
                char *raw = capture_blob(ref);
                CAPTURE_CHECK(raw != NULL && !strcmp(raw, expected[i]));
                free(raw);
            }
        }
    } else if (evidence != NULL)
        CAPTURE_CHECK(evidence->child == NULL);
    if (error != NULL)
        CAPTURE_CHECK(errors != NULL && errors->child != NULL && errors->child->next == NULL &&
                      errors->child->type == 's' && !strcmp(errors->child->string, error));
    else
        CAPTURE_CHECK(errors != NULL && errors->child == NULL);
    free(bytes);
    eh_json_free(doc);
    eh_json_free(record);
    free(text.data);
}

int etude_test_legacy_captures(void)
{
    const char *expected[] = {"{\"return\":{\"status\":\"running\"},\"id\":1}\r\n",
                              "{\"return\":[{\"cpu-index\":0}],\"id\":2}\n",
                              "{\"return\":\"EIP=00123456\\nEAX=00000001\\n\",\"id\":3}\n"};
    const char *error = "query-cpus-fast: QMP could not take or answer it";
    struct etude_supervised_result r = {0};
    struct command c = {0};
    struct set x = {0};
    struct cmp_text text = {0};

    snprintf(x.store, sizeof(x.store), "%s", capture_fix_dir());
    r.end = ETUDE_END_STOPPED;
    r.cleanup = ETUDE_CLEANUP_CONFIRMED;
    took(&x, &c, &r);
    capture_check_result(&x, &c, NULL, NULL);
    capture_clear(&c);
    for (int error_mode = -1; error_mode <= 1; error_mode++) {
        r.qmp_status = strdup(expected[0]);
        r.qmp_cpus = strdup(expected[1]);
        r.qmp_registers = strdup(expected[2]);
        CAPTURE_CHECK(r.qmp_status && r.qmp_cpus && r.qmp_registers);
        took(&x, &c, &r);
        capture_tail(&c, error_mode);
        free(r.qmp_status);
        free(r.qmp_cpus);
        free(r.qmp_registers);
        memset(&r, 0, sizeof(r));
        capture_check_result(&x, &c, expected, NULL);
        capture_clear(&c);
    }
    expected[1] = NULL;
    r.qmp_status = (char *)expected[0];
    r.qmp_registers = (char *)expected[2];
    snprintf(r.capture_error, sizeof(r.capture_error), "%s", error);
    took(&x, &c, &r);
    memset(&r, 0, sizeof(r));
    capture_check_result(&x, &c, expected, error);
    capture_clear(&c);
    snprintf(r.capture_error, sizeof(r.capture_error), "%s", error);
    took(&x, &c, &r);
    capture_check_result(&x, &c, NULL, error);
    capture_clear(&c);
    snprintf(x.store, sizeof(x.store), "%s", capture_fix_path("absent/store"));
    r.qmp_status = (char *)expected[0];
    took(&x, &c, &r);
    CAPTURE_CHECK(c.captures.failed && result(&x, &c, &text) != 0);
    capture_clear(&c);
    free(text.data);
    if (!capture_failures)
        capture_fix_passed();
    return capture_failures != 0;
}
