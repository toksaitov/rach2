/*
 * The rows of etude_legacy_hosttest.c that run `etude prepare import`: the
 * request, the producer's record, receipt and inventories that bind what it
 * imports, the roles and the native builds it makes, and the local, SSH and
 * WSL transports it reads the producer by.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fixdir_host.h"
#include "prepare_fixture.h"

/* The inventory goal that the inventory rows import. */
static const char inventory[] = "build/tests/host-suite/tests.inventory.json";

/* The sanitizer manifest that host preparation writes. */
static const char sanitizer[] = "build/tests/host-suite/sanitizer/tools.json";

/* A fixture of one row, under the row's own directory. */
struct row {
    char work[PF_PATH];
    struct pf_import s;
};

/* Make the row's directory `name` and its import fixture of `goals`, or of
 * the root disk alone when NULL. */
static void row_start(struct row *r, const char *name, const char *const goals[], size_t ngoals,
                      const struct json *checks)
{
    static const char *const root[] = {"build/images/root.disk"};

    pf_scratch(name, r->work);
    if (goals == NULL) {
        goals = root;
        ngoals = 1;
    }
    pf_import_fixture(&r->s, r->work, goals, ngoals, checks);
}

/* Free the row's fixture and remove its directory when every row passed. */
static void row_end(struct row *r)
{
    pf_import_free(&r->s);
    pf_cleanup(r->work);
}

/* Import with the defaults: retained conditioning, the local producer and
 * i686. */
static struct json *run(struct pf_import *s, int expected)
{
    return pf_import_run(s, expected, NULL, NULL);
}

/* The receipt of the producer's metadata, which a row changes. */
static struct json *receipt(struct pf_import *s)
{
    return pf_jget(s->metadata, "receipt");
}

/* Add `text` to the native tree's Makefile. */
static void make_rule(struct pf_import *s, const char *text)
{
    PF_CHECK(!pf_append_text(pf_at(s->tree, "Makefile"), text), "import: a rule added");
}

/* A rule that has etude's host preparation make the sanitizer manifest. */
static void sanitizer_rule(struct pf_import *s)
{
    char rule[3 * PF_PATH];

    fix_format(rule, sizeof(rule),
               ".PHONY: %s\n%s:\n\t@mkdir -p $(@D)\n\t%s prepare -H --only sanitizer tools $(@D)\n",
               sanitizer, sanitizer, pf_etude);
    make_rule(s, rule);
}

/** Check that a request's lines may end in CRLF, and that a goal with a
 * carriage return inside it is refused. */
static void request_lines(void)
{
    struct row r;
    struct json *proof;

    row_start(&r, "request-lines", NULL, 0, NULL);
    r.s.newline = "\r\n";
    proof = run(&r.s, 0);
    r.s.newline = NULL;
    json_free(proof);
    free(r.s.goals[0]);
    r.s.goals[0] = strdup("build/ima\rges/root.disk");
    run(&r.s, 1);
    row_end(&r);
}

/** Check that a clean import requires a producer whose receipt says clean
 * and ordinary, that a retained import takes any, and that neither changes
 * the receipt. */
static void clean_goal_conditioning(void)
{
    static const char *const regimes[] = {"clean", "retained", "unknown", NULL};
    struct row r;
    struct json *original, *conditioning;
    char *before, *after;
    size_t i;

    row_start(&r, "clean-import", NULL, 0, NULL);
    original = pf_jclone(receipt(&r.s));
    for (i = 0; i < 4; i++) {
        struct json *copy = pf_jclone(original);
        pf_jdel(copy, "conditioning");
        if (regimes[i]) {
            conditioning = pf_jobj();
            pf_jset(conditioning, "inhouse", pf_jstr(regimes[i]));
            pf_jset(conditioning, "backend", pf_jstr("ordinary"));
            pf_jset(conditioning, "provenance", pf_jnull());
            pf_jset(copy, "conditioning", conditioning);
        }
        pf_jset(r.s.metadata, "receipt", copy);
        before = pf_jtext(receipt(&r.s));
        r.s.conditioning = "clean";
        json_free(run(&r.s, i == 0 ? 0 : 1));
        r.s.conditioning = NULL;
        json_free(run(&r.s, 0));
        after = pf_jtext(receipt(&r.s));
        PF_CHECK(!strcmp(before, after), "conditioning: the receipt unchanged");
        free(before);
        free(after);
    }
    conditioning = pf_jobj();
    pf_jset(conditioning, "inhouse", pf_jstr("clean"));
    pf_jset(conditioning, "backend", pf_jstr("import"));
    pf_jset(conditioning, "provenance", pf_jobj());
    pf_jset(pf_jget(conditioning, "provenance"), "sha256", pf_jstr("fixture"));
    pf_jset(receipt(&r.s), "conditioning", conditioning);
    r.s.conditioning = "clean";
    run(&r.s, 1);
    json_free(original);
    row_end(&r);
}

/** Check that each goal is imported as a portable artifact or built as a
 * native one, as its kind requires. */
static void roles(void)
{
    static const char *const portable[] = {"build/tests/cases/machine/install.disk",
                                           "build/x86_64/tests/cases/machine/install.disk",
                                           "build/tests/cases/machine/test-password",
                                           "build/tests/cases/host/fstool",
                                           "build/tests/host-suite/schemas/case.schema.json",
                                           "build/tests/hello"};
    static const char *const native[] = {"build/tests/cases/machine/disk",
                                         "build/tools/etude/etude", "build/krffs/readfile.krffs",
                                         "build/tests/host-suite/scripts/kstack", sanitizer};
    enum { P = sizeof(portable) / sizeof(*portable), N = sizeof(native) / sizeof(*native) };
    const char *goals[P + N];
    const struct json *given;
    struct json *proof;
    struct row r;
    size_t i;

    for (i = 0; i < P; i++)
        goals[i] = portable[i];
    for (i = 0; i < N; i++)
        goals[P + i] = native[i];
    row_start(&r, "import-roles", goals, P + N, NULL);
    make_rule(&r.s, "%:\n\t@mkdir -p $(@D)\n\tcp /usr/bin/true $@\n");
    sanitizer_rule(&r.s);
    pf_import_source(&r.s);
    proof = run(&r.s, 0);
    given = json_member(proof, "roles");
    PF_CHECK(given && given->count == P + N, "roles: one for each goal");
    for (i = 0; i < P + N; i++) {
        const char *role = json_text(given, goals[i]);
        PF_CHECK(role && !strcmp(role, i < P ? "portable" : "native"), "roles: each goal's role");
    }
    json_free(proof);
    row_end(&r);
}

