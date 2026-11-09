/*
 * sysfs.c: the table of the /sys entries drivers add at run time and of the
 * PCI functions they report, as sysfs.h describes them.
 */

#include "sysfs.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../network/socket/netlink.h"
#include "synthfs.h"

#define SLOT_BITS 10
#define SLOT_MASK ((1 << SLOT_BITS) - 1)

typedef char sysfs_slots_fit[SYSFS_NODES_MAX <= (1 << SLOT_BITS) ? 1 : -1];

/* An entry, from the heap; `parent` is the slot of its directory, -1 for an
 * anchor, which has its fixed node and key in `node` and `key`. */
struct sysfs_node {
    int kind; /* SYSFS_NODE_* */
    int parent;
    char name[SYSFS_NAME_MAX];
    int node, key; /* an anchor's */
    uint32_t mode; /* an attribute's permission bits */
    sysfs_show_t show;
    sysfs_store_t store;
    void *obj;
    int arg;
    int target; /* a link's: the handle of its directory */
    int uevent; /* 1: a `uevent` file, whose writes send uevents */
};

static struct sysfs_node *nodes[SYSFS_NODES_MAX];
/* Each slot's generation, the handle's bits above SLOT_BITS, never 0. */
static uint32_t gens[SYSFS_NODES_MAX];

struct pci_virtual {
    int used;
    int key;
    struct sysfs_pci_ident id;
};

static struct pci_virtual pci_virtuals[SYSFS_PCI_VIRTUAL_MAX];

/* Linux's UEVENT_BUFFER_SIZE: the most bytes of one uevent. */
#define UEVENT_MAX 2048u

/* The number of the last uevent sent. */
static uint32_t uevent_seqnum;

/** The entry of `handle`, or NULL when it is gone or never was. */
static struct sysfs_node *node_of(int handle)
{
    int slot = handle & SLOT_MASK;

    if (handle <= 0 || slot >= SYSFS_NODES_MAX || !nodes[slot] ||
        (uint32_t)(handle >> SLOT_BITS) != gens[slot]) {
        return NULL;
    }
    return nodes[slot];
}

/** The handle of the entry in `slot`. */
static int handle_of(int slot)
{
    return (int)(gens[slot] << SLOT_BITS) | slot;
}

/** Whether `n` is named by the `len` bytes at `name`. */
static int named(const struct sysfs_node *n, const char *name, int len)
{
    int i;

    for (i = 0; i < len && n->name[i] == name[i]; i++) {
    }
    return i == len && n->name[len] == '\0';
}

/** Whether `n` is a directory entries may be added below. */
static int is_dir(const struct sysfs_node *n)
{
    return n && (n->kind == SYSFS_NODE_DIR || n->kind == SYSFS_NODE_ANCHOR);
}

/** Take a free slot for `n`, whose `kind` is set: its handle, or -ENOSPC. */
static int take_slot(struct sysfs_node *n)
{
    int slot;

    for (slot = 0; slot < SYSFS_NODES_MAX; slot++) {
        if (!nodes[slot]) {
            if (gens[slot] == 0 || gens[slot] >= (0x7FFFFFFFu >> SLOT_BITS)) {
                gens[slot] = 1;
            }
            nodes[slot] = n;
            return handle_of(slot);
        }
    }
    return -ENOSPC;
}

int sysfs_anchor(int node, int key)
{
    struct sysfs_node *n;
    int h = sysfs_anchor_find(node, key);

    if (h > 0) {
        return h;
    }
    n = kzalloc(sizeof(*n));
    if (!n) {
        return -ENOMEM;
    }
    n->kind = SYSFS_NODE_ANCHOR;
    n->parent = -1;
    n->node = node;
    n->key = key;
    h = take_slot(n);
    if (h < 0) {
        kfree(n);
    }
    return h;
}

int sysfs_anchor_find(int node, int key)
{
    int slot;

    for (slot = 0; slot < SYSFS_NODES_MAX; slot++) {
        if (nodes[slot] && nodes[slot]->kind == SYSFS_NODE_ANCHOR && nodes[slot]->node == node &&
            nodes[slot]->key == key) {
            return handle_of(slot);
        }
    }
    return 0;
}

