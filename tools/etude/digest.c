/*
 * digest.c: the three digests a plan's item takes of its case in a
 * manifest, each the SHA-256 of a canonical JSON document, so that equal
 * manifests give equal digests and a changed workload, inventory or profile
 * gives another.
 */
#include "digest.h"

#include <stdio.h>
#include <string.h>

#include "../../utils/sha256.h"

/* The most members a digest's document has, `schema_version` included. */
#define DIGEST_MEMBERS 9

/* The members of one document, as they are gathered. */
struct document {
    struct json *item[DIGEST_MEMBERS];
    size_t count;
};

/** Add the member `name` of `object` to `d`, when `object` has it. */
static void take(struct document *d, const struct json *object, const char *name)
{
    const struct json *m = json_member(object, name);

    if (m != NULL && d->count < DIGEST_MEMBERS - 1) {
        d->item[d->count++] = (struct json *)m;
    }
}

/** Add to `d` the string `text` as the member `name`, kept in `holder`. */
static void give(struct document *d, struct json *holder, const char *name, const char *text)
{
    memset(holder, 0, sizeof(*holder));
    holder->type = JSON_STRING;
    holder->name = (char *)name;
    holder->string = (char *)text;
    if (d->count < DIGEST_MEMBERS - 1) {
        d->item[d->count++] = holder;
    }
}

/**
 * Write into `hex` the SHA-256 of the canonical form of the object whose
 * members `d` has, with `schema_version` 1 added: members sorted by name and
 * no space outside strings, as json_canonical writes it.
 */
static void digest_of(struct document *d, char hex[65])
{
    struct json object = {0}, version = {0};
    struct jbuf b = {0};
    struct sha256 s;
    unsigned char digest[32];

    version.type = JSON_INTEGER;
    version.integer = 1;
    version.name = "schema_version";
    d->item[d->count++] = &version;
    object.type = JSON_OBJECT;
    object.item = d->item;
    object.count = d->count;
    json_canonical(&object, &b);
    sha256_init(&s);
    sha256_update(&s, (const unsigned char *)(b.data != NULL ? b.data : ""), b.len);
    sha256_final(&s, digest);
    sha256_hex(digest, hex);
    jbuf_free(&b);
}

/**
 * Write into `hex` the digest of the workload of case `c` on `architecture`
 * (i686, x86_64 or none): the document of its `architecture`, `case_id`,
 * `workload`, `progress`, `steps` and `services`, when it has them, and
 * `program` and `role` of its `execution`.
 */
void workload_digest(const struct json *c, const char *architecture, char hex[65])
{
    const struct json *execution = json_member(c, "execution");
    struct document d = {{0}, 0};
    struct json arch;

    give(&d, &arch, "architecture", architecture);
    take(&d, c, "case_id");
    take(&d, c, "workload");
    take(&d, c, "progress");
    take(&d, c, "steps");
    take(&d, c, "services");
    take(&d, execution, "program");
    take(&d, execution, "role");
    digest_of(&d, hex);
}

/**
 * Write into `hex` the digest of the assertion inventory of case `c`: the
 * document of its `case_id`, `assertions`, `branches` and `termination`.
 */
void inventory_digest(const struct json *c, char hex[65])
{
    struct document d = {{0}, 0};

    take(&d, c, "assertions");
    take(&d, c, "branches");
    take(&d, c, "case_id");
    take(&d, c, "termination");
    digest_of(&d, hex);
}

/**
 * Write into `hex` the digest of the profile of case `c` placed `placement`
 * (host, qemu or native): the document of its `case_id`, `placement`,
 * `requirements`, `resources` and `machines`, when it has them, and
 * `privileges` of its `execution`, with the `isolation` of its execution for
 * a host or QEMU placement, or the `fixtures` of its native plan for a
 * native one.
 */
void profile_digest(const struct json *c, const char *placement, char hex[65])
{
    const struct json *execution = json_member(c, "execution");
    struct document d = {{0}, 0};
    struct json place;

    take(&d, c, "case_id");
    give(&d, &place, "placement", placement);
    take(&d, c, "requirements");
    take(&d, c, "resources");
    take(&d, c, "machines");
    take(&d, execution, "privileges");
    if (strcmp(placement, "native") == 0) {
        take(&d, json_member(c, "native"), "fixtures");
    } else {
        take(&d, execution, "isolation");
    }
    digest_of(&d, hex);
}
