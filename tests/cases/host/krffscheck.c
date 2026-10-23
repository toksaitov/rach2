/* krffscheck.c: corrupt images and check each prefix of KRFFS updates. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native.h"

enum {
    HDR = 357,
    TYPE = 2,
    ID = 3,
    NAME = 11,
    MODE = 267,
    SIZE = 325,
    PREV = 333,
    DATA = 341,
    PARENT = 349,
    ROOT = 0,
    FREE = 1,
    RESERVED = 2,
    TOTAL = 1 << 20,
    MAX_NODES = 64
};
struct node {
    size_t off, size, data;
    uint64_t parent;
    unsigned type;
    char name[257];
};
struct store {
    size_t off, size;
    unsigned char *bytes;
};
struct stores {
    struct store item[32];
    size_t count;
};
static struct native_case test;
static char mkfs[4096], fsck[4096], stager[4096], reader[4096], image[4096];

static uint64_t get(const unsigned char *p, size_t n)
{
    uint64_t v = 0;
    while (n)
        v = (v << 8) | p[--n];
    return v;
}

static void put(unsigned char *p, uint64_t value, size_t n)
{
    for (size_t i = 0; i < n; i++, value >>= 8)
        p[i] = (unsigned char)value;
}

static uint64_t djb(const char *name)
{
    uint64_t h = 5381;
    while (*name)
        h = (h * 33) ^ (unsigned char)*name++;
    return h;
}

static void header(unsigned char *out, size_t size, size_t prev, unsigned type, const char *name,
                   unsigned mode)
{
    memset(out, 0, HDR);
    put(out, 0x4b52, 2);
    out[TYPE] = (unsigned char)type;
    put(out + ID, djb(name), 8);
    strcpy((char *)out + NAME, name);
    put(out + MODE, mode, 2);
    put(out + SIZE, size, 8);
    put(out + PREV, prev, 8);
}

static int walk(const unsigned char *img, size_t size, struct node out[MAX_NODES])
{
    size_t off = 0;
    int n = 0;
    while (off < size) {
        uint64_t step, length;
        if (size - off < HDR || n == MAX_NODES)
            return -1;
        step = get(img + off + SIZE, 8);
        length = get(img + off + DATA, 8);
        if (step < HDR || step > size - off || length > SIZE_MAX)
            return -1;
        out[n].off = off;
        out[n].size = (size_t)step;
        out[n].data = (size_t)length;
        out[n].parent = get(img + off + PARENT, 8);
        out[n].type = img[off + TYPE];
        memcpy(out[n].name, img + off + NAME, 256);
        out[n].name[256] = 0;
        n++;
        off += (size_t)step;
    }
    return n;
}

static int check(int strict)
{
    const char *args[] = {fsck, image, strict ? "--strict" : NULL, NULL};
    return native_run(&test, args);
}

static void assertion(const char *prefix, const char *suffix, int pass)
{
    char id[80];
    snprintf(id, sizeof(id), "%s%s%s", prefix, *suffix ? "-" : "", suffix);
    native_assert(&test, id, pass, "the image or checker result did not satisfy the assertion");
}

static unsigned char *clean(size_t size)
{
    unsigned char *data = calloc(1, size);
    const char *args[] = {mkfs, image, NULL};
    size_t actual = 0;
    if (!data || native_write(image, data, size) || native_run(&test, args)) {
        free(data);
        return NULL;
    }
    free(data);
    data = native_read(image, &actual);
    if (!data || actual != size) {
        free(data);
        return NULL;
    }
    return data;
}

static int broken(const unsigned char *base, const char *how)
{
    size_t n = 4u << 20, root = (size_t)get(base + SIZE, 8);
    unsigned char *data = malloc(n);
    int rc;
    if (!data || root > n - HDR) {
        free(data);
        return -1;
    }
    memcpy(data, base, n);
    if (!strcmp(how, "backlink"))
        put(data + root + PREV, root + 8, 8);
    else if (!strcmp(how, "rootlink"))
        put(data + PREV, 12345, 8);
    else if (!strcmp(how, "dirty"))
        put(data + MODE, get(data + MODE, 2) | 04000, 2);
    else if (!strcmp(how, "magic"))
        put(data + root, 0x1234, 2);
    else if (!strcmp(how, "datasize"))
        put(data + DATA, root, 8);
    else if (!strcmp(how, "zerosize")) {
        put(data + SIZE, 0, 8);
        put(data + DATA, 0, 8);
    } else if (!strcmp(how, "smallsize"))
        put(data + root + SIZE, 8, 8);
    else if (!strcmp(how, "hugesize"))
        put(data + root + SIZE, UINT64_C(1) << 40, 8);
    else if (!strcmp(how, "freedatasize"))
        put(data + root + DATA, get(data + root + SIZE, 8), 8);
    else {
        free(data);
        return -1;
    }
    rc = native_write(image, data, n);
    free(data);
    return rc;
}

static void ordinary(const char *which)
{
    static const char *repair[] = {"backlink", "rootlink", "dirty"};
    static const char *fatal[] = {"magic", "datasize"};
    static const char *sizes[] = {"zerosize", "smallsize", "hugesize"};
    unsigned char *base = clean(4u << 20);
    const char **faults = !strcmp(which, "repairable") ? repair
                          : !strcmp(which, "fatal")    ? fatal
                                                       : sizes;
    size_t count = !strcmp(which, "fatal") ? 2 : 3;
    if (!strcmp(which, "fresh")) {
        assertion("strict", "", base && check(1) == 0);
        assertion("plain", "", base && check(0) == 0);
    } else if (!strcmp(which, "free")) {
        assertion("accepted", "", base && !broken(base, "freedatasize") && check(1) == 0);
    } else
        for (size_t i = 0; i < count; i++) {
            int ready = base && !broken(base, faults[i]);
            if (!strcmp(which, "repairable")) {
                assertion(faults[i], "refused", ready && check(1) > 0);
                ready = base && !broken(base, faults[i]);
                assertion(faults[i], "repaired", ready && check(0) == 0);
                assertion(faults[i], "clean", ready && check(1) == 0);
            } else {
                size_t before_n = 0, after_n = 0;
                unsigned char *before = ready ? native_read(image, &before_n) : NULL, *after;
                assertion(faults[i], "strict", ready && check(1) > 0);
                assertion(faults[i], "refused", ready && check(0) > 0);
                after = ready ? native_read(image, &after_n) : NULL;
                assertion(faults[i], "unchanged",
                          before && after && before_n == after_n &&
                              !memcmp(before, after, before_n));
                free(before);
                free(after);
            }
        }
    free(base);
}

static void directory_duplicate(void)
{
    char pattern[4096], manifest[4096], extracted[4096], text[8192];
    unsigned char *data = clean(4u << 20), bytes[65536];
    struct node nodes[MAX_NODES];
    size_t size = 0;
    int dir = -1, file = -1, ready = data != NULL, n;
    free(data);
    native_path(pattern, test.work, "pattern.bin");
    native_path(manifest, test.work, "krffsvol-test.manifest");
    native_path(extracted, test.work, "inner.out");
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (unsigned char)(i * 31 + 7);
    snprintf(text, sizeof(text),
             "file 0644 /pattern.bin %s\ntext 0644 /hello.txt hello\\n\n"
             "dir 0755 /sub\ntext 0644 /sub/inner.txt inner\\n\n",
             pattern);
    const char *args[] = {stager, manifest, image, NULL};
    ready = ready && !native_write(pattern, bytes, sizeof(bytes)) &&
            !native_write(manifest, text, strlen(text)) && native_run(&test, args) == 0;
    data = ready ? native_read(image, &size) : NULL;
    n = data ? walk(data, size, nodes) : -1;
    for (int i = 0; i < n; i++) {
        unsigned mode = (unsigned)get(data + nodes[i].off + MODE, 2);
        if (nodes[i].type != RESERVED)
            continue;
        if ((mode & 0170000) == 0040000) {
            if (dir < 0)
                dir = i;
        } else if (nodes[i].name[0] && (file < 0 || nodes[i].size > nodes[file].size))
            file = i;
    }
    ready = data && dir >= 0 && file >= 0;
    if (ready) {
        memcpy(data + nodes[file].off + NAME, data + nodes[dir].off + NAME, 256);
        put(data + nodes[file].off + PARENT, nodes[dir].parent, 8);
        ready = !native_write(image, data, size);
    }
    free(data);
    assertion("refused", "", ready && check(1) > 0);
    assertion("repaired", "", ready && check(0) == 0);
    const char *read_args[] = {reader, image, "/sub/inner.txt", extracted, NULL};
    ready = ready && native_run(&test, read_args) == 0;
    data = ready ? native_read(extracted, &size) : NULL;
    assertion("directory-kept", "", data && size == 6 && !memcmp(data, "inner\n", 6));
    free(data);
}

static void add(struct stores *s, size_t off, const void *data, size_t size)
{
    struct store *p;
    if (s->count == 32 || off > TOTAL || size > TOTAL - off)
        abort();
    p = &s->item[s->count++];
    p->off = off;
    p->size = size;
    p->bytes = malloc(size);
    if (!p->bytes)
        abort();
    memcpy(p->bytes, data, size);
}

static void number(struct stores *s, size_t off, uint64_t value, size_t size)
{
    unsigned char data[8];
    put(data, value, size);
    add(s, off, data, size);
}

static unsigned char *apply(const unsigned char *img, const struct stores *s, size_t count)
{
    unsigned char *out = malloc(TOTAL);
    if (!out)
        abort();
    memcpy(out, img, TOTAL);
    for (size_t i = 0; i < count; i++)
        memcpy(out + s->item[i].off, s->item[i].bytes, s->item[i].size);
    return out;
}

static int named(const struct node *nodes, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(nodes[i].name, name))
            return i;
    abort();
}

static void split(const unsigned char *img, struct stores *s, const char *name, size_t capacity)
{
    struct node nodes[MAX_NODES], *f;
    unsigned char h[HDR];
    int n = walk(img, TOTAL, nodes), last = -1;
    for (int i = 0; i < n; i++)
        if (nodes[i].type == FREE)
            last = i;
    if (last < 0)
        abort();
    f = &nodes[last];
    size_t want = HDR + capacity, tail = f->size - want;
    if (tail >= HDR) {
        header(h, tail, want, FREE, "", 0100644);
        add(s, f->off + want, h, HDR);
        number(s, f->off + SIZE, want, 8);
        number(s, PREV, tail, 8);
    }
    add(s, f->off + NAME, name, strlen(name) + 1);
    number(s, f->off + ID, djb(name), 8);
    number(s, f->off + PARENT, 0, 8);
    number(s, f->off + MODE, 0100644, 2);
    number(s, f->off + DATA, 0, 8);
    number(s, f->off + TYPE, RESERVED, 1);
}

static void fill(struct stores *s, size_t off, size_t start, size_t end)
{
    unsigned char *data = malloc(end - start);
    if (!data)
        abort();
    for (size_t i = start; i < end; i++)
        data[i - start] = (unsigned char)(i % 0x7e + 1);
    add(s, off + start, data, end - start);
    free(data);
}

static void resize(const unsigned char *img, struct stores *s, size_t length, int grow)
{
    struct node nodes[MAX_NODES], *p;
    unsigned char h[HDR];
    int n = walk(img, TOTAL, nodes), i = named(nodes, n, "alpha");
    p = &nodes[i];
    size_t want = HDR + length, whole = p->size + nodes[i + 1].size, tail = whole - want;
    if (grow) {
        number(s, p->off + SIZE, whole, 8);
        number(s, PREV, whole, 8);
    } else
        number(s, p->off + DATA, length, 8);
    if (tail >= HDR) {
        header(h, tail, want, FREE, "", 0100644);
        add(s, p->off + want, h, HDR);
        number(s, p->off + SIZE, want, 8);
        number(s, PREV, tail, 8);
    }
    if (grow) {
        fill(s, p->off + HDR, p->data, length);
        number(s, p->off + DATA, length, 8);
    }
}

static void remove_node(const unsigned char *img, struct stores *s, const char *name)
{
    struct node nodes[MAX_NODES], *p;
    int n = walk(img, TOTAL, nodes), i = named(nodes, n, name);
    p = &nodes[i];
    size_t size = p->size;
    number(s, p->off + TYPE, FREE, 1);
    if (i + 1 < n && nodes[i + 1].type == FREE) {
        size += nodes[i + 1].size;
        number(s, p->off + SIZE, size, 8);
    }
    if (i && nodes[i - 1].type == FREE)
        number(s, nodes[i - 1].off + SIZE, nodes[i - 1].size + size, 8);
    unsigned char *after = apply(img, s, s->count);
    n = walk(after, TOTAL, nodes);
    if (n < 1)
        abort();
    number(s, PREV, nodes[n - 1].size, 8);
    free(after);
}

static int duplicate_names(const struct node *nodes, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < i; j++)
            if (nodes[i].type == RESERVED && nodes[j].type == RESERVED && nodes[i].name[0] &&
                nodes[i].parent == nodes[j].parent && !strcmp(nodes[i].name, nodes[j].name))
                return 1;
    return 0;
}

static int shorter(const struct node *p, const struct node *before, int n)
{
    for (int j = 0; j < n; j++)
        if (before[j].type == RESERVED && p->parent == before[j].parent &&
            !strcmp(p->name, before[j].name) && p->data < before[j].data)
            return 1;
    return 0;
}

static int judge_repair(const unsigned char *img, const struct node *before, int n,
                        const char *present)
{
    struct node after[MAX_NODES];
    int m = walk(img, TOTAL, after), found = !present;
    if (m < 0 || duplicate_names(after, m))
        return 1;
    for (int i = 0; i < m; i++) {
        struct node *p = &after[i];
        if (p->type != RESERVED || !p->name[0])
            continue;
        if (p->data > p->size - HDR || memchr(img + p->off + HDR, 0xa5, p->data) ||
            shorter(p, before, n))
            return 1;
        if (present && p->parent == 0 && !strcmp(p->name, present))
            found = 1;
    }
    return !found;
}

/* A failure of execution is distinct from an image the checker rejects. */
static int judge(const unsigned char *img, const char *present)
{
    struct node before[MAX_NODES];
    int n = walk(img, TOTAL, before), status;
    unsigned char *repaired;
    size_t size;
    if (native_write(image, img, TOTAL))
        return -1;
    if (n >= 0 && duplicate_names(before, n)) {
        status = check(1);
        if (status < 0)
            return -1;
        if (!status)
            return 1;
    }
    status = check(0);
    if (status)
        return status < 0 ? -1 : 1;
    status = check(1);
    if (status)
        return status < 0 ? -1 : 1;
    repaired = native_read(image, &size);
    if (!repaired)
        return -1;
    int bad = n < 0 || size != TOTAL || judge_repair(repaired, before, n, present);
    free(repaired);
    return bad;
}