/** Check that a case declares the sanitizer manifest as a goal, that it is
 * built natively and recorded, and that a manifest for another platform is
 * refused. */
static void sanitizer_file_goal(void)
{
    static const char *const goals[] = {sanitizer};
    const char *cases = pf_at(pf_repo, "tests/cases");
    char *listing, *line, *next, rule[PF_PATH];
    struct jbuf out = {0};
    const char *ls[] = {"ls", cases, NULL};
    struct json *proof, *manifest;
    int declared = 0;
    struct row r;

    pf_run(NULL, NULL, ls, &out);
    listing = out.data;
    for (line = listing; line && *line; line = next) {
        struct json *document;
        const struct json *list;
        size_t i, j, n = strcspn(line, "\n");
        next = line + n + (line[n] != 0);
        line[n] = 0;
        if (n < 5 || strcmp(line + n - 5, ".json"))
            continue;
        document = pf_jload(pf_at(pf_at(pf_repo, "tests/cases"), line));
        list = json_member(document, "cases");
        for (i = 0; list && i < list->count; i++) {
            const struct json *prepare = json_member(list->item[i], "prepare");
            for (j = 0; prepare && j < prepare->count; j++) {
                const char *node = json_text(prepare->item[j], "node");
                declared |= node && !strcmp(node, "tests/host-suite/sanitizer/tools.json");
            }
        }
        json_free(document);
    }
    jbuf_free(&out);
    PF_CHECK(declared, "sanitizer goal: a case declares it");
    row_start(&r, "sanitizer-goal", goals, 1, NULL);
    sanitizer_rule(&r.s);
    pf_import_source(&r.s);
    proof = run(&r.s, 0);
    PF_CHECK(pf_jsame(json_member(json_member(proof, "artifacts"), sanitizer),
                      pf_at(r.s.tree, sanitizer)),
             "sanitizer goal: recorded as built");
    json_free(proof);
    manifest = pf_jload(pf_at(r.s.tree, sanitizer));
    PF_CHECK(manifest != NULL, "sanitizer goal: the manifest");
    if (manifest) {
        pf_jset(manifest, "platform", pf_jstr("foreign"));
        pf_jsave(pf_at(r.s.tree, sanitizer), manifest);
    }
    json_free(manifest);
    fix_format(rule, sizeof(rule), "%s: ; @true\n", sanitizer);
    make_rule(&r.s, rule);
    pf_import_source(&r.s);
    run(&r.s, 1);
    row_end(&r);
}

/** Check that the sanitizer manifest's inputs bind what it was made from: a
 * repeated or escaping input is refused, and on macOS a changed, foreign or
 * linked snapshot of atos. */
