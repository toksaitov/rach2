/*
 * ns.c: namespaces, and the calls that make and join them.
 *
 * A process keeps its user namespace, its PID namespace and its numbers in
 * the PID namespaces it is in on its own record, and its other namespaces in
 * a `struct nsset` that the processes it makes share until one of them
 * changes it. A namespace counts the records and the sets that hold it, and
 * the namespaces it owns hold their user namespace. The first namespaces are
 * static, and a record or a set that holds none of a kind holds the first.
 */

#include "ns.h"
#include "tgroup.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../io/anon.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

/* The first namespaces have the inode numbers Linux gives its first ones,
 * and the others are numbered upward from NS_FIRST_DYNAMIC. */
#define NS_FIRST_DYNAMIC 0xF0000100u

/* The highest number a PID namespace other than the first gives out. */
#define NS_PID_MAX 32768u

/* The mount namespaces a mount's bit set has room for. */
#define NS_MNT_MAX 32u

/* The open flag that marks a descriptor close-on-exec. */
#define NS_O_CLOEXEC 02000000u

/* The clone flags beside the namespaces' that clone and unshare check. */
#define CLONE_VM      0x00000100u
#define CLONE_FS      0x00000200u
#define CLONE_FILES   0x00000400u
#define CLONE_SIGHAND 0x00000800u
#define CLONE_THREAD  0x00010000u
#define CLONE_SYSVSEM 0x00040000u

struct userns init_userns = {
    {1, 0xEFFFFFFDu, NS_USER, NULL}, NULL, 0, 0, 0, {{0, 0, 0xFFFFFFFFu}}, 1,
    {{0, 0, 0xFFFFFFFFu}},           1,    0};
struct pidns init_pidns = {{1, 0xEFFFFFFCu, NS_PID, NULL}, NULL, 0, 0, 1};
struct mntns init_mntns = {{1, 0xF0000000u, NS_MNT, NULL}, 0};
struct netns init_netns = {{1, 0xF0000098u, NS_NET, NULL}};
struct utsns init_utsns = {{1, 0xEFFFFFFEu, NS_UTS, NULL}, "rach2", "(none)"};
struct ipcns init_ipcns = {{1, 0xEFFFFFFFu, NS_IPC, NULL}};

static struct nsset init_nsset = {1,           &init_mntns, &init_netns,
                                  &init_utsns, &init_ipcns, &init_pidns};

static uint32_t next_inum = NS_FIRST_DYNAMIC;

/* The bits of the mount namespaces that exist, the first's bit 0. */
static uint32_t mnt_bits = 1u;

static const char *const kind_names[NS_KINDS] = {"user", "pid", "mnt", "net", "uts", "ipc"};

static const uint32_t kind_flags[NS_KINDS] = {CLONE_NEWUSER, CLONE_NEWPID, CLONE_NEWNS,
                                              CLONE_NEWNET,  CLONE_NEWUTS, CLONE_NEWIPC};

/** Whether `c` is one of the first namespaces, which are never freed. */
static int ns_first(const struct ns_common *c)
{
    return c == &init_userns.c || c == &init_pidns.c || c == &init_mntns.c || c == &init_netns.c ||
           c == &init_utsns.c || c == &init_ipcns.c;
}

static void ns_get(struct ns_common *c)
{
    c->refs++;
}