static void prefixes(const char *id, const unsigned char *img, const struct stores *s,
                     const char *present, int wrong)
{
    int verdict = 0;
    for (size_t k = 0; k <= s->count; k++) {
        unsigned char *part = apply(img, s, k);
        verdict = judge(part, present);
        free(part);
        if (verdict) {
            printf("krffscheck: %s: prefix %lu: %s\n", id, (unsigned long)k,
                   verdict < 0 ? "execution failed" : "image rejected");
            break;
        }
    }
    assertion(id, "", wrong ? verdict == 1 : verdict == 0);
}

static void relocate(const unsigned char *img, struct stores *ops, struct stores *removal)
{
    struct node nodes[MAX_NODES];
    split(img, ops, "beta", 900);
    unsigned char *after = apply(img, ops, ops->count);
    int n = walk(after, TOTAL, nodes), last = -1;
    for (int j = 0; j < n; j++)
        if (!strcmp(nodes[j].name, "beta"))
            last = j;
    if (last < 0)
        abort();
    fill(ops, nodes[last].off + HDR, 0, 900);
    number(ops, nodes[last].off + DATA, 900, 8);
    free(after);
    after = apply(img, ops, ops->count);
    remove_node(after, removal, "beta");
    free(after);
    for (size_t j = 0; j < removal->count; j++)
        add(ops, removal->item[j].off, removal->item[j].bytes, removal->item[j].size);
}

