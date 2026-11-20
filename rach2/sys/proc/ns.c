/*
 * ns.c: namespaces, and the calls that make and join them.
 *
 * A process keeps its user namespace, its PID namespace and its numbers in
 * the PID namespaces it is in on its own record, and its other namespaces in
 * a `struct nsset` that the processes it makes share until one of them
 * changes it. A namespace counts the records and the sets that refer to it,
 * and each namespace has a reference to the user namespace that owns it, and
 * a user or PID namespace to its parent. The first namespaces are static,
 * and a NULL pointer in a record stands for the first of its kind, or for
 * the set of the first ones.
 */

#include "ns.h"
#include "pidfd.h"
#include "tgroup.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../io/anon.h"
#include "../ipc/mqueue.h"
#include "../lib/idset.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"

/* The first namespaces have the inode numbers Linux gives its first ones,
 * and the others are numbered upward from NS_FIRST_DYNAMIC. */
#define NS_FIRST_DYNAMIC 0xF0000100u

/* The highest user.max_mnt_namespaces: INT_MAX, which a user namespace
 * other than the first starts with. */
#define NS_LIMIT_MAX 0x7FFFFFFF

/* The stack of a thread on Linux, of whose count for the machine's memory
 * the first user namespace's max_mnt is half, and the bounds of that count,
 * as fork.c's set_max_threads takes them. */
#if defined __x86_64__
#define NS_THREAD_SIZE 16384u
#else
#define NS_THREAD_SIZE 8192u
#endif
#define NS_MIN_THREADS 20u
#define NS_MAX_THREADS 0x3FFFFFFFu

/* The open flag that marks a descriptor close-on-exec. */
#define NS_O_CLOEXEC 02000000u

/* The clone flags beside the namespaces' that clone and unshare check. */
#define CLONE_VM      0x00000100u
#define CLONE_FS      0x00000200u
#define CLONE_FILES   0x00000400u
#define CLONE_SIGHAND 0x00000800u
#define CLONE_THREAD  0x00010000u
#define CLONE_SYSVSEM 0x00040000u

int ns_overflowuid = NS_OVERFLOW_ID;
int ns_overflowgid = NS_OVERFLOW_ID;

struct userns init_userns = {{1, 0xEFFFFFFDu, NS_USER, NULL},
                             NULL,
                             0,
                             0,
                             0,
                             {1, {{0, 0, 0xFFFFFFFFu}}, NULL, NULL},
                             {1, {{0, 0, 0xFFFFFFFFu}}, NULL, NULL},
                             0,
                             NS_LIMIT_UNSET,
                             0};
struct pidns init_pidns = {{1, 0xEFFFFFFCu, NS_PID, NULL}, NULL, 0, 0, 1, 0, 1};
struct mntns init_mntns = {{1, 0xF0000000u, NS_MNT, NULL}, 0, 0, NULL, NULL};
struct netns init_netns = {{1, 0xF0000098u, NS_NET, NULL}, NULL};
struct utsns init_utsns = {{1, 0xEFFFFFFEu, NS_UTS, NULL}, "rach2", "(none)"};
struct ipcns init_ipcns = {{1, 0xEFFFFFFFu, NS_IPC, NULL}, NULL};

/* The set of the first namespaces, which a record without a set uses. */
static struct nsset init_nsset = {1,           &init_mntns, &init_netns,
                                  &init_utsns, &init_ipcns, &init_pidns};

/* The inode number the next namespace made is given. */
static uint32_t next_inum = NS_FIRST_DYNAMIC;

/* The ids of the mount namespaces that exist, the first's 0, and the list
 * of those but the first. */
static struct idset mnt_ids = {1u, NULL, 0, 1};
static struct mntns *mnt_list;

static const char *const kind_names[NS_KINDS] = {"user", "pid", "mnt", "net", "uts", "ipc"};

static const uint32_t kind_flags[NS_KINDS] = {CLONE_NEWUSER, CLONE_NEWPID, CLONE_NEWNS,
                                              CLONE_NEWNET,  CLONE_NEWUTS, CLONE_NEWIPC};

/** Whether `c` is one of the first namespaces, which are never freed. */
static int ns_first(const struct ns_common *c)
{
    return c == &init_userns.c || c == &init_pidns.c || c == &init_mntns.c || c == &init_netns.c ||
           c == &init_utsns.c || c == &init_ipcns.c;
}

/** Take a reference to `c`. */
static void ns_get(struct ns_common *c)
{
    c->refs++;
}

/** Free the copies from the heap of the map `m`. */
static void idmap_free(struct ns_idmap *m)
{
    kfree(m->forward);
    kfree(m->reverse);
    m->forward = m->reverse = NULL;
}

/** The end of the mount namespace `m`: its mounts go, then its id, which a
 * namespace made meanwhile cannot take, and its place in the list. */
static void mntns_gone(struct mntns *m)
{
    vfs_mntns_gone(m->id);
    idset_del(&mnt_ids, m->id);
    if (m->prev) {
        m->prev->next = m->next;
    } else {
        mnt_list = m->next;
    }
    if (m->next) {
        m->next->prev = m->prev;
    }
}

void ns_put(struct ns_common *c)
{
    if (!c || ns_first(c) || --c->refs != 0) {
        return;
    }
    switch (c->kind) {
    case NS_USER:
        idmap_free(&((struct userns *)c)->uid_map);
        idmap_free(&((struct userns *)c)->gid_map);
        ns_put(&((struct userns *)c)->parent->c);
        break;
    case NS_PID:
        ns_put(&((struct pidns *)c)->parent->c);
        break;
    case NS_MNT:
        mntns_gone((struct mntns *)c);
        break;
    case NS_NET:
        net_ns_gone((struct netns *)c);
        kfree(((struct netns *)c)->lo);
        break;
    case NS_IPC:
        ipc_ns_gone((struct ipcns *)c);
        break;
    default:
        break;
    }
    ns_put(&c->owner->c);
    kfree(c);
}

/** A new namespace of `size` bytes and kind `kind`, owned by `owner`, with
 * one reference and the next inode number; NULL when the heap is full. */
static void *ns_new(uint32_t size, int kind, struct userns *owner)
{
    struct ns_common *c = kzalloc(size);

    if (!c) {
        return NULL;
    }
    c->refs = 1;
    c->inum = next_inum++;
    c->kind = (uint32_t)kind;
    c->owner = owner;
    ns_get(&owner->c);
    return c;
}

struct userns *proc_userns(const process_t *p)
{
    return p && p->userns ? p->userns : &init_userns;
}

struct pidns *proc_pidns(const process_t *p)
{
    return p && p->pidns ? p->pidns : &init_pidns;
}

/** The set of `p`'s other namespaces, the first set where it has none. */
static struct nsset *proc_nsset(const process_t *p)
{
    return p && p->nsset ? p->nsset : &init_nsset;
}

struct mntns *proc_mntns(const process_t *p)
{
    return proc_nsset(p)->mnt;
}

struct netns *proc_netns(const process_t *p)
{
    return proc_nsset(p)->net;
}

struct utsns *proc_utsns(const process_t *p)
{
    return proc_nsset(p)->uts;
}

struct ipcns *proc_ipcns(const process_t *p)
{
    return proc_nsset(p)->ipc;
}

/* ---- ids ---- */

/** Extent `i` of the map `m` in the order /proc/PID/uid_map lists them. */
static const struct ns_extent *idmap_at(const struct ns_idmap *m, uint32_t i)
{
    return m->extents <= NS_BASE_EXTENTS ? &m->base[i] : &m->forward[i];
}

/**
 * The extent of `m` that holds all the `count` ids from `id` in the
 * namespace, or NULL when no one extent does, as Linux's map_id_range_down
 * finds it: the extents in the order written, or by halves in the copy
 * sorted by `first`.
 */