void ns_put(struct ns_common *c)
{
    if (!c || ns_first(c) || --c->refs != 0) {
        return;
    }
    switch (c->kind) {
    case NS_USER:
        ns_put(&((struct userns *)c)->parent->c);
        break;
    case NS_PID:
        ns_put(&((struct pidns *)c)->parent->c);
        break;
    case NS_MNT:
        vfs_mntns_gone(((struct mntns *)c)->bit);
        mnt_bits &= ~(1u << ((struct mntns *)c)->bit);
        break;
    case NS_NET:
        net_ns_gone((struct netns *)c);
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

uint32_t mntns_live_bits(void)
{
    return mnt_bits;
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

int64_t ns_uid_to_kernel(const struct userns *ns, uint32_t id)
{
    uint32_t i;

    if (ns == &init_userns) {
        return id;
    }
    for (i = 0; i < ns->uid_extents; i++) {
        if (id - ns->uid_map[i].first < ns->uid_map[i].count) {
            return ns->uid_map[i].lower + (id - ns->uid_map[i].first);
        }
    }
    return -1;
}

int64_t ns_gid_to_kernel(const struct userns *ns, uint32_t id)
{
    uint32_t i;

    if (ns == &init_userns) {
        return id;
    }
    for (i = 0; i < ns->gid_extents; i++) {
        if (id - ns->gid_map[i].first < ns->gid_map[i].count) {
            return ns->gid_map[i].lower + (id - ns->gid_map[i].first);
        }
    }
    return -1;
}

uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t kid)
{
    uint32_t i;

    if (ns == &init_userns) {
        return kid;
    }
    for (i = 0; i < ns->uid_extents; i++) {
        if (kid - ns->uid_map[i].lower < ns->uid_map[i].count) {
            return ns->uid_map[i].first + (kid - ns->uid_map[i].lower);
        }
    }
    return NS_OVERFLOW_ID;
}

uint32_t ns_gid_from_kernel(const struct userns *ns, uint32_t kid)
{
    uint32_t i;

    if (ns == &init_userns) {
        return kid;
    }
    for (i = 0; i < ns->gid_extents; i++) {
        if (kid - ns->gid_map[i].lower < ns->gid_map[i].count) {
            return ns->gid_map[i].first + (kid - ns->gid_map[i].lower);
        }
    }
    return NS_OVERFLOW_ID;
}

/** Whether one of the `n` extents of `map` covers the kernel's id `kid`. */
static int extent_has(const struct ns_extent *map, uint32_t n, uint32_t kid)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (kid - map[i].lower < map[i].count) {
            return 1;
        }
    }
    return 0;
}

int ns_ids_mapped(const struct userns *ns, uint32_t uid, uint32_t gid)
{
    return ns == &init_userns || (extent_has(ns->uid_map, ns->uid_extents, uid) &&
                                  extent_has(ns->gid_map, ns->gid_extents, gid));
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

/** Append `v` right-aligned in ten columns to `out` at `*at`. */
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
    const struct ns_extent *map = which == NS_MAP_GID ? ns->gid_map : ns->uid_map;
    uint32_t extents = which == NS_MAP_GID ? ns->gid_extents : ns->uid_extents;
    uint32_t i;
    int at = 0;

    if (own == ns && ns->parent) {
        own = ns->parent;
    }
    if (which == NS_MAP_SETGROUPS) {
        const char *word = ns->setgroups_denied ? "deny\n" : "allow\n";

        while (*word && at < cap - 1) {
            out[at++] = *word++;
        }
        out[at] = '\0';
        return at;
    }
    for (i = 0; i < extents; i++) {
        uint32_t lower = which == NS_MAP_GID ? ns_gid_from_kernel(own, map[i].lower)
                                             : ns_uid_from_kernel(own, map[i].lower);

        put_col(out, cap, &at, map[i].first);
        if (at < cap - 1) {
            out[at++] = ' ';
        }
        put_col(out, cap, &at, lower);
        if (at < cap - 1) {
            out[at++] = ' ';
        }
        put_col(out, cap, &at, map[i].count);
        if (at < cap - 1) {
            out[at++] = '\n';
        }
    }
    out[at] = '\0';
    return at;
}

/** Read an unsigned number at `*p`, past spaces before it; 0 with none. */
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

/** Whether the extent of `count` ids from `first` in the namespace and
 * from the kernel's `klo` overlaps one of the `n` extents of `map`. */