/** Make an entry of `kind` named `name` below `parent`, for the caller to
 * fill: 0 with the entry in `*out` and its handle in `*handle`, or -errno. */
static int add_node(int parent, const char *name, int kind, struct sysfs_node **out, int *handle)
{
    struct sysfs_node *p = node_of(parent), *n;
    uint32_t len = name ? (uint32_t)k_strlen(name) : 0;
    int h;

    if (!is_dir(p)) {
        return -ENOENT;
    }
    if (len == 0 || len >= SYSFS_NAME_MAX) {
        return len == 0 ? -EINVAL : -ENAMETOOLONG;
    }
    if (sysfs_node_child(parent, name, (int)len) > 0) {
        return -EEXIST;
    }
    n = kzalloc(sizeof(*n));
    if (!n) {
        return -ENOMEM;
    }
    n->kind = kind;
    n->parent = parent & SLOT_MASK;
    k_memcpy(n->name, name, len + 1);
    h = take_slot(n);
    if (h < 0) {
        kfree(n);
        return h;
    }
    *out = n;
    *handle = h;
    return 0;
}

int sysfs_add_dir(int parent, const char *name)
{
    struct sysfs_node *n;
    int h, rc = add_node(parent, name, SYSFS_NODE_DIR, &n, &h);

    return rc < 0 ? rc : h;
}

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    struct sysfs_node *n;
    int h, rc = add_node(parent, attr->name, SYSFS_NODE_ATTR, &n, &h);

    if (rc < 0) {
        return rc;
    }
    n->mode = attr->mode & 0777u;
    n->show = attr->show;
    n->store = attr->store;
    n->obj = obj;
    n->arg = arg;
    return h;
}

int sysfs_add_uevent(int dir, sysfs_show_t show, void *obj, int arg)
{
    struct sysfs_attr attr;
    int h;

    attr.name = "uevent";
    attr.mode = 0644u;
    attr.show = show;
    attr.store = NULL;
    h = sysfs_add_attr(dir, &attr, obj, arg);

    if (h > 0) {
        node_of(h)->uevent = 1;
    }
    return h;
}

int sysfs_add_link(int parent, const char *name, int target)
{
    struct sysfs_node *n;
    int h, rc;

    if (!is_dir(node_of(target))) {
        return -ENOENT;
    }
    rc = add_node(parent, name, SYSFS_NODE_LINK, &n, &h);
    if (rc < 0) {
        return rc;
    }
    n->target = target;
    return h;
}

void sysfs_remove(int handle)
{
    struct sysfs_node *n = node_of(handle);
    int slot = handle & SLOT_MASK, i;

    if (!n || n->kind == SYSFS_NODE_ANCHOR) {
        return;
    }
    for (i = 0; i < SYSFS_NODES_MAX; i++) {
        if (nodes[i] && nodes[i]->parent == slot) {
            sysfs_remove(handle_of(i));
        }
    }
    nodes[slot] = NULL;
    gens[slot]++;
    kfree(n);
}

int sysfs_node_kind(int handle)
{
    struct sysfs_node *n = node_of(handle);

    return n ? n->kind : 0;
}

int sysfs_node_child(int dir, const char *name, int len)
{
    int slot, parent = dir & SLOT_MASK;

    if (!is_dir(node_of(dir)) || len <= 0 || len >= SYSFS_NAME_MAX) {
        return 0;
    }
    for (slot = 0; slot < SYSFS_NODES_MAX; slot++) {
        struct sysfs_node *n = nodes[slot];

        if (n && n->parent == parent && named(n, name, len)) {
            return handle_of(slot);
        }
    }
    return 0;
}

int sysfs_node_count(int dir)
{
    int slot, count = 0, parent = dir & SLOT_MASK;

    if (!is_dir(node_of(dir))) {
        return 0;
    }
    for (slot = 0; slot < SYSFS_NODES_MAX; slot++) {
        if (nodes[slot] && nodes[slot]->parent == parent) {
            count++;
        }
    }
    return count;
}