static const struct ns_extent *idmap_down(const struct ns_idmap *m, uint32_t id, uint32_t count)
{
    uint32_t last = id + count - 1u, lo = 0, hi = m->extents, i;
    const struct ns_extent *e;

    if (m->extents <= NS_BASE_EXTENTS) {
        for (i = 0; i < m->extents; i++) {
            e = &m->base[i];
            if (id - e->first < e->count && last - e->first < e->count) {
                return e;
            }
        }
        return NULL;
    }
    while (lo < hi) {
        i = lo + (hi - lo) / 2u;
        e = &m->forward[i];
        if (id < e->first) {
            hi = i;
        } else if (id - e->first >= e->count) {
            lo = i + 1u;
        } else {
            return last - e->first < e->count ? e : NULL;
        }
    }
    return NULL;
}

/** The extent of `m` that holds the kernel's id `kid`, or NULL, as Linux's
 * map_id_up finds it: in the order written, or by halves in the copy
 * sorted by `lower`. */
static const struct ns_extent *idmap_up(const struct ns_idmap *m, uint32_t kid)
{
    uint32_t lo = 0, hi = m->extents, i;
    const struct ns_extent *e;

    if (m->extents <= NS_BASE_EXTENTS) {
        for (i = 0; i < m->extents; i++) {
            if (kid - m->base[i].lower < m->base[i].count) {
                return &m->base[i];
            }
        }
        return NULL;
    }
    while (lo < hi) {
        i = lo + (hi - lo) / 2u;
        e = &m->reverse[i];
        if (kid < e->lower) {
            hi = i;
        } else if (kid - e->lower >= e->count) {
            lo = i + 1u;
        } else {
            return e;
        }
    }
    return NULL;
}

/** The kernel's id of `id` in `ns` by the map `m`, or -1 for none. */
static int64_t id_down(const struct userns *ns, const struct ns_idmap *m, uint32_t id)
{
    const struct ns_extent *e;

    if (ns == &init_userns) {
        return id;
    }
    e = idmap_down(m, id, 1u);
    return e ? (int64_t)(e->lower + (id - e->first)) : -1;
}

/** What `ns` sees of the kernel's id `kid` by the map `m`, or `overflow`. */
static uint32_t id_up(const struct userns *ns, const struct ns_idmap *m, uint32_t kid, int overflow)
{
    const struct ns_extent *e;

    if (ns == &init_userns) {
        return kid;
    }
    e = idmap_up(m, kid);
    return e ? e->first + (kid - e->lower) : (uint32_t)overflow;
}

int64_t ns_uid_to_kernel(const struct userns *ns, uint32_t id)
{
    return id_down(ns, &ns->uid_map, id);
}

int64_t ns_gid_to_kernel(const struct userns *ns, uint32_t id)
{
    return id_down(ns, &ns->gid_map, id);
}

uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t kid)
{
    return id_up(ns, &ns->uid_map, kid, ns_overflowuid);
}

uint32_t ns_gid_from_kernel(const struct userns *ns, uint32_t kid)
{
    return id_up(ns, &ns->gid_map, kid, ns_overflowgid);
}

int ns_uid_mapped(const struct userns *ns, uint32_t uid)
{
    return ns == &init_userns || idmap_up(&ns->uid_map, uid) != NULL;
}

int ns_ids_mapped(const struct userns *ns, uint32_t uid, uint32_t gid)
{
    return ns == &init_userns || (idmap_up(&ns->uid_map, uid) && idmap_up(&ns->gid_map, gid));
}

uint32_t uid_seen(uint32_t kid)
{
    return ns_uid_from_kernel(proc_userns(current_proc), kid);
}

uint32_t gid_seen(uint32_t kid)
{
    return ns_gid_from_kernel(proc_userns(current_proc), kid);
}

int64_t uid_named(uint32_t id)
{
    return ns_uid_to_kernel(proc_userns(current_proc), id);
}

int64_t gid_named(uint32_t id)
{
    return ns_gid_to_kernel(proc_userns(current_proc), id);
}

/* ---- /proc/PID/uid_map, gid_map and setgroups ---- */

/** Whether setgroups is denied in `ns`: by a write of "deny" to it or to an
 * ancestor, since a namespace made below one that denies it denies it too,
 * as on Linux, where bubblewrap's second namespace counts on that. */
static int userns_denies_setgroups(const struct userns *ns)
{
    for (; ns && ns != &init_userns; ns = ns->parent) {
        if (ns->setgroups_denied) {
            return 1;
        }
    }
    return 0;
}

/** Append `v` right-aligned in ten columns to `out` at `*at`, stopping
 * short of `out[cap - 1]`, the NUL's place. */
static void put_col(char *out, int cap, int *at, uint32_t v)
{
    char digits[10];
    int n = 0, i;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    for (i = n; i < 10 && *at < cap - 1; i++) {
        out[(*at)++] = ' ';
    }
    while (n > 0 && *at < cap - 1) {
        out[(*at)++] = digits[--n];
    }
}

int ns_map_text(const struct userns *ns, int which, const process_t *reader, char *out, int cap)
{
    const struct userns *own = proc_userns(reader);
    const struct ns_idmap *map = which == NS_MAP_GID ? &ns->gid_map : &ns->uid_map;
    uint32_t i;
    int at = 0;

    if (own == ns && ns->parent) {
        own = ns->parent;
    }
    if (which == NS_MAP_SETGROUPS) {
        const char *word = userns_denies_setgroups(ns) ? "deny\n" : "allow\n";

        while (*word && at < cap - 1) {
            out[at++] = *word++;
        }
        out[at] = '\0';
        return at;
    }
    for (i = 0; i < map->extents; i++) {
        const struct ns_extent *e = idmap_at(map, i);
        uint32_t lower = which == NS_MAP_GID ? ns_gid_from_kernel(own, e->lower)
                                             : ns_uid_from_kernel(own, e->lower);

        put_col(out, cap, &at, e->first);
        if (at < cap - 1) {
            out[at++] = ' ';
        }
        put_col(out, cap, &at, lower);
        if (at < cap - 1) {
            out[at++] = ' ';
        }
        put_col(out, cap, &at, e->count);
        if (at < cap - 1) {
            out[at++] = '\n';
        }
    }
    out[at] = '\0';
    return at;
}

/** Read an unsigned number of at most ten digits at `*p` into `*v`, past
 * the spaces and tabs before it, and step `*p` past it; 1, or 0 for no
 * digit or more than ten. Ten digits past 4294967295 wrap. */
static int read_number(const char **p, const char *end, uint32_t *v)
{
    uint32_t n = 0;
    int digits = 0;

    while (*p < end && (**p == ' ' || **p == '\t')) {
        (*p)++;
    }
    while (*p < end && **p >= '0' && **p <= '9' && digits < 10) {
        n = n * 10u + (uint32_t)(**p - '0');
        (*p)++;
        digits++;
    }
    *v = n;
    return digits > 0 && (*p >= end || **p < '0' || **p > '9');
}

/** Whether the extent `e` overlaps one of the `n` extents of `map`, in
 * the namespace's ids or in its parent's, as Linux's mappings_overlap. */