static void wrong_orders(unsigned char *images[8], const struct stores ops[7],
                         const struct stores *removal)
{
    static const char *orders[] = {"order-split", "order-length", "order-nudge", "order-shrink"};
    for (int i = 0; i < 4; i++) {
        size_t a = i == 1 ? ops[i].count - 2 : i == 3 ? 1 : 0;
        size_t b = i == 1 ? ops[i].count - 1 : i == 0 ? 1 : 2;
        struct stores reordered = ops[i];
        struct store tmp = reordered.item[a];
        reordered.item[a] = reordered.item[b];
        reordered.item[b] = tmp;
        prefixes(orders[i], images[i], &reordered, i ? "alpha" : NULL, 1);
    }
    struct stores reordered = {0};
    for (size_t j = 0; j < removal->count; j++)
        reordered.item[reordered.count++] = removal->item[j];
    for (size_t j = 0; j < ops[6].count - removal->count; j++)
        reordered.item[reordered.count++] = ops[6].item[j];
    prefixes("order-copy", images[6], &reordered, "beta", 1);
}

static void protocol(int wrong)
{
    static const char *ids[] = {"prefix-create", "prefix-grow",   "prefix-nudge",   "prefix-shrink",
                                "prefix-beside", "prefix-remove", "prefix-twonodes"};
    static const char *present[] = {NULL, "alpha", "alpha", "alpha", "alpha", "beta", "beta"};
    struct stores ops[7] = {0}, removal = {0};
    unsigned char *images[8];
    images[0] = calloc(1, TOTAL);
    if (!images[0])
        abort();
    header(images[0], HDR, TOTAL - HDR, ROOT, "", 040755);
    header(images[0] + HDR, TOTAL - HDR, HDR, FREE, "", 0100644);
    memset(images[0] + HDR * 2, 0xa5, TOTAL - HDR * 2);
    for (int i = 0; i < 7; i++) {
        if (i == 0 || i == 4)
            split(images[i], &ops[i], i ? "beta" : "alpha", i ? 200 : 100);
        else if (i < 4)
            resize(images[i], &ops[i], i == 1 ? 4000 : i == 2 ? 4030 : 50, i < 3);
        else if (i == 5)
            remove_node(images[i], &ops[i], "alpha");
        else
            relocate(images[i], &ops[i], &removal);
        images[i + 1] = apply(images[i], &ops[i], ops[i].count);
    }
    if (!wrong)
        for (int i = 0; i < 7; i++)
            prefixes(ids[i], images[i], &ops[i], present[i], 0);
    else
        wrong_orders(images, ops, &removal);
    for (int i = 0; i < 8; i++)
        free(images[i]);
    for (int i = 0; i < 7; i++)
        for (size_t j = 0; j < ops[i].count; j++)
            free(ops[i].item[j].bytes);
    for (size_t j = 0; j < removal.count; j++)
        free(removal.item[j].bytes);
}

int main(int argc, char **argv)
{
    const char *names[] = {"fresh", "repairable", "fatal",    "sizes",
                           "free",  "dirdup",     "prefixes", "orders"};
    char id[128];
    int valid = 0;
    if (argc != 2)
        return 2;
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        valid |= !strcmp(argv[1], names[i]);
    if (!valid)
        return 2;
    snprintf(id, sizeof(id), "host.krffscheck.%s", argv[1]);
    if (native_begin(&test, id))
        return 2;
    native_tool(&test, mkfs, "krffs/mkfs.krffs");
    native_tool(&test, fsck, "krffs/fsck.krffs");
    native_tool(&test, stager, "krffs/stageimage.krffs");
    native_tool(&test, reader, "krffs/readfile.krffs");
    native_path(image, test.work, "check.krffs");
    if (!strcmp(argv[1], "prefixes") || !strcmp(argv[1], "orders"))
        protocol(!strcmp(argv[1], "orders"));
    else if (!strcmp(argv[1], "dirdup"))
        directory_duplicate();
    else
        ordinary(argv[1]);
    return native_end(&test);
}