static void sanitizer_source_binding(void)
{
    static const char *const goals[] = {sanitizer};
    static const char a64[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    char rule[PF_PATH], dir[PF_PATH];
    struct json *manifest, *saved, *rows;
    struct row r;
    size_t i;

    row_start(&r, "sanitizer-binding", goals, 1, NULL);
    fix_format(dir, sizeof(dir), "%s/build/tests/host-suite/sanitizer", r.s.tree);
    PF_CHECK(!pf_mkdirs(dir), "sanitizer binding: its directory");
    {
        const char *argv[] = {pf_etude, "prepare", "-H", "--only", "sanitizer", "tools", dir, NULL};
        PF_CHECK(pf_run(r.s.tree, NULL, argv, NULL) == 0, "sanitizer binding: prepared");
    }
    fix_format(rule, sizeof(rule), "%s: ; @true\n", sanitizer);
    make_rule(&r.s, rule);
    pf_import_source(&r.s);
    manifest = pf_jload(pf_at(r.s.tree, sanitizer));
    json_free(run(&r.s, 0));
    PF_CHECK(json_member(manifest, "inputs"), "sanitizer binding: its inputs");
    if (!json_member(manifest, "inputs")) {
        json_free(manifest);
        row_end(&r);
        return;
    }
    saved = pf_jclone(json_member(manifest, "inputs"));
    if (saved->count) {
        rows = pf_jarr();
        for (i = 0; i < 2 * saved->count; i++)
            pf_jadd(rows, pf_jclone(saved->item[i % saved->count]));
        pf_jset(manifest, "inputs", rows);
        pf_jsave(pf_at(r.s.tree, sanitizer), manifest);
        run(&r.s, 1);
    }
    rows = pf_jarr();
    pf_jadd(rows, pf_jobj());
    pf_jset(rows->item[0], "path", pf_jstr("../elsewhere"));
    pf_jset(rows->item[0], "sha256", pf_jstr(a64));
    pf_jset(manifest, "inputs", rows);
    pf_jsave(pf_at(r.s.tree, sanitizer), manifest);
    run(&r.s, 1);
    pf_jset(manifest, "inputs", saved);
    pf_jsave(pf_at(r.s.tree, sanitizer), manifest);
#ifdef __APPLE__
    {
        char snapshot[PF_PATH], proof[PF_PATH], truth[PF_PATH];
        size_t size;
        char *before, *previous;
        struct json *value;
        fix_format(snapshot, sizeof(snapshot), "%s/atos", dir);
        fix_format(proof, sizeof(proof), "%s.provenance.json", snapshot);
        before = pf_read(snapshot, &size);
        pf_append_text(snapshot, "changed");
        run(&r.s, 1);
        pf_write(snapshot, before, size);
        previous = pf_read(proof, NULL);
        value = pf_jparse(previous);
        PF_CHECK(value && realpath("/usr/bin/true", truth), "sanitizer binding: provenance");
        if (value) {
            pf_jset(value, "source", pf_jstr(truth));
            pf_jsave(proof, value);
        }
        run(&r.s, 1);
        pf_write_text(proof, previous ? previous : "");
        unlink(snapshot);
        PF_CHECK(!symlink("/usr/bin/atos", snapshot), "sanitizer binding: linked atos");
        run(&r.s, 1);
        json_free(value);
        free(previous);
        free(before);
    }
#endif
    json_free(manifest);
    row_end(&r);
}

/* Replace each `old` in `text` with `new`, in a new buffer. */
static char *replaced(const char *text, const char *old, const char *new)
{
    struct jbuf out = {0};
    const char *found;

    jbuf_puts(&out, "");
    while ((found = strstr(text, old)) != NULL) {
        jbuf_put(&out, text, (size_t)(found - text));
        jbuf_puts(&out, new);
        text = found + strlen(old);
    }
    jbuf_puts(&out, text);
    return out.data;
}

/** Check that the producer's record must be the one its receipt binds,
 * whole, inside the tree, with valid digests and sizes. */
static void record_identity(void)
{
    struct jbuf twice = {0};
    struct row r;
    char *bad[5];
    const char *line;
    size_t i;

    row_start(&r, "record-identity", NULL, 0, NULL);
    for (line = r.s.record; strchr(line, '\n') && strchr(line, '\n')[1];)
        line = strchr(line, '\n') + 1;
    jbuf_puts(&twice, r.s.record);
    jbuf_puts(&twice, line);
    bad[0] = twice.data;
    bad[1] = replaced(r.s.record, "build/images/root.disk", "../root");
    bad[2] = replaced(r.s.record, json_text(json_member(r.s.files, r.s.goals[0]), "sha256"),
                      "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
    bad[3] = strdup("etude-prepare 2\n");
    bad[4] = replaced(r.s.record, " 15 1 0 ", " 99999999999999999999999 1 0 ");
    json_free(run(&r.s, 0));
    for (i = 0; i < 5; i++) {
        pf_jset(r.s.metadata, "record", pf_jstr(bad[i]));
        run(&r.s, 1);
        free(bad[i]);
    }
    row_end(&r);
}

/** Check that the receipt must say a passing, cleaned production of this
 * commit and record, with a valid interval and identities. */
static void receipt_binding(void)
{
    static const char *const keys[] = {"cleanup", "outcome", "commit",  "record_sha256",
                                       "end",     "run_id",  "boot_id", "start"};
    struct json *original;
    struct row r;
    size_t i;

    row_start(&r, "receipt-binding", NULL, 0, NULL);
    original = pf_jclone(receipt(&r.s));
    json_free(run(&r.s, 0));
    for (i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
        struct json *copy = pf_jclone(original), *changed;
        switch (i) {
        case 0:
            changed = pf_jstr("unknown");
            break;
        case 1:
            changed = pf_jstr("fail");
            break;
        case 2:
            changed = pf_jstr("2222222222222222222222222222222222222222");
            break;
        case 3:
            changed = pf_jstr("0000000000000000000000000000000000000000000000000000000000000000");
            break;
        case 4:
            changed = pf_jclone(json_member(original, "end"));
            pf_jset(changed, "monotonic_us", pf_jint(0));
            break;
        case 5:
            changed = pf_jstr("");
            break;
        case 6:
            changed = pf_jstr("other boot");
            break;
        default:
            changed = pf_jclone(json_member(original, "start"));
            pf_jset(changed, "resolution_us", pf_jint(0));
            break;
        }
        pf_jset(copy, keys[i], changed);
        pf_jset(r.s.metadata, "receipt", copy);
        run(&r.s, 1);
    }
    json_free(original);
    row_end(&r);
}

/** Check that a producer file whose bytes changed, or that became a link,
 * is refused. */
static void changed_file(void)
{
    char path[PF_PATH];
    struct row r;

    row_start(&r, "changed-import", NULL, 0, NULL);
    json_free(run(&r.s, 0));
    fix_format(path, sizeof(path), "%s/%s", r.s.producer, r.s.goals[0]);
    pf_write_text(path, "different bytes\n");
    run(&r.s, 1);
    unlink(path);
    PF_CHECK(!symlink("/dev/null", path), "changed file: a link");
    run(&r.s, 1);
    row_end(&r);
}

/* An inventory check `id` of the GNU backend whose inputs are `inputs`. */
static struct json *gnu_check(const char *id, struct json *inputs)
{
    struct json *check = pf_jobj();

    pf_jset(check, "assertion_id", pf_jstr(id));
    pf_jset(check, "backend", pf_jstr("gnu"));
    pf_jset(check, "inputs", inputs);
    return check;
}

/* An input row of an inventory: `key` is path or path_hex. */
static struct json *input(const char *key, const char *path, const char *sha256)
{
    struct json *row = pf_jobj();

    pf_jset(row, key, pf_jstr(path));
    pf_jset(row, "sha256", pf_jstr(sha256));
    return row;
}

/** Check that an inventory goal brings the nested inputs it names, by their
 * digests, and that an inventory that differs from its record is refused. */
static void inventory_chain(void)
{
    static const char nested[] =
        "build/tests/host-suite/tools/exfat_external/exfat_src/e\xc3\xa9t\xc3\xa9.txt";
    static const char *const goals[] = {inventory};
    struct json *checks = pf_jarr(), *inputs = pf_jarr(), *proof, *changed;
    char digest[65], encoded[2 * sizeof(nested)], *text, *copy, *spaced;
    struct row r;
    size_t i;

    pf_sha_bytes("nested fixture bytes\n", 21, digest);
    for (i = 0; i < sizeof(nested) - 1; i++)
        snprintf(encoded + 2 * i, 3, "%02x", (unsigned char)nested[i]);
    pf_jadd(inputs, input("path_hex", encoded, digest));
    pf_jadd(checks, gnu_check("exfat", inputs));
    row_start(&r, "inventory-chain", goals, 1, checks);
    proof = run(&r.s, 0);
    PF_CHECK(json_text(json_member(proof, "roles"), inventory) &&
                 !strcmp(json_text(json_member(proof, "roles"), inventory), "inventory"),
             "inventory chain: the inventory's role");
    PF_CHECK(json_member(json_member(proof, "imported"), nested) &&
                 json_equal(json_member(json_member(proof, "imported"), nested),
                            json_member(json_member(r.s.metadata, "nested"), nested)),
             "inventory chain: the nested input imported");
    text = pf_read(pf_at(r.s.tree, nested), NULL);
    PF_CHECK(text && !strcmp(text, "nested fixture bytes\n"), "inventory chain: its bytes");
    free(text);
    json_free(proof);
    copy = strdup(json_text(json_member(r.s.metadata, "inventories"), inventory));
    spaced = json_alloc(strlen(copy) + 2);
    strcpy(spaced, copy);
    strcat(spaced, " ");
    pf_jset(pf_jget(r.s.metadata, "inventories"), inventory, pf_jstr(spaced));
    free(spaced);
    run(&r.s, 1);
    changed = pf_jparse(copy);
    pf_jset(changed, "manifest_sha256",
            pf_jstr("0000000000000000000000000000000000000000000000000000000000000000"));
    text = pf_jtext(changed);
    pf_jset(pf_jget(r.s.metadata, "inventories"), inventory, pf_jstr(text));
    run(&r.s, 1);
    free(text);
    free(copy);
    json_free(changed);
    json_free(checks);
    row_end(&r);
}

/** Check that the alignment checks' e2fsprogs inputs are imported, and that
 * an input outside the tree, a bad digest or a repeated path is refused. */
static void alignment_inventory_chain(void)
{
    static const char prefix[] = "vendor/e2fsprogs/e2fsprogs-1.47.4/lib/";
    static const char other[] = "build/x86_64/vendor-work/e2fsprogs/e2fsprogs-1.47.4/lib/";
    static const char *const names[] = {"ext2fs/unix_io.c", "ext2fs/ext2_types.h",
                                        "ext2fs/ext2fs.h", "et/com_err.h"};
    static const char *const goals[] = {inventory};
    static const char a64[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char b64[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    struct json *inputs = pf_jarr(), *checks = pf_jarr(), *proof;
    char digest[65], path[256], *original;
    struct row r;
    size_t i, k;

    pf_sha_bytes("nested fixture bytes\n", 21, digest);
    for (k = 0; k < 2; k++)
        for (i = 0; i < 4; i++) {
            fix_format(path, sizeof(path), "%s%s", k ? other : prefix, names[i]);
            pf_jadd(inputs, input("path", path, digest));
        }
    pf_jadd(checks, gnu_check("e2fsprogs_alignment", pf_jclone(inputs)));
    pf_jadd(checks, gnu_check("e2fsprogs_alignment64", pf_jclone(inputs)));
    row_start(&r, "alignment-chain", goals, 1, checks);
    proof = run(&r.s, 0);
    for (i = 0; i < inputs->count; i++)
        PF_CHECK(json_member(json_member(proof, "imported"), json_text(inputs->item[i], "path")),
                 "alignment chain: each input imported");
    json_free(proof);
    original = strdup(json_text(json_member(r.s.metadata, "inventories"), inventory));
    for (k = 0; k < 4; k++) {
        struct json *document = pf_jparse(original), *rows = pf_jarr();
        char *text;
        switch (k) {
        case 0:
            pf_jadd(rows, input("path", "../escape.h", a64));
            break;
        case 1:
            fix_format(path, sizeof(path), "%s../escape.h", prefix);
            pf_jadd(rows, input("path", path, a64));
            break;
        case 2:
            fix_format(path, sizeof(path), "%sext2fs/ext2fs.h", prefix);
            pf_jadd(rows, input("path", path, "bad"));
            break;
        default:
            fix_format(path, sizeof(path), "%sext2fs/ext2fs.h", prefix);
            pf_jadd(rows, input("path", path, a64));
            pf_jadd(rows, input("path", path, b64));
            break;
        }
        pf_jset(json_member(document, "checks")->item[0], "inputs", rows);
        text = pf_jtext(document);
        pf_rerecord(&r.s, inventory, text);
        run(&r.s, 1);
        free(text);
        json_free(document);
    }
    free(original);
    json_free(inputs);
    json_free(checks);
    row_end(&r);
}

/* The length of `argv` as one Windows command line, as Python's
 * subprocess.list2cmdline quotes it. */
static size_t windows_length(const struct json *argv)
{
    size_t i, n = 0;

    for (i = 0; i < argv->count; i++) {
        const char *word = argv->item[i]->string ? argv->item[i]->string : "";
        n += strlen(word) + (i > 0);
        if (!*word || strpbrk(word, " \t"))
            n += 2;
        n += pf_count(word, "\"");
    }
    return n;
}

/* Order two strings, for qsort. */
static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/** Check that many native goals are built by several commands, each short
 * enough for Windows, that together name every goal once in order; and that
 * portable goals too long to request are refused before any build. */
static void windows_vectors(void)
{
    enum { NATIVE = 900, PORTABLE = 130 };
    char **names = calloc(NATIVE + PORTABLE + 1, sizeof(*names));
    const char **goals = calloc(NATIVE + PORTABLE + 1, sizeof(*goals));
    const char **sorted = calloc(NATIVE, sizeof(*sorted));
    const struct json *commands;
    struct json *proof;
    struct row r;
    size_t i, j, k, selected = 0, ordered = 1, short_enough = 1;
    char name[512], filler[221];

    for (i = 0; i < NATIVE; i++) {
        fix_format(name, sizeof(name), "build/tests/cases/machine/scenario%zu", i);
        names[i] = strdup(name);
        goals[i] = sorted[i] = names[i];
    }
    qsort(sorted, NATIVE, sizeof(*sorted), by_name);
    row_start(&r, "native-argv", goals, NATIVE, NULL);
    make_rule(&r.s, "%:\n\t@mkdir -p $(@D)\n\tcp /usr/bin/true $@\n");
    pf_import_source(&r.s);
    proof = run(&r.s, 0);
    commands = json_member(proof, "native_build");
    PF_CHECK(commands && commands->count > 1, "argv: several native commands");
    for (i = 0; commands && i < commands->count; i++) {
        const struct json *argv = commands->item[i];
        short_enough &= windows_length(argv) < 28000;
        for (j = 0; j < argv->count; j++) {
            const char *word = argv->item[j]->string;
            for (k = 0; word && k < NATIVE; k++) {
                if (strcmp(word, names[k]))
                    continue;
                ordered &= selected < NATIVE && !strcmp(word, sorted[selected]);
                selected++;
                break;
            }
        }
    }
    PF_CHECK(selected == NATIVE && ordered, "argv: every goal once, in order");
    PF_CHECK(short_enough, "argv: each command fits a Windows command line");
    json_free(proof);
    row_end(&r);
    memset(filler, 'a', 220);
    filler[220] = 0;
    for (i = 0; i < PORTABLE; i++) {
        fix_format(name, sizeof(name), "build/images/%zu-%s.disk", i, filler);
        names[NATIVE + i] = strdup(name);
        goals[i] = names[NATIVE + i];
    }
    goals[PORTABLE] = "build/tests/cases/machine/a";
    row_start(&r, "frozen-argv", goals, PORTABLE + 1, NULL);
    run(&r.s, 1);
    PF_CHECK(!pf_exists(pf_at(r.s.tree, "build/compile.calls")), "argv: nothing compiled");
    row_end(&r);
    for (i = 0; i < NATIVE + PORTABLE; i++)
        free(names[i]);
    free(names);
    free(goals);
    free(sorted);
}

/* Set the times of `path` to `ns` nanoseconds. */
static int touch(const char *path, long long ns)
{
    struct timespec times[2];

    times[0].tv_sec = times[1].tv_sec = (time_t)(ns / 1000000000LL);
    times[0].tv_nsec = times[1].tv_nsec = (long)(ns % 1000000000LL);
    return utimensat(AT_FDCWD, path, times, 0);
}

/* The modification time of `path` in nanoseconds, or -1. */
static long long mtime_ns(const char *path)
{
    struct stat st;

    if (stat(path, &st))
        return -1;
#ifdef __APPLE__
    return st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

/** Check that a native goal is compiled here even when a newer foreign copy
 * is in place, kept while its sources are unchanged, and compiled again when
 * a source or header changes, while portable goals are imported. */
static void native_compilation(void)
{
    static const char *const goals[] = {"build/images/root.disk", "build/tests/cases/machine/a"};
    static const char *const changes[][2] = {
        {"value.h", "#define VALUE 1\n"},
        {"source.c", "#include \"value.h\"\nint main(void) { return VALUE + 1; }\n"}};
    char target[PF_PATH], *text;
    struct stat before, after;
    long long future, before_ns;
    struct row r;
    size_t i;

    row_start(&r, "native-build", goals, 2, NULL);
    fix_format(target, sizeof(target), "%s/%s", r.s.tree, goals[1]);
    PF_CHECK(!pf_write_text(target, "old foreign executable"), "native build: a foreign copy");
    future = mtime_ns(pf_at(r.s.tree, "source.c"));
    if (mtime_ns(pf_at(r.s.tree, "value.h")) > future)
        future = mtime_ns(pf_at(r.s.tree, "value.h"));
    touch(target, future + 2000000000LL);
    json_free(run(&r.s, 0));
    text = pf_read(target, NULL);
    PF_CHECK(text && strcmp(text, "old foreign executable"), "native build: compiled here");
    free(text);
    stat(target, &before);
    before_ns = mtime_ns(target);
    json_free(run(&r.s, 0));
    PF_CHECK(!stat(target, &after) && before.st_ino == after.st_ino &&
                 mtime_ns(target) == before_ns,
             "native build: kept when unchanged");
    for (i = 0; i < 2; i++) {
        const char *path = pf_at(r.s.tree, changes[i][0]);
        pf_write_text(path, changes[i][1]);
        touch(path, mtime_ns(target) + 2000000000LL);
        pf_import_source(&r.s);
        json_free(run(&r.s, 0));
        PF_CHECK(pf_status(target) == (int)i + 1, "native build: compiled again");
    }
    text = pf_read(pf_at(r.s.tree, "build/compile.calls"), NULL);
    PF_CHECK(text && !strcmp(text, "compile\ncompile\ncompile\n"), "native build: three compiles");
    free(text);
    text = pf_read(pf_at(r.s.tree, goals[0]), NULL);
    PF_CHECK(text && !strcmp(text, "producer bytes\n"), "native build: the portable goal");
    free(text);
    row_end(&r);
}

/** Check that the OpenSSL build of the fake API runs a forced upstream
 * configuration under etude's make wrapper with the build's job limit. */
static void forced_openssl_dependency(void)
{
    static const char pin[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *given = getenv("ETUDE_MAKE_REAL");
    char work[PF_PATH], real[PF_PATH], source[PF_PATH], path[PF_PATH + 16], bin[PF_PATH];
    char make_real[PF_PATH + 32], setting[2 * PF_PATH], *text, expected[PF_PATH + 8];

    pf_scratch("import-openssl", work);
    PF_CHECK(given && *given ? realpath(given, real) != NULL : !pf_which("make", real),
             "openssl: a make");
    fix_format(bin, sizeof(bin), "%s/bin", work);
    PF_CHECK(!pf_copy(pf_at(pf_repo, "tests/system/fakeapi/build_openssl.sh"),
                      pf_at(work, "tests/system/fakeapi/build_openssl.sh")) &&
                 !pf_copy(pf_etude, pf_at(bin, "make")) && !chmod(pf_at(bin, "make"), 0755) &&
                 !pf_copy(pf_self, pf_at(bin, "uname")) && !chmod(pf_at(bin, "uname"), 0755),
             "openssl: the build script, the make wrapper and uname");
    PF_CHECK(!pf_write_text(pf_at(work, "vendor/gnu/fetch.sh"), "printf '%s\\n' unused-archive\n"),
             "openssl: fetch");
    fix_format(path, sizeof(path), "openssl 1 unused %s\n", pin);
    PF_CHECK(!pf_write_text(pf_at(work, "vendor/gnu/sources.txt"), path), "openssl: pins");
    fix_format(source, sizeof(source), "%s/build/vendor/host-openssl/source-%s", work, pin);
    PF_CHECK(!pf_mkdirs(pf_at(source, "include/openssl")) &&
                 !pf_write_text(pf_at(source, "Configure"), "exit 0;\n") &&
                 !pf_write_text(pf_at(source, "configdata.mk"), "# configured\n") &&
                 !touch(pf_at(source, "Configure"), 1000000000LL) &&
                 !touch(pf_at(source, "configdata.mk"), 2000000000LL),
             "openssl: the configured source");
    PF_CHECK(!pf_write_text(pf_at(source, "Makefile"),
                            "include configdata.mk\n"
                            "configdata.mk: Configure\n"
                            "\t@echo forced-upstream-configuration; exit 17\n"
                            ".PHONY: build_libs\n"
                            "build_libs:\n"
                            "\t@printf \"%s\\n\" \"$$ETUDE_BUILD_JOBS\" \"$$ETUDE_MAKE_REAL\" > "
                            "policy\n"
                            "\t@printf ssl > libssl.a; printf crypto > libcrypto.a\n") &&
                 !pf_write_text(pf_at(work, "Makefile"),
                                ".PHONY: all\nall:\n\tsh tests/system/fakeapi/build_openssl.sh\n"),
             "openssl: the Makefiles");
    fix_format(setting, sizeof(setting), "PATH=%s:%s", bin, getenv("PATH") ? getenv("PATH") : "");
    fix_format(make_real, sizeof(make_real), "ETUDE_MAKE_REAL=%s", real);
    {
        const char *env[] = {setting,   "ETUDE_BUILD_JOBS=2", make_real, "MAKEFLAGS=",
                             "MFLAGS=", "MAKELEVEL=0",        "OS=",     NULL};
        const char *argv[] = {real, "-B", "-k", "-j2", NULL};
        struct jbuf out = {0};
        int status = pf_run(work, env, argv, &out);
        PF_CHECK(status == 0, "openssl: the build");
        if (status)
            printf("etude-prepare-test: the OpenSSL build gave %d:\n%s", status, out.data);
        jbuf_free(&out);
    }
    fix_format(expected, sizeof(expected), "2\n%s\n", real);
    text = pf_read(pf_at(source, "policy"), NULL);
    PF_CHECK(text && !strcmp(text, expected), "openssl: the job limit and the real make");
    free(text);
    text = pf_read(pf_at(work, "build/vendor/host-openssl/lib/libssl.a"), NULL);
    PF_CHECK(text && !strcmp(text, "ssl"), "openssl: the library installed");
    free(text);
    PF_CHECK(pf_exists(pf_at(work, "build/vendor/host-openssl/ready")), "openssl: ready");
    pf_cleanup(work);
}

/* Import from a producer changed by the members `name` = `value`. */
static void producer_run(struct pf_import *s, int expected, const char *name, const char *value,
                         const char *const env[])
{
    struct json *producer = pf_jobj();

    pf_jset(producer, name, pf_jstr(value));
    json_free(pf_import_run(s, expected, producer, env));
    json_free(producer);
}

/** Check that a local producer must be idle and another tree than the
 * destination, by its path or a link to it. */
static void idle_local_producer(void)
{
    char alias[PF_PATH];
    struct row r;

    row_start(&r, "idle-producer", NULL, 0, NULL);
    json_free(run(&r.s, 0));
    producer_run(&r.s, 1, "tree", r.s.tree, NULL);
    fix_format(alias, sizeof(alias), "%s/alias", r.work);
    PF_CHECK(!symlink(r.s.tree, alias), "idle producer: an alias");
    producer_run(&r.s, 1, "tree", alias, NULL);
    PF_CHECK(!symlink(pf_at(r.s.producer, "absent"), pf_at(r.s.producer, "build/.run.lock")),
             "idle producer: a lock");
    run(&r.s, 1);
    row_end(&r);
}

/** Check that WSL alone, or an unknown transport, is refused. */
static void refused_wsl_transports(void)
{
    struct row r;

    row_start(&r, "wsl-refusal", NULL, 0, NULL);
    producer_run(&r.s, 1, "transport", "wsl", NULL);
    producer_run(&r.s, 1, "transport", "other", NULL);
    row_end(&r);
}

/* A remote producer reached by `transport` through the ssh and rsync
 * stand-ins: its fields, the PATH that finds them, in `path`, and their call
 * log, in `log`. */
static struct json *import_remote(struct pf_import *s, const char *work, const char *transport,
                                  char path[2 * PF_PATH], char log[PF_PATH])
{
    struct json *fields = pf_jobj();
    char commands[PF_PATH], conf[4 * PF_PATH];

    fix_format(commands, sizeof(commands), "%s/commands", work);
    fix_format(log, PF_PATH, "%s/calls.jsonl", commands);
    fix_format(conf, sizeof(conf), "%s\n%s\n%s\n%s\n", log, pf_at(s->producer, "producer.json"),
               pf_etude, s->producer);
    PF_CHECK(!pf_copy(pf_self, pf_at(commands, "ssh")) &&
                 !pf_copy(pf_self, pf_at(commands, "rsync")) &&
                 !pf_write_text(pf_at(commands, "standin"), conf),
             "remote: the stand-ins");
    pf_jset(pf_jget(s->metadata, "validator"), "path", pf_jstr("/immutable/etude"));
    pf_jset(fields, "transport", pf_jstr(transport));
    pf_jset(fields, "host", pf_jstr("fixture"));
    pf_jset(fields, "etude", pf_jstr("/immutable/etude"));
    pf_jset(fields, "tree", pf_jstr("/producer"));
    pf_jset(fields, "metadata", pf_jstr("/metadata"));
    if (!strcmp(transport, "wsl-ssh")) {
        pf_jset(fields, "distribution", pf_jstr("Ubuntu-26.04"));
        pf_jset(fields, "user", pf_jstr("testing"));
    }
    fix_format(path, 2 * PF_PATH, "PATH=%s:%s", commands, getenv("PATH") ? getenv("PATH") : "");
    return fields;
}

/* The calls of the stand-ins' log, each an array of its words. */
static struct json *calls(const char *log)
{
    struct json *all = pf_jarr();
    char *text = pf_read(log, NULL), *line, *next;

    for (line = text; line && *line; line = next) {
        next = line + strcspn(line, "\n");
        if (*next)
            *next++ = 0;
        struct json *call = pf_jparse(line);
        pf_jadd(all, call ? call : pf_jnull());
    }
    free(text);
    return all;
}

/* The program name of a logged call. */
static const char *called(const struct json *call)
{
    return call && call->type == JSON_ARRAY && call->count ? call->item[0]->string : "";
}

/** Check the WSL producer's SSH command, and that a distribution or user
 * that is empty, an option or more than one word is refused before any
 * call. */
static void posix_wsl_ssh(void)
{
    static const char *const keys[] = {"distribution", "user"};
    static const char *const values[] = {"", "-bad", "bad value", "bad;command"};
    static const char *const prefix[] = {"env",          "MSYS_NO_PATHCONV=1",
                                         "wsl.exe",      "--distribution",
                                         "Ubuntu-26.04", "--user",
                                         "testing",      "--exec"};
    char path[2 * PF_PATH], log[PF_PATH];
    struct json *producer, *log_calls;
    const char *env[] = {path, NULL};
    struct row r;
    size_t i, j, n;
    char **words;

    row_start(&r, "wsl-command", NULL, 0, NULL);
    producer = import_remote(&r.s, r.work, "wsl-ssh", path, log);
    json_free(pf_import_run(&r.s, 0, producer, env));
    log_calls = calls(log);
    PF_CHECK(log_calls->count == 3 && !strcmp(called(log_calls->item[0]), "ssh") &&
                 !strcmp(called(log_calls->item[1]), "ssh") &&
                 !strcmp(called(log_calls->item[2]), "rsync"),
             "wsl: two ssh calls and one rsync");
    if (log_calls->count && log_calls->item[0]->count &&
        pf_split(log_calls->item[0]->item[log_calls->item[0]->count - 1]->string, &n, &words)) {
        int same = n >= 8;
        for (i = 0; same && i < 8; i++)
            same = !strcmp(words[i], prefix[i]);
        PF_CHECK(same, "wsl: the distribution and user");
        pf_words_free(words, n);
    } else {
        PF_CHECK(0, "wsl: the first command");
    }
    json_free(log_calls);
    for (i = 0; i < 2; i++)
        for (j = 0; j < 4; j++) {
            struct json *changed = pf_jclone(producer);
            pf_jset(changed, keys[i], pf_jstr(values[j]));
            json_free(pf_import_run(&r.s, 1, changed, env));
            json_free(changed);
        }
    log_calls = calls(log);
    PF_CHECK(log_calls->count == 3, "wsl: no call for a refused producer");
    json_free(log_calls);
    json_free(producer);
    row_end(&r);
}

/** Check the SSH producer's rsync command, and that a validator other than
 * the producer's etude is refused. */
static void ssh_compression(void)
{
    static const char *const server[] = {"/immutable/etude", "lock",           "tree",
                                         "/producer",        "prepare-import", "/usr/bin/rsync"};
    char path[2 * PF_PATH], log[PF_PATH], tail[PF_PATH];
    struct json *producer, *log_calls;
    const struct json *argv;
    const char *env[] = {path, NULL};
    struct row r;
    size_t i, compress = 0, sparse = 0, n;
    char **words = NULL;

    row_start(&r, "ssh-command", NULL, 0, NULL);
    producer = import_remote(&r.s, r.work, "ssh", path, log);
    json_free(pf_import_run(&r.s, 0, producer, env));
    log_calls = calls(log);
    argv = log_calls->count ? log_calls->item[log_calls->count - 1] : NULL;
    PF_CHECK(argv && argv->type == JSON_ARRAY && argv->count >= 4, "ssh: the rsync call");
    for (i = 0; argv && i < argv->count; i++) {
        const char *word = argv->item[i]->string;
        compress += !strcmp(word, "--compress");
        sparse += !strcmp(word, "--sparse");
        if (!strncmp(word, "--rsync-path=", 13) && words == NULL &&
            pf_split(word + 13, &n, &words) == NULL)
            words = NULL;
    }
    PF_CHECK(compress == 1 && sparse >= 1, "ssh: compressed and sparse once");
    {
        int same = words && n == 6;
        for (i = 0; same && i < 6; i++)
            same = !strcmp(words[i], server[i]);
        PF_CHECK(same, "ssh: the remote rsync under the producer's lock");
        if (words)
            pf_words_free(words, n);
    }
    fix_format(tail, sizeof(tail), "%s/", r.s.tree);
    PF_CHECK(argv && argv->count >= 4 && !strcmp(argv->item[argv->count - 3]->string, "--") &&
                 !strcmp(argv->item[argv->count - 2]->string, "fixture:/producer/") &&
                 !strcmp(argv->item[argv->count - 1]->string, tail),
             "ssh: the source and the destination");
    json_free(log_calls);
    pf_jset(pf_jget(r.s.metadata, "validator"), "path", pf_jstr("/different/etude"));
    json_free(pf_import_run(&r.s, 1, producer, env));
    json_free(producer);
    row_end(&r);
}

/** Check that a local import copies the producer's file, by its digest. */
static void native_local_copy(void)
{
    char here[PF_PATH], there[PF_PATH];
    struct json *proof;
    struct stat a, b;
    struct row r;

    row_start(&r, "local-copy", NULL, 0, NULL);
    proof = run(&r.s, 0);
    PF_CHECK(json_equal(json_member(json_member(proof, "imported"), r.s.goals[0]),
                        json_member(r.s.files, r.s.goals[0])),
             "local copy: imported by its digest");
    fix_format(here, sizeof(here), "%s/%s", r.s.tree, r.s.goals[0]);
    fix_format(there, sizeof(there), "%s/%s", r.s.producer, r.s.goals[0]);
    PF_CHECK(!stat(here, &a) && !stat(there, &b) && a.st_ino != b.st_ino,
             "local copy: a file of its own");
    PF_CHECK(pf_same_file(here, there), "local copy: the same bytes");
    json_free(proof);
    row_end(&r);
}

/** Check that an inventory input outside the tree, or with an invalid or
 * doubled encoding of its name, is refused. */
static void encoded_inputs(void)
{
    static const char *const keys[][2] = {{"path", NULL},       {"path_hex", NULL},
                                          {"path_hex", NULL},   {"path_hex", NULL},
                                          {"path_hex", "path"}, {"path", NULL}};
    static const char *const values[][2] = {{"/outside", NULL}, {"0", NULL}, {"XX", NULL},
                                            {"00", NULL},       {"61", "a"}, {"../escape", NULL}};
    static const char *const goals[] = {inventory};
    static const char a64[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    size_t i;

    for (i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
        struct json *checks = pf_jarr(), *document, *item = pf_jobj(), *rows = pf_jarr();
        char *text;
        struct row r;
        pf_jadd(checks, gnu_check("exfat", pf_jarr()));
        row_start(&r, "encoded-input", goals, 1, checks);
        document = pf_jparse(json_text(json_member(r.s.metadata, "inventories"), inventory));
        pf_jset(item, keys[i][0], pf_jstr(values[i][0]));
        if (keys[i][1])
            pf_jset(item, keys[i][1], pf_jstr(values[i][1]));
        pf_jset(item, "sha256", pf_jstr(a64));
        pf_jadd(rows, item);
        pf_jset(json_member(document, "checks")->item[0], "inputs", rows);
        text = pf_jtext(document);
        pf_rerecord(&r.s, inventory, text);
        run(&r.s, 1);
        free(text);
        json_free(document);
        json_free(checks);
        row_end(&r);
    }
}

/** Check that native goals are built for the architectures their paths
 * give, without forcing a rebuild, and for the architectures requested. */
static void actual_architecture_buckets(void)
{
    static const char *const goals[] = {"build/tests/cases/machine/a",
                                        "build/x86_64/tests/cases/machine/b", inventory};
    static const char *const expected[] = {"ARCH=i686", "ARCH=x86_64", "ARCH=i686"};
    const struct json *commands;
    struct json *proof;
    struct row r;
    size_t i, j;

    row_start(&r, "architecture-buckets", goals, 3, NULL);
    make_rule(&r.s, "build/x86_64/tests/cases/machine/b: source.c value.h\n"
                    "\t@mkdir -p $(@D)\n\t$(CC) -o $@ $<\n");
    pf_import_source(&r.s);
    proof = run(&r.s, 0);
    commands = json_member(proof, "native_build");
    PF_CHECK(commands && commands->count == 3, "architectures: three builds");
    for (i = 0; commands && i < commands->count && i < 3; i++) {
        const char *arch = NULL;
        int forced = 0;
        for (j = 0; j < commands->item[i]->count; j++) {
            const char *word = commands->item[i]->item[j]->string;
            if (!arch && word && !strncmp(word, "ARCH=", 5))
                arch = word;
            forced |= word && !strcmp(word, "-B");
        }
        PF_CHECK(arch && !strcmp(arch, expected[i]), "architectures: each build's");
        PF_CHECK(!forced, "architectures: no forced rebuild");
    }
    json_free(proof);
    r.s.arches = "x86_64";
    proof = run(&r.s, 0);
    commands = json_member(proof, "native_build");
    for (i = 0; commands && i < commands->count; i++) {
        int found = 0;
        for (j = 0; j < commands->item[i]->count; j++)
            found |= commands->item[i]->item[j]->string &&
                     !strcmp(commands->item[i]->item[j]->string, "ARCH=x86_64");
        PF_CHECK(found, "architectures: the requested one");
    }
    json_free(proof);
    row_end(&r);
}

/** Run the import rows. */
void pf_import_rows(void)
{
    request_lines();
    clean_goal_conditioning();
    roles();
    sanitizer_file_goal();
    sanitizer_source_binding();
    record_identity();
    receipt_binding();
    changed_file();
    inventory_chain();
    alignment_inventory_chain();
    windows_vectors();
    native_compilation();
    forced_openssl_dependency();
    idle_local_producer();
    refused_wsl_transports();
    posix_wsl_ssh();
    ssh_compression();
    native_local_copy();
    encoded_inputs();
    actual_architecture_buckets();
}