static int extent_overlaps(const struct ns_extent *map, int n, const struct ns_extent *e)
{
    int i;

    for (i = 0; i < n; i++) {
        if ((e->first < map[i].first + map[i].count && map[i].first < e->first + e->count) ||
            (e->lower < map[i].lower + map[i].count && map[i].lower < e->lower + e->count)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Parse the lines of a map written to `text`, at most NS_EXTENTS of three
 * numbers each, the ids in the namespace, the ids in its parent and the
 * count, into `map`, of NS_EXTENTS entries, with the parent's ids as the
 * parent names them. The number of extents, or -EINVAL for text of another
 * form, an empty or wrapping range, two that overlap, or a line past
 * NS_EXTENTS.
 */
static int parse_map(const char *text, uint32_t len, struct ns_extent *map)
{
    const char *p = text, *end = text + len;
    int n = 0;

    while (p < end) {
        struct ns_extent e;

        if (*p == '\n') {
            p++;
            continue;
        }
        if (n == NS_EXTENTS || !read_number(&p, end, &e.first) || !read_number(&p, end, &e.lower) ||
            !read_number(&p, end, &e.count) || e.count == 0 || e.first + e.count - 1u < e.first ||
            e.lower + e.count - 1u < e.lower || extent_overlaps(map, n, &e)) {
            return -EINVAL;
        }
        map[n++] = e;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p < end && *p != '\n') {
            return -EINVAL;
        }
    }
    return n == 0 ? -EINVAL : n;
}

/** Make the parent's ids of the `n` extents of `map` the kernel's, through
 * `parent`'s map `pmap`: 0, or -EPERM for an extent whose parent ids no one
 * extent of `pmap` holds, as Linux refuses one. */
static int map_lower(const struct userns *parent, const struct ns_idmap *pmap,
                     struct ns_extent *map, int n)
{
    const struct ns_extent *e;
    int i;

    if (parent == &init_userns) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        e = idmap_down(pmap, map[i].lower, map[i].count);
        if (!e) {
            return -EPERM;
        }
        map[i].lower = e->lower + (map[i].lower - e->first);
    }
    return 0;
}

/** Sort the `n` extents of `map` by `first`, or by `lower` when `by_lower`
 * is set; no two have a key in common. */
static void extents_sort(struct ns_extent *map, uint32_t n, int by_lower)
{
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        struct ns_extent e = map[i];
        uint32_t key = by_lower ? e.lower : e.first;

        for (j = i; j > 0 && (by_lower ? map[j - 1u].lower : map[j - 1u].first) > key; j--) {
            map[j] = map[j - 1u];
        }
        map[j] = e;
    }
}

/** Store the `n` extents of `map` in the empty map `dst`: in its record up
 * to NS_BASE_EXTENTS, and past that in the two sorted copies of Linux's
 * sort_idmaps. 0, or -ENOMEM, which leaves `dst` empty. */
static int idmap_store(struct ns_idmap *dst, const struct ns_extent *map, uint32_t n)
{
    if (n <= NS_BASE_EXTENTS) {
        k_memcpy(dst->base, map, sizeof(map[0]) * n);
        dst->extents = n;
        return 0;
    }
    dst->forward = kmalloc(sizeof(map[0]) * n);
    dst->reverse = kmalloc(sizeof(map[0]) * n);
    if (!dst->forward || !dst->reverse) {
        idmap_free(dst);
        return -ENOMEM;
    }
    k_memcpy(dst->forward, map, sizeof(map[0]) * n);
    k_memcpy(dst->reverse, map, sizeof(map[0]) * n);
    extents_sort(dst->forward, n, 0);
    extents_sort(dst->reverse, n, 1);
    dst->extents = n;
    return 0;
}

/** Whether the `len` bytes of `text` begin with `word`. */
static int text_begins(const char *text, uint32_t len, const char *word)
{
    uint32_t i;

    for (i = 0; word[i]; i++) {
        if (i >= len || text[i] != word[i]) {
            return 0;
        }
    }
    return 1;
}

/** A write of /proc/PID/setgroups: "deny", or "allow" when setgroups was
 * not denied, before the group map is written. `len`, or -EPERM, or -EINVAL
 * for another word. */
static int32_t setgroups_write(struct userns *ns, const char *text, uint32_t len)
{
    if (ns->gid_map.extents != 0) {
        return -EPERM;
    }
    if (text_begins(text, len, "deny")) {
        ns->setgroups_denied = 1;
        return (int32_t)len;
    }
    if (text_begins(text, len, "allow")) {
        return userns_denies_setgroups(ns) ? -EPERM : (int32_t)len;
    }
    return -EINVAL;
}

/** Whether `writer`, which is not privileged in the parent of `ns`, may
 * write the `n` extents of `map` to the map `which` of `ns`: one id, its
 * own effective one, and a group only after setgroups was denied. */
static int map_unprivileged_ok(const struct userns *ns, int which, const struct ns_extent *map,
                               int n, const process_t *writer)
{
    int64_t k;

    if (n != 1 || map[0].count != 1) {
        return 0;
    }
    if (which == NS_MAP_GID) {
        k = ns_gid_to_kernel(ns->parent, map[0].lower);
        return k == (int64_t)writer->egid && userns_denies_setgroups(ns);
    }
    k = ns_uid_to_kernel(ns->parent, map[0].lower);
    return k == (int64_t)writer->euid;
}

int32_t ns_map_write(struct userns *ns, int which, const char *text, uint32_t len,
                     const process_t *writer)
{
    struct ns_idmap *dst = which == NS_MAP_GID ? &ns->gid_map : &ns->uid_map;
    const struct ns_idmap *pmap = which == NS_MAP_GID ? &ns->parent->gid_map : &ns->parent->uid_map;
    struct ns_extent *map;
    int n, rc;

    if (ns == &init_userns || !ns_privileged(writer, ns)) {
        return -EPERM;
    }
    if (which == NS_MAP_SETGROUPS) {
        return setgroups_write(ns, text, len);
    }
    if (dst->extents != 0) {
        return -EPERM;
    }
    if (proc_userns(writer) != ns && proc_userns(writer) != ns->parent) {
        return -EPERM;
    }
    map = kmalloc(sizeof(*map) * NS_EXTENTS);
    if (!map) {
        return -ENOMEM;
    }
    n = parse_map(text, len, map);
    rc = n < 0 ? n : 0;
    if (rc == 0 && !ns_privileged(writer, ns->parent) &&
        !map_unprivileged_ok(ns, which, map, n, writer)) {
        rc = -EPERM;
    }
    if (rc == 0) {
        rc = map_lower(ns->parent, pmap, map, n);
    }
    if (rc == 0) {
        rc = idmap_store(dst, map, (uint32_t)n);
    }
    kfree(map);
    return rc != 0 ? rc : (int32_t)len;
}

struct userns *ns_exec_owner(const process_t *p, int unreadable, uint32_t uid, uint32_t gid)
{
    struct userns *ns = proc_userns(p);

    while (unreadable && ns != &init_userns && !ns_ids_mapped(ns, uid, gid)) {
        ns = ns->parent;
    }
    return ns_userns_get(ns == &init_userns ? NULL : ns);
}

struct userns *ns_userns_get(struct userns *ns)
{
    if (ns) {
        ns_get(&ns->c);
    }
    return ns;
}

void ns_userns_put(struct userns *ns)
{
    if (ns) {
        ns_put(&ns->c);
    }
}

int ns_setgroups_denied(const process_t *p)
{
    return userns_denies_setgroups(proc_userns(p));
}

/* ---- privilege ---- */

int ns_capable_with(const process_t *p, const struct userns *ns, int held)
{
    const struct userns *own = proc_userns(p);

    if (!ns) {
        ns = &init_userns;
    }
    for (;;) {
        if (ns == own) {
            return held;
        }
        if (ns->level <= own->level) {
            return 0;
        }
        if (ns->parent == own && ns->owner_uid == p->euid) {
            return 1;
        }
        ns = ns->parent;
    }
}

int ns_capable(const process_t *p, const struct userns *ns, uint32_t cap)
{
    return ns_capable_with(p, ns, cap_holds(p, cap));
}

int ns_privileged(const process_t *p, const struct userns *ns)
{
    return ns_capable(p, ns, CAP_SYS_ADMIN);
}

int ns_owner_privileged(const process_t *p, const struct ns_common *c)
{
    return ns_privileged(p, c->owner ? c->owner : &init_userns);
}

/* ---- PIDs ---- */

/*
 * A reaped process's numbers that a group or a session still has as its ID:
 * its kernel ID, its PID namespace, held, and its numbers in that namespace
 * and those above it, as the process had them. Linux's struct pid lasts
 * while any task has it as its PID, group or session, so a group's number
 * names it in every namespace its leader was in after the leader is reaped
 * (`ns_release` keeps one, `pid_held_prune` lets it go). Its memory is taken
 * at the fork (`ns_fork`, `pid_keep`), so that the reap needs none.
 */
struct pid_held {
    struct pid_held *next;
    uint32_t kid;
    struct pidns *ns;
    uint32_t nr[NS_LEVELS];
};

static struct pid_held *pids_held;