int sysfs_node_nth(int dir, int n)
{
    int slot, parent = dir & SLOT_MASK;

    if (!is_dir(node_of(dir)) || n < 0) {
        return 0;
    }
    for (slot = 0; slot < SYSFS_NODES_MAX; slot++) {
        if (nodes[slot] && nodes[slot]->parent == parent && n-- == 0) {
            return handle_of(slot);
        }
    }
    return 0;
}

const char *sysfs_node_name(int handle)
{
    struct sysfs_node *n = node_of(handle);

    return n ? n->name : "";
}

int sysfs_node_parent(int handle)
{
    struct sysfs_node *n = node_of(handle);

    return n && n->parent >= 0 ? handle_of(n->parent) : 0;
}

void sysfs_node_anchor(int handle, int *node, int *key)
{
    struct sysfs_node *n = node_of(handle);
    int anchor = n && n->kind == SYSFS_NODE_ANCHOR;

    *node = anchor ? n->node : -1;
    *key = anchor ? n->key : 0;
}

int sysfs_node_target(int handle)
{
    struct sysfs_node *n = node_of(handle);

    return n && n->kind == SYSFS_NODE_LINK ? n->target : 0;
}

uint32_t sysfs_node_mode(int handle)
{
    struct sysfs_node *n = node_of(handle);

    return n && n->kind == SYSFS_NODE_ATTR ? n->mode : 0;
}

int sysfs_node_writable(int handle)
{
    struct sysfs_node *n = node_of(handle);

    return n && n->kind == SYSFS_NODE_ATTR && (n->store || n->uevent) && (n->mode & 0222u);
}

int32_t sysfs_node_show(int handle, char *buf, uint32_t cap)
{
    struct sysfs_node *n = node_of(handle);

    if (!n || n->kind != SYSFS_NODE_ATTR) {
        return -ENOENT;
    }
    return n->show ? n->show(n->obj, n->arg, buf, cap) : -EACCES;
}

int32_t sysfs_node_store(int handle, const char *buf, uint32_t len)
{
    struct sysfs_node *n = node_of(handle);

    if (!n || n->kind != SYSFS_NODE_ATTR) {
        return -ENOENT;
    }
    if (n->uevent) {
        return sysfs_uevent_write(handle_of(n->parent), buf, len);
    }
    return n->store ? n->store(n->obj, n->arg, buf, len) : -EACCES;
}