static int extent_overlaps(const struct ns_extent *map, int n, uint32_t first, uint32_t klo,
                           uint32_t count)
{
    int i;

    for (i = 0; i < n; i++) {
        if ((first < map[i].first + map[i].count && map[i].first < first + count) ||
            (klo < map[i].lower + map[i].count && map[i].lower < klo + count)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Parse the lines of a map written to `text`, at most NS_EXTENTS of three
 * numbers each, the ids in the namespace, the ids in its parent and the
 * count, into `map`, with the parent's ids made the kernel's through
 * `parent`'s map. The number of extents, or -EINVAL for text of another
 * form, an empty or wrapping range or two that overlap, or -EPERM for
 * parent ids the parent does not map.
 */
static int parse_map(const char *text, uint32_t len, const struct userns *parent, int gid,
                     struct ns_extent *map)
{
    const char *p = text, *end = text + len;
    int n = 0;

    while (p < end) {
        uint32_t first, lower, count;
        int64_t klo, khi;

        if (*p == '\n') {
            p++;
            continue;
        }
        if (n == NS_EXTENTS || !read_number(&p, end, &first) || !read_number(&p, end, &lower) ||
            !read_number(&p, end, &count) || count == 0 || first + count - 1u < first ||
            lower + count - 1u < lower) {
            return -EINVAL;
        }
        klo = gid ? ns_gid_to_kernel(parent, lower) : ns_uid_to_kernel(parent, lower);
        khi = gid ? ns_gid_to_kernel(parent, lower + count - 1u)
                  : ns_uid_to_kernel(parent, lower + count - 1u);
        if (klo < 0 || khi < 0 || (uint32_t)khi - (uint32_t)klo != count - 1u) {
            return -EPERM;
        }
        if (extent_overlaps(map, n, first, (uint32_t)klo, count)) {
            return -EINVAL;
        }
        map[n].first = first;
        map[n].lower = (uint32_t)klo;
        map[n].count = count;
        n++;
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p < end && *p != '\n') {
            return -EINVAL;
        }
    }
    return n == 0 ? -EINVAL : n;
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
 * not denied, before the group map is written. */
static int32_t setgroups_write(struct userns *ns, const char *text, uint32_t len)
{
    if (ns->gid_extents != 0) {
        return -EPERM;
    }
    if (text_begins(text, len, "deny")) {
        ns->setgroups_denied = 1;
        return (int32_t)len;
    }
    if (text_begins(text, len, "allow")) {
        return ns->setgroups_denied ? -EPERM : (int32_t)len;
    }
    return -EINVAL;
}

int32_t ns_map_write(struct userns *ns, int which, const char *text, uint32_t len,
                     const process_t *writer)
{
    struct ns_extent map[NS_EXTENTS];
    int gid = which == NS_MAP_GID;
    int n;

    if (ns == &init_userns || !ns_privileged(writer, ns)) {
        return -EPERM;
    }
    if (which == NS_MAP_SETGROUPS) {
        return setgroups_write(ns, text, len);
    }
    if ((gid ? ns->gid_extents : ns->uid_extents) != 0) {
        return -EPERM;
    }
    if (proc_userns(writer) != ns && proc_userns(writer) != ns->parent) {
        return -EPERM;
    }
    n = parse_map(text, len, ns->parent, gid, map);
    if (n < 0) {
        return n;
    }
    if (!ns_privileged(writer, ns->parent) &&
        (n != 1 || map[0].count != 1 || map[0].lower != (gid ? writer->egid : writer->euid) ||
         (gid && !ns->setgroups_denied))) {
        return -EPERM;
    }
    if (gid) {
        k_memcpy(ns->gid_map, map, sizeof(map[0]) * (uint32_t)n);
        ns->gid_extents = (uint32_t)n;
    } else {
        k_memcpy(ns->uid_map, map, sizeof(map[0]) * (uint32_t)n);
        ns->uid_extents = (uint32_t)n;
    }
    return (int32_t)len;
}

int ns_setgroups_denied(const process_t *p)
{
    const struct userns *ns;

    for (ns = proc_userns(p); ns != &init_userns; ns = ns->parent) {
        if (ns->setgroups_denied) {
            return 1;
        }
    }
    return 0;
}

/* ---- privilege ---- */

/** Whether `p` has privilege in its own user namespace: in the first, an
 * effective uid of 0; in another, the capabilities it was given there, or
 * an effective uid that is 0 there; in both, unless it gave them up. */
static int own_privileged(const process_t *p)
{
    const struct userns *ns = proc_userns(p);

    if (p->nocaps) {
        return 0;
    }
    if (ns == &init_userns) {
        return p->euid == 0;
    }
    return ns_caps_held(p) || ns_uid_from_kernel(ns, p->euid) == 0;
}

int ns_privileged(const process_t *p, const struct userns *ns)
{
    const struct userns *own = proc_userns(p);

    if (!ns) {
        ns = &init_userns;
    }
    for (;;) {
        if (ns == own) {
            return own_privileged(p);
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

int ns_owner_privileged(const process_t *p, const struct ns_common *c)
{
    return ns_privileged(p, c->owner ? c->owner : &init_userns);
}

/* ---- PIDs ---- */

/** The number of `p` in the PID namespace `v`, 0 when `p` is not in `v`
 * or below it. */
static uint32_t pid_in(const process_t *p, const struct pidns *v)
{
    const struct pidns *ns = proc_pidns(p);

    if (v == &init_pidns) {
        return p->pid;
    }
    if (ns->level < v->level) {
        return 0;
    }
    while (ns->level > v->level) {
        ns = ns->parent;
    }
    return ns == v ? p->nspid[v->level] : 0;
}

uint32_t pid_seen_by(const process_t *viewer, uint32_t kid)
{
    const struct pidns *v = proc_pidns(viewer);
    const process_t *p;

    if (v == &init_pidns || kid == 0) {
        return kid;
    }
    p = get_process(kid);
    return p ? pid_in(p, v) : 0;
}

uint32_t pid_seen(uint32_t kid)
{
    return pid_seen_by(current_proc, kid);
}

uint32_t pid_named(uint32_t pid)
{
    const struct pidns *v = proc_pidns(current_proc);
    const process_t *p;

    if (v == &init_pidns || pid == 0) {
        return pid;
    }
    for (p = proc_list; p; p = p->next) {
        if (p->state != PROC_UNUSED && pid_in(p, v) == pid) {
            return p->pid;
        }
    }
    return 0;
}

/** The next free number of the PID namespace `ns`: 1 for its first
 * process, then upward, past the numbers its processes hold, from 2 again
 * after NS_PID_MAX; 0 when every number is held. */
static uint32_t pidns_alloc(struct pidns *ns)
{
    uint32_t tries;

    for (tries = 0; tries < NS_PID_MAX; tries++) {
        const process_t *p;
        uint32_t n = ns->last >= NS_PID_MAX ? 2u : ns->last + 1u;

        ns->last = n;
        for (p = proc_list; p; p = p->next) {
            if (p->state != PROC_UNUSED && p->pidns && pid_in(p, ns) == n) {
                break;
            }
        }
        if (!p) {
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
    return ns;
}

/** A new PID namespace below `parent`, owned by `owner`; NULL with `*err`
 * set as for userns_new. */
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

/** A mount namespace with the next free bit; NULL with `*err` -ENOSPC when
 * NS_MNT_MAX exist, or -ENOMEM. Its mounts are those `from` has. */
static struct mntns *mntns_new(const struct mntns *from, struct userns *owner, int32_t *err)
{
    struct mntns *ns;
    uint32_t bit;

    for (bit = 1; bit < NS_MNT_MAX && (mnt_bits & (1u << bit)); bit++) {
    }
    if (bit == NS_MNT_MAX) {
        *err = -ENOSPC;
        return NULL;
    }
    ns = ns_new(sizeof(*ns), NS_MNT, owner);
    if (!ns) {
        *err = -ENOMEM;
        return NULL;
    }
    ns->bit = bit;
    mnt_bits |= 1u << bit;
    vfs_mntns_copy(from->bit, bit);
    return ns;
}

/**
 * A copy of the set `from` with new namespaces for the CLONE_NEW flags of
 * `flags` other than the user's, owned by `owner`; the new PID namespace is
 * made below `active`, the PID namespace of the process the set is for.
 * NULL with `*err` set on failure.
 */
static struct nsset *nsset_copy(const struct nsset *from, uint32_t flags, struct userns *owner,
                                struct pidns *active, int32_t *err)
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
        struct mntns *m = mntns_new(from->mnt, owner, err);

        if (!m) {
            goto fail;
        }
        ns_put(&s->mnt->c);
        s->mnt = m;
    }
    if (flags & CLONE_NEWNET) {
        struct netns *n = ns_new(sizeof(*n), NS_NET, owner);

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
 * first's child, making it the init of `ns` when it is the namespace's
 * first process. 0, or -ENOMEM when one of them is full or has lost its
 * init. */
static int32_t ns_number(process_t *child, struct pidns *ns)
{
    int first = ns != &init_pidns && ns->last == 0;
    struct pidns *at;

    for (at = ns; at != &init_pidns; at = at->parent) {
        if (!(first && at == ns) && at->reaper == 0) {
            return -ENOMEM;
        }
        child->nspid[at->level] = pidns_alloc(at);
        if (child->nspid[at->level] == 0) {
            return -ENOMEM;
        }
    }
    if (first) {
        ns->reaper = child->pid;
    }
    return 0;
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
    child->ns_caps[0] = parent->ns_caps[0];
    child->ns_caps[1] = parent->ns_caps[1];
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
        set = nsset_copy(set, flags, uns, set->pid_children, &err);
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
    return ns_number(child, pns);
}

void ns_release(process_t *p)
{
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
    p->ns_caps[0] = p->ns_caps[1] = 0;
}

void ns_caps_fill(process_t *p)
{
    p->ns_caps[0] = 0xFFFFFFFFu;
    p->ns_caps[1] = NS_CAPS_HIGH;
}

int ns_caps_held(const process_t *p)
{
    return (p->ns_caps[0] | p->ns_caps[1]) != 0;
}

/* ---- unshare and setns ---- */

/**
 * unshare takes the flags. CLONE_NEWUSER gives the caller a new user namespace,
 * in which it has every capability, and needs no privilege; the other CLONE_NEW
 * flags need privilege in the caller's user namespace, the new one with
 * CLONE_NEWUSER, and give it new namespaces of those kinds, but CLONE_NEWPID,
 * whose namespace is the one the caller's next children are made in.
 * CLONE_FILES and CLONE_FS give it a descriptor table and directories of its
 * own, CLONE_NEWNS and CLONE_NEWUSER the second with it, and CLONE_SYSVSEM,
 * CLONE_THREAD, CLONE_SIGHAND and CLONE_VM are taken for a process of one
 * thread. Cgroup and time namespaces are refused, and so is a flag outside
 * these, in the upper half of an x86-64 long too.
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
                             proc_pidns(self), &err);
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
 * ioctl(2) on a namespace's descriptor, as lsns(8) asks: NS_GET_USERNS gives
 * a descriptor of the user namespace that owns it, NS_GET_PARENT one of the
 * parent of a user or PID namespace, NS_GET_NSTYPE its CLONE_NEW flag and
 * NS_GET_OWNER_UID the uid of a user namespace's maker, as the caller sees
 * it, at `arg`. The namespace handed out must be the caller's or below it,
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

const struct file_ops nsfs_ops = {NULL, NULL, NULL, nsfs_ioctl, nsfs_close};

struct ns_common *nsfs_ns(const fd_entry_t *e)
{
    return e && e->type == FD_TYPE_ANON && e->ops == &nsfs_ops && e->priv
               ? ((const struct nsfs_file *)e->priv)->ns
               : NULL;
}

/** Write "KIND:[INUM]" for `c` into `out`, of at least 24 bytes. */
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
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = &nsfs_ops;
    e->priv = f;
    e->flags = FD_FLAG_READ | ((oflag & NS_O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
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
 * capability. */
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

/**
 * setns takes a descriptor open on a namespace, from /proc/PID/ns and its
 * CLONE_NEW flag or 0 for any kind. The caller must be privileged in its own
 * user namespace and in the one that owns the target. A PID namespace becomes
 * the one the caller's next children are made in, and must be the caller's or
 * below it; a mount namespace needs directories of the caller's own.
 */
long sys_setns(isr_params_t *params)
{
    process_t *self = current_proc;
    struct ns_common *c = nsfs_ns(fd_get(self->fds, (int)SC_ARG(params, 0)));
    struct nsset *set;
    int32_t err = 0;

    if (!c) {
        return fd_get(self->fds, (int)SC_ARG(params, 0)) ? -EINVAL : -EBADF;
    }
    if (SC_ARG(params, 1) != 0 && SC_ARG(params, 1) != kind_flags[c->kind]) {
        return -EINVAL;
    }
    if (c->kind == NS_USER) {
        return setns_user((struct userns *)c);
    }
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
    set = nsset_copy(proc_nsset(self), 0, proc_userns(self), proc_pidns(self), &err);
    if (!set) {
        return err;
    }
    switch (c->kind) {
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
    nsset_put(self->nsset);
    self->nsset = set;
    return 0;
}