/** The number in the PID namespace `v` of the ID `kid` whose namespace is
 * `ns` and whose numbers by level are `nr`, 0 when `ns` is not `v` or
 * below it. */
static uint32_t pid_number(const struct pidns *ns, const uint32_t *nr, uint32_t kid,
                           const struct pidns *v)
{
    if (v == &init_pidns) {
        return kid;
    }
    if (ns->level < v->level) {
        return 0;
    }
    while (ns->level > v->level) {
        ns = ns->parent;
    }
    return ns == v ? nr[v->level] : 0;
}

/** The number of `p` in the PID namespace `v`, 0 when `p` is not in `v`
 * or below it. */
static uint32_t pid_in(const process_t *p, const struct pidns *v)
{
    return pid_number(proc_pidns(p), p->nspid, p->pid, v);
}

/** Whether a live process other than `gone` has the kernel ID `kid` as its
 * group or its session. */
static int pid_held_by_group(uint32_t kid, const process_t *gone)
{
    return proc_group_held(kid, 1, 0, gone);
}

/** Release one allocated PID in `ns` and each ancestor below the first,
 * with the big lock held. A last release before init was published closes
 * allocation, as Linux 6.12.115's free_pid does (kernel/pid.c:140-155).
 * A kept group or session ID counts until pid_held_prune releases it. */
static void pidns_free(struct pidns *ns)
{
    for (; ns != &init_pidns; ns = ns->parent) {
        if (--ns->allocated == 0) {
            ns->adding = 0;
        }
    }
}

/** Let go of the kept numbers no group or session has now but through
 * `gone`, a record being freed. */
static void pid_held_prune(const process_t *gone)
{
    struct pid_held **link = &pids_held;

    while (*link) {
        struct pid_held *h = *link;

        if (pid_held_by_group(h->kid, gone)) {
            link = &h->next;
            continue;
        }
        *link = h->next;
        pidns_free(h->ns);
        ns_put(&h->ns->c);
        kfree(h);
    }
}

/** The kept numbers of the reaped process whose kernel ID was `kid`, or
 * NULL. */
static const struct pid_held *pid_held_find(uint32_t kid)
{
    const struct pid_held *h;

    for (h = pids_held; h && h->kid != kid; h = h->next) {
    }
    return h;
}

/** Keep the numbers of `p`, a record being freed in a PID namespace below
 * the first, while a group or a session has its ID, in the memory its fork
 * took for them, `p->pid_keep`, which is otherwise freed. Return nonzero
 * when the numbers are kept, so ns_release does not free their allocation. */
static int pid_held_keep(process_t *p)
{
    struct pid_held *h = p->pid_keep;

    p->pid_keep = NULL;
    if (!h) {
        return 0;
    }
    if (!p->pidns || p->pidns == &init_pidns || !p->nspid[p->pidns->level] ||
        !pid_held_by_group(p->pid, p)) {
        kfree(h);
        return 0;
    }
    h->kid = p->pid;
    h->ns = p->pidns;
    ns_get(&h->ns->c);
    k_memcpy(h->nr, p->nspid, sizeof(h->nr));
    h->next = pids_held;
    pids_held = h;
    return 1;
}

uint32_t pid_seen_by(const process_t *viewer, uint32_t kid)
{
    const struct pidns *v = proc_pidns(viewer);
    const struct pid_held *h;
    const process_t *p;

    if (v == &init_pidns || kid == 0) {
        return kid;
    }
    p = get_process(kid);
    if (p) {
        return pid_in(p, v);
    }
    h = pid_held_find(kid);
    return h ? pid_number(h->ns, h->nr, kid, v) : 0;
}

uint32_t pid_seen(uint32_t kid)
{
    return pid_seen_by(current_proc, kid);
}

void pid_nums_set(struct pid_nums *n, uint32_t kid)
{
    struct pidns *old = n->ns;
    const process_t *p = kid != 0 ? get_process(kid) : NULL;
    const struct pid_held *h = p || kid == 0 ? NULL : pid_held_find(kid);

    n->ns = NULL;
    if (p && proc_pidns(p) != &init_pidns) {
        n->ns = pidns_get(proc_pidns(p));
        k_memcpy(n->nr, p->nspid, sizeof(n->nr));
    } else if (h) {
        n->ns = pidns_get(h->ns);
        k_memcpy(n->nr, h->nr, sizeof(n->nr));
    }
    if (old) {
        ns_put(&old->c);
    }
}

uint32_t pid_nums_seen(const struct pid_nums *n, uint32_t kid)
{
    const struct pidns *v = proc_pidns(current_proc);

    if (v == &init_pidns || kid == 0) {
        return kid;
    }
    return n->ns ? pid_number(n->ns, n->nr, kid, v) : 0;
}

struct pidns *pidns_get(struct pidns *ns)
{
    ns_get(&ns->c);
    return ns;
}

struct userns *userns_get(struct userns *ns)
{
    ns_get(&ns->c);
    return ns;
}

uint32_t pidns_last_pid(void)
{
    const struct pidns *ns = proc_pidns(current_proc);

    return ns == &init_pidns ? proc_last_pid() : ns->last;
}

int32_t pidns_set_last_pid(int32_t pid)
{
    struct pidns *ns = proc_pidns(current_proc);
    const struct userns *owner = ns->c.owner ? ns->c.owner : &init_userns;

    if (!ns_capable(current_proc, owner, CAP_SYS_ADMIN) &&
        !ns_capable(current_proc, owner, CAP_CHECKPOINT_RESTORE)) {
        return -EPERM;
    }
    if (pid < 0 || pid > pid_max) {
        return -EINVAL;
    }
    if (ns == &init_pidns) {
        proc_set_last_pid((uint32_t)pid);
    } else {
        ns->last = (uint32_t)pid;
    }
    return 0;
}

int pidns_seen(const struct pidns *ns)
{
    const struct pidns *v = proc_pidns(current_proc);

    while (ns && ns->level > v->level) {
        ns = ns->parent;
    }
    return ns == v;
}

uint32_t pid_named(uint32_t pid)
{
    const struct pidns *v = proc_pidns(current_proc);
    const struct pid_held *h;
    const process_t *p;

    if (v == &init_pidns || pid == 0) {
        return pid;
    }
    for (p = proc_list; p; p = p->next) {
        if (p->state != PROC_UNUSED && pid_in(p, v) == pid) {
            return p->pid;
        }
    }
    for (h = pids_held; h; h = h->next) {
        if (pid_number(h->ns, h->nr, h->kid, v) == pid && pid_held_by_group(h->kid, NULL)) {
            return h->kid;
        }
    }
    return 0;
}

process_t *proc_named(uint32_t pid)
{
    uint32_t kid = pid_named(pid);

    return kid != 0 ? get_process(kid) : NULL;
}

/** Whether a record, forks still being built included, or a kept group's
 * numbers have the number `n` in the PID namespace `ns`. */
static int pidns_held(const struct pidns *ns, uint32_t n)
{
    const struct pid_held *h;
    const process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->pidns && pid_in(p, ns) == n) {
            return 1;
        }
    }
    for (h = pids_held; h; h = h->next) {
        if (pid_number(h->ns, h->nr, h->kid, ns) == n) {
            return 1;
        }
    }
    return 0;
}

/** The next free number of the PID namespace `ns`: the first above its
 * last that is not held, below `pid_max`, then from RESERVED_PIDS, or from
 * 1 before the namespace has given out RESERVED_PIDS, as Linux 6.12's
 * alloc_pid takes its number by idr_alloc_cyclic (kernel/pid.c), so its
 * first process takes 1 and a write of ns_last_pid above any earlier
 * number is followed; 0 when one lap of the range finds every number
 * held. */
static uint32_t pidns_alloc(struct pidns *ns)
{
    uint32_t max = (uint32_t)pid_max;
    uint32_t min = ns->last >= RESERVED_PIDS ? RESERVED_PIDS : 1u;
    uint32_t n = ns->last + 1u;
    uint32_t tries;

    for (tries = 0; tries <= max - min; tries++, n++) {
        if (n >= max) {
            n = min;
        }
        if (!pidns_held(ns, n)) {
            ns->last = n;
            return n;
        }
    }
    return 0;
}