void sysfs_node_path(int handle, char *out, int cap)
{
    struct sysfs_node *n = node_of(handle);
    const char *names[16];
    int depth = 0, pos, slot = handle & SLOT_MASK;
    uint32_t len;

    if (cap <= 0) {
        return;
    }
    while (n && n->kind != SYSFS_NODE_ANCHOR && depth < 16) {
        names[depth++] = n->name;
        slot = n->parent;
        n = slot >= 0 ? nodes[slot] : NULL;
    }
    if (!n || n->kind != SYSFS_NODE_ANCHOR) {
        k_strncpy(out, "/sys", (uint32_t)cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    synth_sys_path(n->node, n->key, out, cap);
    pos = (int)k_strlen(out);
    while (depth > 0) {
        const char *s = names[--depth];

        len = (uint32_t)k_strlen(s);
        if (pos + 1 + (int)len >= cap) {
            break;
        }
        out[pos++] = '/';
        k_memcpy(out + pos, s, len);
        pos += (int)len;
    }
    out[pos] = '\0';
}

int sysfs_link_text(int handle, char *out, int cap)
{
    char from[256], to[256];
    int f = 0, t = 0, common = 0, n = 0, i;
    struct sysfs_node *link = node_of(handle);

    if (!link || link->kind != SYSFS_NODE_LINK || !is_dir(node_of(link->target))) {
        return -ENOENT;
    }
    sysfs_node_path(handle_of(link->parent), from, (int)sizeof(from));
    sysfs_node_path(link->target, to, (int)sizeof(to));
    /* The longest run of whole components the two paths share. */
    while (from[f] != '\0' && from[f] == to[t]) {
        f++;
        t++;
        if ((from[f] == '/' || from[f] == '\0') && (to[t] == '/' || to[t] == '\0')) {
            common = f;
        }
    }
    t = common;
    for (i = common; from[i] != '\0'; i++) {
        if (from[i] != '/') {
            continue;
        }
        if (n > 0 && n < cap) {
            out[n++] = '/';
        }
        if (n + 2 <= cap) {
            out[n++] = '.';
            out[n++] = '.';
        }
    }
    if (to[t] == '/') {
        t++;
    }
    if (to[t] != '\0' && n > 0 && n < cap) {
        out[n++] = '/';
    }
    for (; to[t] != '\0' && n < cap; t++) {
        out[n++] = to[t];
    }
    return n;
}

int sysfs_pci_virtual_add(int key, const struct sysfs_pci_ident *id)
{
    int i, free = -1;

    for (i = 0; i < SYSFS_PCI_VIRTUAL_MAX; i++) {
        if (pci_virtuals[i].used && pci_virtuals[i].key == key) {
            return -EEXIST;
        }
        if (!pci_virtuals[i].used && free < 0) {
            free = i;
        }
    }
    if (free < 0) {
        return -ENOSPC;
    }
    pci_virtuals[free].used = 1;
    pci_virtuals[free].key = key;
    pci_virtuals[free].id = *id;
    return 0;
}

void sysfs_pci_virtual_remove(int key)
{
    int i;

    for (i = 0; i < SYSFS_PCI_VIRTUAL_MAX; i++) {
        if (pci_virtuals[i].used && pci_virtuals[i].key == key) {
            pci_virtuals[i].used = 0;
        }
    }
}

int sysfs_pci_virtual(int key, struct sysfs_pci_ident *id)
{
    int i;

    for (i = 0; i < SYSFS_PCI_VIRTUAL_MAX; i++) {
        if (pci_virtuals[i].used && pci_virtuals[i].key == key) {
            if (id) {
                *id = pci_virtuals[i].id;
            }
            return 1;
        }
    }
    return 0;
}

int sysfs_pci_virtual_nth(int n)
{
    int i;

    for (i = 0; i < SYSFS_PCI_VIRTUAL_MAX; i++) {
        if (pci_virtuals[i].used && n-- == 0) {
            return pci_virtuals[i].key;
        }
    }
    return -1;
}

/* A uevent being built: its bytes and their count, and whether one did not
 * fit. */
struct uevent_msg {
    char *buf;
    uint32_t len;
    int full;
};

/** Append the `n` bytes at `s` to `m`. */
static void ue_put(struct uevent_msg *m, const char *s, uint32_t n)
{
    if (m->full || n > UEVENT_MAX - m->len) {
        m->full = 1;
        return;
    }
    k_memcpy(m->buf + m->len, s, n);
    m->len += n;
}

/** Append the string `s` to `m`. */
static void ue_str(struct uevent_msg *m, const char *s)
{
    ue_put(m, s, (uint32_t)k_strlen(s));
}

/** Append the decimal `v` to `m`. */
static void ue_u32(struct uevent_msg *m, uint32_t v)
{
    char digits[11];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n > 0) {
        ue_put(m, &digits[--n], 1);
    }
}

/** Append the `len` bytes of `text`, lines of `KEY=value`, to `m` as
 * variables, each ended by a NUL; empty lines make none. */
static void ue_lines(struct uevent_msg *m, const char *text, uint32_t len)
{
    uint32_t start = 0, i;

    for (i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n' || text[i] == '\0') {
            if (i > start) {
                ue_put(m, text + start, i - start);
                ue_put(m, "", 1);
            }
            start = i + 1;
        }
    }
}