uint32_t ns_reaper(const process_t *p)
{
    const struct pidns *ns = proc_pidns(p);

    return ns == &init_pidns ? 0 : ns->reaper;
}

void ns_reaper_gone(const process_t *p)
{
    struct pidns *ns = p->pidns;
    struct k_siginfo info;
    process_t *q;

    if (!ns || ns->reaper != p->pid) {
        return;
    }
    ns->reaper = 0;
    ns->adding = 0;
    k_memset(&info, 0, sizeof(info));
    info.code = SI_KERNEL;
    for (q = proc_list; q; q = q->next) {
        if (q != p && q->state != PROC_UNUSED && q->state != PROC_ZOMBIE && pid_in(q, ns) != 0) {
            signal_post_thread(q, SIGKILL, &info);
        }
    }
}

/* ---- making namespaces ---- */

/** A new user namespace below `parent`, owned by `p`'s effective ids, with
 * no map; NULL with `*err` -EUSERS past NS_LEVELS or -ENOMEM. */
static struct userns *userns_new(struct userns *parent, const process_t *p, int32_t *err)
{
    struct userns *ns;

    if (parent->level + 1u >= NS_LEVELS) {
        *err = -EUSERS;
        return NULL;
    }
    ns = ns_new(sizeof(*ns), NS_USER, parent);
    if (!ns) {
        *err = -ENOMEM;
        return NULL;
    }
    ns->parent = parent;
    ns_get(&parent->c);
    ns->level = parent->level + 1u;
    ns->owner_uid = p->euid;
    ns->owner_gid = p->egid;
    ns->max_mnt = NS_LIMIT_MAX;
    ns->msgqueue_max = p->rlim[RLIM_MSGQUEUE].cur;
    return ns;
}

/** A new PID namespace below `parent`, owned by `owner`, open for PID
 * allocation as Linux 6.12.115's create_pid_namespace leaves PIDNS_ADDING
 * set (kernel/pid_namespace.c:113); NULL with `*err` set as for userns_new. */
static struct pidns *pidns_new(struct pidns *parent, struct userns *owner, int32_t *err)
{
    struct pidns *ns;

    if (parent->level + 1u >= NS_LEVELS) {
        *err = -EUSERS;
        return NULL;
    }
    ns = ns_new(sizeof(*ns), NS_PID, owner);
    if (!ns) {
        *err = -ENOMEM;
        return NULL;
    }
    ns->parent = parent;
    ns_get(&parent->c);
    ns->level = parent->level + 1u;
    ns->adding = 1;
    return ns;
}

/** Drop a reference to the set `s`, and with the last its namespaces. */
static void nsset_put(struct nsset *s)
{
    if (!s || s == &init_nsset || --s->refs != 0) {
        return;
    }
    ns_put(&s->mnt->c);
    ns_put(&s->net->c);
    ns_put(&s->uts->c);
    ns_put(&s->ipc->c);
    ns_put(&s->pid_children->c);
    kfree(s);
}

int32_t ns_max_mnt(struct userns *ns)
{
    if (ns->max_mnt == NS_LIMIT_UNSET) {
        uint64_t threads = (uint64_t)frame_total_count() * 4096u / (8u * NS_THREAD_SIZE);

        threads = threads < NS_MIN_THREADS   ? NS_MIN_THREADS
                  : threads > NS_MAX_THREADS ? NS_MAX_THREADS
                                             : threads;
        ns->max_mnt = (int32_t)(threads / 2u);
    }
    return ns->max_mnt;
}

int ns_set_max_mnt(struct userns *ns, int32_t v)
{
    if (v < 0) {
        return -EINVAL;
    }
    ns->max_mnt = v;
    return 0;
}

/** The live mount namespaces charged to `uid` in the user namespace `ns`
 * (`mntns_charge_ok`). */
static uint32_t mntns_charged(const struct userns *ns, uint32_t uid)
{
    const struct mntns *m;
    uint32_t n = 0;

    for (m = mnt_list; m; m = m->next) {
        const struct userns *at = m->c.owner;
        uint32_t u = m->maker_uid;

        while (at != ns && at != &init_userns) {
            u = at->owner_uid;
            at = at->parent;
        }
        n += at == ns && u == uid;
    }
    return n;
}

/**
 * Whether one more mount namespace, owned by `owner` and made by the
 * kernel's uid `uid`, stays within each limit it counts against, as Linux's
 * ucounts count it: against max_mnt of `owner` among those `uid` made in
 * it, and then against max_mnt of each ancestor among those charged to the
 * owner of the user namespace below it.
 */
static int mntns_charge_ok(struct userns *owner, uint32_t uid)
{
    struct userns *ns = owner;

    for (;;) {
        if (mntns_charged(ns, uid) >= (uint32_t)ns_max_mnt(ns)) {
            return 0;
        }
        if (ns == &init_userns) {
            return 1;
        }
        uid = ns->owner_uid;
        ns = ns->parent;
    }
}

/** A mount namespace with the lowest free id, owned by `owner` and made by
 * the kernel's uid `uid`, whose mounts are those `from` has; NULL with
 * `*err` -ENOSPC past a limit of `mntns_charge_ok`, or -ENOMEM. */
static struct mntns *mntns_new(const struct mntns *from, struct userns *owner, uint32_t uid,
                               int32_t *err)
{
    struct mntns *ns;
    uint32_t id = idset_first_free(&mnt_ids);

    if (!mntns_charge_ok(owner, uid)) {
        *err = -ENOSPC;
        return NULL;
    }
    ns = ns_new(sizeof(*ns), NS_MNT, owner);
    if (!ns) {
        *err = -ENOMEM;
        return NULL;
    }
    if (idset_add(&mnt_ids, id) != 0 || vfs_mntns_copy(from->id, id) != 0) {
        idset_del(&mnt_ids, id);
        ns_put(&owner->c);
        kfree(ns);
        *err = -ENOMEM;
        return NULL;
    }
    ns->id = id;
    ns->maker_uid = uid;
    ns->next = mnt_list;
    if (mnt_list) {
        mnt_list->prev = ns;
    }
    mnt_list = ns;
    return ns;
}

/**
 * A copy of the set `from` with new namespaces for the CLONE_NEW flags of
 * `flags` other than the user's, owned by `owner`; the new PID namespace is
 * made below `active`, the one `from` makes children in, and a new mount
 * namespace is charged to the kernel's uid `uid`. NULL with `*err` set on
 * failure.
 */
static struct nsset *nsset_copy(const struct nsset *from, uint32_t flags, struct userns *owner,
                                struct pidns *active, uint32_t uid, int32_t *err)
{
    struct nsset *s = kzalloc(sizeof(*s));

    if (!s) {
        *err = -ENOMEM;
        return NULL;
    }
    s->refs = 1;
    s->mnt = from->mnt;
    s->net = from->net;
    s->uts = from->uts;
    s->ipc = from->ipc;
    s->pid_children = from->pid_children;
    ns_get(&s->mnt->c);
    ns_get(&s->net->c);
    ns_get(&s->uts->c);
    ns_get(&s->ipc->c);
    ns_get(&s->pid_children->c);
    if (flags & CLONE_NEWNS) {
        struct mntns *m = mntns_new(from->mnt, owner, uid, err);

        if (!m) {
            goto fail;
        }
        ns_put(&s->mnt->c);
        s->mnt = m;
    }
    if (flags & CLONE_NEWNET) {
        struct netns *n = ns_new(sizeof(*n), NS_NET, owner);

        if (n) {
            n->lo = netif_ns_lo_new();
            if (!n->lo) {
                ns_put(&n->c);
                n = NULL;
            }
        }
        if (!n) {
            *err = -ENOMEM;
            goto fail;
        }
        ns_put(&s->net->c);
        s->net = n;
    }
    if (flags & CLONE_NEWUTS) {
        struct utsns *u = ns_new(sizeof(*u), NS_UTS, owner);

        if (!u) {
            *err = -ENOMEM;
            goto fail;
        }
        k_memcpy(u->hostname, from->uts->hostname, sizeof(u->hostname));
        k_memcpy(u->domainname, from->uts->domainname, sizeof(u->domainname));
        ns_put(&s->uts->c);
        s->uts = u;
    }
    if (flags & CLONE_NEWIPC) {
        struct ipcns *i = ns_new(sizeof(*i), NS_IPC, owner);

        if (!i) {
            *err = -ENOMEM;
            goto fail;
        }
        if (mq_root_made(i) != 0) {
            ns_put(&i->c);
            *err = -ENOMEM;
            goto fail;
        }
        ns_put(&s->ipc->c);
        s->ipc = i;
    }
    if (flags & CLONE_NEWPID) {
        struct pidns *pn = pidns_new(active, owner, err);

        if (!pn) {
            goto fail;
        }
        ns_put(&s->pid_children->c);
        s->pid_children = pn;
    }
    return s;
fail:
    nsset_put(s);
    return NULL;
}

/** Give `child` its numbers in each PID namespace from `ns` up to the
 * first's child, reserving number 1 for the first process without making
 * it the reaper yet (`ns_fork_commit`). Allocation stays open while that
 * first fork is built, as Linux 6.12.115's alloc_pid tests PIDNS_ADDING
 * (kernel/pid.c:277-285). 0, -EAGAIN when a namespace is full, as
 * alloc_pid turns the IDR's -ENOSPC, or -ENOMEM when one is closed,
 * with partial numbers cleared and an empty new namespace's cursor reset
 * as alloc_pid does (kernel/pid.c:294-303). The big lock is held; no sleep. */
static int32_t ns_number(process_t *child, struct pidns *ns)
{
    struct pidns *at;
    int32_t rc;

    pid_held_prune(NULL);
    for (at = ns; at != &init_pidns; at = at->parent) {
        if (!at->adding) {
            rc = -ENOMEM;
            goto fail;
        }
        child->nspid[at->level] = pidns_alloc(at);
        if (child->nspid[at->level] == 0) {
            rc = -EAGAIN;
            goto fail;
        }
    }
    for (at = ns; at != &init_pidns; at = at->parent) {
        at->allocated++;
    }
    return 0;

fail:
    if (ns != &init_pidns && ns->adding && ns->allocated == 0) {
        ns->last = 0;
    }
    k_memset(child->nspid, 0, sizeof(child->nspid));
    return rc;
}

int32_t ns_fork_commit(process_t *child)
{
    struct pidns *ns = proc_pidns(child);
    struct pidns *at;
    int first = ns != &init_pidns && child->nspid[ns->level] == 1;

    for (at = ns; at != &init_pidns; at = at->parent) {
        if (!at->adding) {
            return -ENOMEM;
        }
    }
    if (first) {
        ns->reaper = child->pid;
        child->tg->unkillable = 1;
    }
    return 0;
}

void ns_fork_abort(process_t *child)
{
    struct pidns *ns = proc_pidns(child);

    if (ns != &init_pidns && child->nspid[ns->level] != 0) {
        pidns_free(ns);
        k_memset(child->nspid, 0, sizeof(child->nspid));
    }
}

int32_t ns_fork(process_t *child, const process_t *parent, uint32_t flags)
{
    struct userns *uns = proc_userns(parent);
    struct nsset *set = proc_nsset(parent);
    struct pidns *pns;
    int32_t err = 0;
    int priv;

    if ((flags & CLONE_THREAD) && (flags & (CLONE_NEWUSER | CLONE_NEWPID))) {
        return -EINVAL;
    }
    if ((flags & CLONE_NEWUSER) && (flags & CLONE_FS)) {
        return -EINVAL;
    }
    if ((flags & CLONE_THREAD) && set->pid_children != proc_pidns(parent)) {
        return -EINVAL;
    }
    child->ns_granted = parent->ns_granted;
    if (flags & CLONE_NEWUSER) {
        uns = userns_new(uns, parent, &err);
        if (!uns) {
            return err;
        }
        ns_caps_fill(child);
    } else {
        ns_get(&uns->c);
    }
    child->userns = uns == &init_userns ? NULL : uns;
    priv = (flags & CLONE_NEWUSER) || ns_privileged(parent, uns);
    if ((flags & (CLONE_NEW_ANY & ~CLONE_NEWUSER)) && !priv) {
        return -EPERM;
    }
    if (flags & (CLONE_NEW_ANY & ~CLONE_NEWUSER)) {
        set = nsset_copy(set, flags, uns, set->pid_children, parent->euid, &err);
        if (!set) {
            return err;
        }
    } else if (set != &init_nsset) {
        set->refs++;
    }
    child->nsset = set == &init_nsset ? NULL : set;
    pns = set->pid_children;
    ns_get(&pns->c);
    child->pidns = pns == &init_pidns ? NULL : pns;
    if (child->pidns) {
        child->pid_keep = kzalloc(sizeof(*child->pid_keep));
        if (!child->pid_keep) {
            return -ENOMEM;
        }
    }
    return ns_number(child, pns);
}

void ns_release(process_t *p)
{
    pid_held_prune(p);
    if (!pid_held_keep(p) && p->pidns && p->nspid[p->pidns->level] != 0) {
        pidns_free(p->pidns);
    }
    if (p->userns) {
        ns_put(&p->userns->c);
        p->userns = NULL;
    }
    if (p->pidns) {
        ns_put(&p->pidns->c);
        p->pidns = NULL;
    }
    nsset_put(p->nsset);
    p->nsset = NULL;
}

void ns_exec(process_t *p)
{
    p->ns_granted = 0;
}

int cap_ids(const process_t *p)
{
    if (!p->userns) {
        return p->euid == 0;
    }
    return p->ns_granted || ns_uid_from_kernel(p->userns, p->euid) == 0;
}

/** Whether `uid` is root in `p`'s user namespace. */
static int uid_root_in(const process_t *p, uint32_t uid)
{
    return p->userns ? ns_uid_from_kernel(p->userns, uid) == 0 : uid == 0;
}

uint32_t cap_word(const process_t *p, uint32_t i)
{
    uint32_t full = i == 0 ? 0xFFFFFFFFu : CAP_HIGH_WORD;
    int ids = cap_ids(p) || uid_root_in(p, p->ruid) || uid_root_in(p, p->suid);

    if (i > 1) {
        return 0;
    }
    return p->amb_granted || ids ? full & ~p->cap_lost[i] : 0u;
}

uint32_t cap_eff_word(const process_t *p, uint32_t i)
{
    uint32_t fs = i == 0 ? CAP_FS_WORD0 : CAP_FS_WORD1;
    uint32_t held = cap_word(p, i);

    if (i > 1) {
        return 0;
    }
    if (p->eff_own) {
        held &= p->cap_eff[i];
        if ((p->fs_own & PROC_FSCAPS_OFF) ||
            (!uid_root_in(p, proc_fsuid(p)) && uid_root_in(p, p->euid))) {
            held &= ~fs;
        }
        return held;
    }
    if (!p->amb_granted && !cap_ids(p)) {
        held = 0;
    }
    if (p->fs_own & PROC_FSCAPS_OFF) {
        return held & ~fs;
    }
    if (uid_root_in(p, proc_fsuid(p))) {
        return held | (fs & ~p->cap_lost[i]);
    }
    return uid_root_in(p, p->euid) ? held & ~fs : held;
}

int cap_holds(const process_t *p, uint32_t cap)
{
    return cap <= CAP_LAST_CAP && (cap_eff_word(p, cap >> 5) & (1u << (cap & 31u))) != 0;
}

void cap_store(process_t *p, uint32_t lost0, uint32_t lost1)
{
    p->cap_lost[0] = lost0;
    p->cap_lost[1] = lost1 & CAP_HIGH_WORD;
    p->eff_own = 0;
    p->nocaps = (lost0 & (1u << CAP_SYS_ADMIN)) != 0;
}