int32_t sysfs_uevent(int dir, const char *action, const char *env)
{
    struct uevent_msg m;
    char path[256], link[256];
    int sub, attr, n, i;
    int32_t got;

    if (!is_dir(node_of(dir))) {
        return -ENOENT;
    }
    m.buf = kmalloc(2 * UEVENT_MAX);
    if (!m.buf) {
        return -ENOMEM;
    }
    m.len = 0;
    m.full = 0;
    sysfs_node_path(dir, path, (int)sizeof(path));
    ue_str(&m, action);
    ue_str(&m, "@");
    ue_str(&m, path + 4);
    ue_put(&m, "", 1);
    ue_str(&m, "ACTION=");
    ue_str(&m, action);
    ue_put(&m, "", 1);
    ue_str(&m, "DEVPATH=");
    ue_str(&m, path + 4);
    ue_put(&m, "", 1);
    sub = sysfs_node_child(dir, "subsystem", 9);
    n = sub > 0 ? sysfs_link_text(sub, link, (int)sizeof(link) - 1) : -1;
    if (n > 0) {
        for (i = n; i > 0 && link[i - 1] != '/'; i--) {
        }
        link[n] = '\0';
        ue_str(&m, "SUBSYSTEM=");
        ue_str(&m, link + i);
        ue_put(&m, "", 1);
    }
    if (env) {
        ue_lines(&m, env, (uint32_t)k_strlen(env));
    }
    attr = sysfs_node_child(dir, "uevent", 6);
    got = attr > 0 ? sysfs_node_show(attr, m.buf + UEVENT_MAX, UEVENT_MAX) : 0;
    if (got > (int32_t)UEVENT_MAX) {
        m.full = 1;
    } else if (got > 0) {
        ue_lines(&m, m.buf + UEVENT_MAX, (uint32_t)got);
    }
    ue_str(&m, "SEQNUM=");
    ue_u32(&m, uevent_seqnum + 1);
    ue_put(&m, "", 1);
    if (m.full) {
        kfree(m.buf);
        return -E2BIG;
    }
    uevent_seqnum++;
    netlink_uevent((const uint8_t *)m.buf, m.len);
    kfree(m.buf);
    return 0;
}

/** Whether the `len` bytes at `s` are the word `w`. */
static int word_is(const char *s, uint32_t len, const char *w)
{
    uint32_t i;

    for (i = 0; i < len && w[i] == s[i]; i++) {
    }
    return i == len && w[i] == '\0';
}

int32_t sysfs_uevent_write(int dir, const char *buf, uint32_t len)
{
    static const char *const actions[] = {"add",    "remove",  "change", "move",
                                          "online", "offline", "bind",   "unbind"};
    char env[256], action[8];
    uint32_t at = 0, start, n = 0, i, words = 0, plen;
    int a = -1;

    while (at < len) {
        while (at < len && (buf[at] == ' ' || buf[at] == '\n' || buf[at] == '\t')) {
            at++;
        }
        start = at;
        while (at < len && buf[at] != ' ' && buf[at] != '\n' && buf[at] != '\t') {
            at++;
        }
        if (at == start) {
            break;
        }
        if (words == 0) {
            for (i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
                if (word_is(buf + start, at - start, actions[i])) {
                    a = (int)i;
                }
            }
            if (a < 0) {
                return -EINVAL;
            }
        } else {
            const char *prefix = words == 1 ? "SYNTH_UUID=" : "SYNTH_ARG_";

            if (words == 1 && at - start != 36) {
                return -EINVAL;
            }
            plen = (uint32_t)k_strlen(prefix);
            if (n + plen + (at - start) + 2 > sizeof(env)) {
                return -EINVAL;
            }
            k_memcpy(env + n, prefix, plen);
            n += plen;
            k_memcpy(env + n, buf + start, at - start);
            n += at - start;
            env[n++] = '\n';
        }
        words++;
    }
    if (a < 0) {
        return -EINVAL;
    }
    if (words == 1) {
        k_memcpy(env, "SYNTH_UUID=0\n", 13);
        n = 13;
    }
    env[n] = '\0';
    k_strncpy(action, actions[a], sizeof(action) - 1);
    action[sizeof(action) - 1] = '\0';
    i = (uint32_t)sysfs_uevent(dir, action, env);
    return (int32_t)i < 0 ? (int32_t)i : (int32_t)len;
}

uint32_t sysfs_uevent_seqnum(void)
{
    return uevent_seqnum;
}