void cap_store_eff(process_t *p, uint32_t eff0, uint32_t eff1)
{
    p->eff_own = 1;
    p->cap_eff[0] = eff0;
    p->cap_eff[1] = eff1 & CAP_HIGH_WORD;
    p->nocaps = ((p->cap_lost[0] | ~eff0) & (1u << CAP_SYS_ADMIN)) != 0;
}

void ns_caps_fill(process_t *p)
{
    p->ns_granted = 1;
    p->amb_granted = 0;
    p->securebits = 0;
    p->bset_lost[0] = p->bset_lost[1] = 0;
    p->cap_inh[0] = p->cap_inh[1] = 0;
    p->cap_amb[0] = p->cap_amb[1] = 0;
    cap_store(p, 0, 0);
    proc_cred_commit(p);
}

/* ---- unshare and setns ---- */

/**
 * The new namespaces are made before any of the caller's is replaced, so
 * that a failure to make one leaves the caller's as they were. The new set
 * is a copy, and the processes that shared the caller's set keep it.
 */
long sys_unshare(isr_params_t *params)
{
    uint32_t flags = (uint32_t)SC_ARG(params, 0);
    process_t *self = current_proc;
    struct userns *uns = NULL;
    struct nsset *set = NULL;
    int32_t err = 0;

    if (SC_ARG(params, 0) != flags ||
        (flags & ~(CLONE_NEW_ANY | CLONE_SYSVSEM | CLONE_FILES | CLONE_FS | CLONE_THREAD |
                   CLONE_SIGHAND | CLONE_VM))) {
        return -EINVAL;
    }
    if ((flags & (CLONE_NEWUSER | CLONE_THREAD | CLONE_SIGHAND | CLONE_VM)) && self->tg->live > 1) {
        return -EINVAL;
    }
    if ((flags & CLONE_NEWPID) && proc_nsset(self)->pid_children != proc_pidns(self)) {
        return -EINVAL;
    }
    if (flags & (CLONE_NEWNS | CLONE_NEWUSER)) {
        flags |= CLONE_FS;
    }
    if (flags & CLONE_NEWUSER) {
        uns = userns_new(proc_userns(self), self, &err);
        if (!uns) {
            return err;
        }
    }
    if (flags & (CLONE_NEW_ANY & ~CLONE_NEWUSER)) {
        if (!uns && !ns_privileged(self, proc_userns(self))) {
            err = -EPERM;
        } else {
            set = nsset_copy(proc_nsset(self), flags, uns ? uns : proc_userns(self),
                             proc_pidns(self), self->euid, &err);
        }
        if (!set) {
            if (uns) {
                ns_put(&uns->c);
            }
            return err;
        }
    }
    if (((flags & CLONE_FS) && proc_fs_unshare() != 0) ||
        ((flags & CLONE_FILES) && proc_files_unshare() != 0)) {
        nsset_put(set);
        if (uns) {
            ns_put(&uns->c);
        }
        return -ENOMEM;
    }
    if (uns) {
        if (self->userns) {
            ns_put(&self->userns->c);
        }
        self->userns = uns;
        ns_caps_fill(self);
    }
    if (set) {
        nsset_put(self->nsset);
        self->nsset = set;
    }
    return 0;
}

/* ---- nsfs ---- */

/* A descriptor open on a namespace, which /proc/PID/fd names as Linux's
 * nsfs does: the kind and the inode number. */
struct nsfs_file {
    struct anon_obj anon;
    char name[24];
    struct ns_common *ns;
};

/** Close a namespace's descriptor: drop its reference and free its record. */
static void nsfs_close(fd_entry_t *e)
{
    struct nsfs_file *f = e->priv;

    if (f) {
        ns_put(f->ns);
        kfree(f);
        e->priv = NULL;
    }
}

/* The requests of ioctl(2) on a namespace's descriptor, Linux's numbers. */
#define NS_GET_USERNS    0xB701u
#define NS_GET_PARENT    0xB702u
#define NS_GET_NSTYPE    0xB703u
#define NS_GET_OWNER_UID 0xB704u

/** Whether `c`, a user or PID namespace, is the current process's of its
 * kind or below it, as a namespace an ioctl hands out must be. */
static int ns_below_own(const struct ns_common *c)
{
    if (c->kind == NS_USER) {
        const struct userns *u;

        for (u = (const struct userns *)c; u; u = u->parent) {
            if (u == proc_userns(current_proc)) {
                return 1;
            }
        }
    } else {
        const struct pidns *p;

        for (p = (const struct pidns *)c; p; p = p->parent) {
            if (p == proc_pidns(current_proc)) {
                return 1;
            }
        }
    }
    return 0;
}

/**
 * ioctl(2) on a namespace's descriptor, the requests lsns(8) makes:
 * NS_GET_USERNS gives a descriptor of the user namespace that owns it,
 * NS_GET_PARENT one of the parent of a user or PID namespace, NS_GET_NSTYPE
 * its CLONE_NEW flag and NS_GET_OWNER_UID the uid of a user namespace's
 * maker, as the caller sees it, at `arg`. A descriptor handed out is
 * close-on-exec. The namespace handed out must be the caller's or below it,
 * and the first has no parent: -EPERM; -EINVAL for a request of the wrong
 * kind, -ENOTTY for another.
 */
static int32_t nsfs_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct ns_common *c = nsfs_ns(e);
    struct ns_common *to;
    uint32_t uid;

    switch (request) {
    case NS_GET_NSTYPE:
        return (int32_t)kind_flags[c->kind];
    case NS_GET_OWNER_UID:
        if (c->kind != NS_USER) {
            return -EINVAL;
        }
        uid = uid_seen(((struct userns *)c)->owner_uid);
        return copy_to_user((void __user *)arg, &uid, sizeof(uid)) == 0 ? 0 : -EFAULT;
    case NS_GET_USERNS:
        to = c->kind == NS_USER ? (struct ns_common *)((struct userns *)c)->parent
                                : (c->owner ? &c->owner->c : &init_userns.c);
        break;
    case NS_GET_PARENT:
        if (c->kind != NS_USER && c->kind != NS_PID) {
            return -EINVAL;
        }
        to = c->kind == NS_USER ? (struct ns_common *)((struct userns *)c)->parent
                                : (struct ns_common *)((struct pidns *)c)->parent;
        break;
    default:
        return -ENOTTY;
    }
    if (!to || !ns_below_own(to)) {
        return -EPERM;
    }
    return nsfs_open(to, NS_O_CLOEXEC);
}

/* A namespace's descriptor has only ioctl and close. */
const struct file_ops nsfs_ops = {NULL, NULL, NULL, nsfs_ioctl, nsfs_close, NULL, NULL, 0};

struct ns_common *nsfs_ns(const fd_entry_t *e)
{
    return e && e->type == FD_TYPE_ANON && e->ops == &nsfs_ops && e->priv
               ? ((const struct nsfs_file *)e->priv)->ns
               : NULL;
}

void ns_link_name(const struct ns_common *c, char *out)
{
    char digits[12];
    uint32_t v = c->inum;
    int n = 0, at;

    for (at = 0; kind_names[c->kind][at]; at++) {
        out[at] = kind_names[c->kind][at];
    }
    out[at++] = ':';
    out[at++] = '[';
    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    while (n > 0) {
        out[at++] = digits[--n];
    }
    out[at++] = ']';
    out[at] = '\0';
}

int32_t nsfs_open(struct ns_common *c, uint32_t oflag)
{
    struct nsfs_file *f = kzalloc(sizeof(*f));
    fd_entry_t *e;
    int fd;

    if (!f) {
        return -ENOMEM;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    ns_link_name(c, f->name);
    f->anon.name = f->name;
    f->ns = c;
    ns_get(c);
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &nsfs_ops;
    e->priv = f;
    e->flags = FD_FLAG_READ | ((oflag & NS_O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    e->ofd->status_flags |= OFD_OPEN_STATUS(oflag);
    fd_install(current_proc->fds, fd);
    return fd;
}

struct ns_common *ns_of(const process_t *p, int kind)
{
    struct ns_common *c;

    switch (kind) {
    case NS_USER:
        c = &proc_userns(p)->c;
        break;
    case NS_PID:
        c = &proc_pidns(p)->c;
        break;
    case NS_MNT:
        c = &proc_mntns(p)->c;
        break;
    case NS_NET:
        c = &proc_netns(p)->c;
        break;
    case NS_UTS:
        c = &proc_utsns(p)->c;
        break;
    case NS_IPC:
        c = &proc_ipcns(p)->c;
        break;
    default:
        return NULL;
    }
    ns_get(c);
    return c;
}

const char *ns_kind_name(int kind)
{
    return kind >= 0 && kind < NS_KINDS ? kind_names[kind] : NULL;
}

/** setns of a user namespace `ns`: a process of one thread, whose
 * directories are its own, privileged in `ns`, takes it with every
 * capability. 0, -EINVAL for the caller's own namespace or a caller of
 * more threads or shared directories, or -EPERM without the privilege. */
static int32_t setns_user(struct userns *ns)
{
    process_t *self = current_proc;

    if (ns == proc_userns(self) || self->tg->live > 1 || self->fs->refs > 1) {
        return -EINVAL;
    }
    if (!ns_privileged(self, ns)) {
        return -EPERM;
    }
    ns_get(&ns->c);
    if (self->userns) {
        ns_put(&self->userns->c);
    }
    self->userns = ns;
    ns_caps_fill(self);
    return 0;
}

/** Whether the caller may join `c`, of a kind other than user: 0, -EPERM
 * without privilege in its own user namespace and in the one that owns
 * `c`, or -EINVAL for a PID namespace neither its own nor below it or a
 * mount namespace while it shares its directories. */
static int32_t setns_check(const process_t *self, const struct ns_common *c)
{
    if (!ns_owner_privileged(self, c) || !ns_privileged(self, proc_userns(self))) {
        return -EPERM;
    }
    if (c->kind == NS_PID) {
        const struct pidns *p = (const struct pidns *)c;

        while (p && p->level > proc_pidns(self)->level) {
            p = p->parent;
        }
        if (p != proc_pidns(self)) {
            return -EINVAL;
        }
    }
    if (c->kind == NS_MNT && self->fs->refs > 1) {
        return -EINVAL;
    }
    return 0;
}

/**
 * Give the caller a copy of its set in which each namespace of `ns` not
 * NULL, of a kind other than user, takes the place of its kind, so that the
 * processes that shared the caller's set keep it. The copy takes a
 * reference to each. 0, or -ENOMEM with the caller's set as it was.
 */
static int32_t setns_commit(process_t *self, struct ns_common *const ns[NS_KINDS])
{
    struct nsset *set;
    int32_t err = 0;
    int k;

    set = nsset_copy(proc_nsset(self), 0, proc_userns(self), proc_pidns(self), self->euid, &err);
    if (!set) {
        return err;
    }
    for (k = 0; k < NS_KINDS; k++) {
        struct ns_common *c = ns[k];

        if (!c || k == NS_USER) {
            continue;
        }
        switch (k) {
        case NS_PID:
            ns_put(&set->pid_children->c);
            set->pid_children = (struct pidns *)c;
            break;
        case NS_MNT:
            ns_put(&set->mnt->c);
            set->mnt = (struct mntns *)c;
            break;
        case NS_NET:
            ns_put(&set->net->c);
            set->net = (struct netns *)c;
            break;
        case NS_UTS:
            ns_put(&set->uts->c);
            set->uts = (struct utsns *)c;
            break;
        default:
            ns_put(&set->ipc->c);
            set->ipc = (struct ipcns *)c;
            break;
        }
        ns_get(c);
    }
    nsset_put(self->nsset);
    self->nsset = set;
    return 0;
}

/**
 * setns of a pidfd naming `target`, the thread whose namespaces are joined,
 * NULL once it has let its id go and -ESRCH, as is a thread that has ended,
 * whose namespaces Linux's exit_task_namespaces has let go, with `flags` the
 * CLONE_NEW flags of the kinds to join: the caller joins each of those
 * namespaces of the target, or none of them, as on Linux. A user namespace
 * is joined first, under setns_user's rules, and the other kinds are then
 * judged with the capabilities it gives; when one is refused, the caller's
 * user namespace and capabilities, with their identity (`cred_id`), are
 * put back.
 */
static long setns_pidfd(process_t *self, const process_t *target, unsigned long flags)
{
    struct ns_common *ns[NS_KINDS];
    struct userns *old = self->userns;
    uint32_t cap_lost[2], bset_lost[2], inh[2], amb[2], eff[2];
    uint32_t nocaps = self->nocaps, granted = self->ns_granted, amb_granted = self->amb_granted;
    uint32_t eff_own = self->eff_own, securebits = self->securebits;
    uint64_t cred_id = self->cred_id;
    int32_t err = 0;
    int k, joined = 0;

    if (flags == 0 || (flags & ~(unsigned long)CLONE_NEW_ANY)) {
        return -EINVAL;
    }
    if (!target || target->state == PROC_ZOMBIE || target->tg->live == 0) {
        return -ESRCH;
    }
    for (k = 0; k < NS_KINDS; k++) {
        ns[k] = (flags & kind_flags[k]) ? ns_of(target, k) : NULL;
    }
    cap_lost[0] = self->cap_lost[0];
    cap_lost[1] = self->cap_lost[1];
    bset_lost[0] = self->bset_lost[0];
    bset_lost[1] = self->bset_lost[1];
    k_memcpy(inh, self->cap_inh, sizeof(inh));
    k_memcpy(amb, self->cap_amb, sizeof(amb));
    k_memcpy(eff, self->cap_eff, sizeof(eff));
    if (old) {
        ns_get(&old->c);
    }
    if (ns[NS_USER]) {
        err = setns_user((struct userns *)ns[NS_USER]);
        joined = err == 0;
    }
    for (k = 0; !err && k < NS_KINDS; k++) {
        if (ns[k] && k != NS_USER) {
            err = setns_check(self, ns[k]);
        }
    }
    if (!err) {
        err = setns_commit(self, ns);
    }
    if (err && joined) {
        ns_put(&self->userns->c);
        self->userns = old;
        old = NULL;
        self->ns_granted = granted;
        self->amb_granted = amb_granted;
        self->bset_lost[0] = bset_lost[0];
        self->bset_lost[1] = bset_lost[1];
        self->cap_lost[0] = cap_lost[0];
        self->cap_lost[1] = cap_lost[1];
        self->nocaps = nocaps;
        self->eff_own = eff_own;
        self->securebits = securebits;
        k_memcpy(self->cap_eff, eff, sizeof(eff));
        self->cred_id = cred_id;
        k_memcpy(self->cap_inh, inh, sizeof(inh));
        k_memcpy(self->cap_amb, amb, sizeof(amb));
    }
    if (old) {
        ns_put(&old->c);
    }
    for (k = 0; k < NS_KINDS; k++) {
        ns_put(ns[k]);
    }
    return err;
}

/**
 * The caller is given a copy of its set with the one kind changed, so the
 * processes that shared the caller's set keep it.
 */
long sys_setns(isr_params_t *params)
{
    process_t *self = current_proc;
    int fd = (int)SC_ARG(params, 0);
    struct ns_common *c = nsfs_ns(fd_get(self->fds, fd));
    struct ns_common *ns[NS_KINDS] = {NULL};
    process_t *target;
    int32_t err;

    if (!c) {
        if (pidfd_lookup(fd, &target, NULL) == 0) {
            return setns_pidfd(self, target, SC_ARG(params, 1));
        }
        return fd_get(self->fds, fd) ? -EINVAL : -EBADF;
    }
    if (SC_ARG(params, 1) != 0 && SC_ARG(params, 1) != kind_flags[c->kind]) {
        return -EINVAL;
    }
    if (c->kind == NS_USER) {
        return setns_user((struct userns *)c);
    }
    err = setns_check(self, c);
    if (err) {
        return err;
    }
    ns[c->kind] = c;
    return setns_commit(self, ns);
}
