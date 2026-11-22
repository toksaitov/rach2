/**
 * Drive the namespace limits of proc/ns.c and fs/vfs_ns.c past the old ones:
 * a thousand mount namespaces with a mount each, and id maps of 340 extents.
 *
 * host-deps: rach2/sys/lib/idset.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/walkseq_host.c
 *
 * The test includes both sources whole. Its mount table holds the boot's
 * permanent mounts and the records the rows make, and its unmount lets a
 * record go as the kernel's would, so that the end of a namespace and the
 * reaper act on it. Processes are records of the test's with the ids and
 * namespaces a row gives them; the clone and unshare paths make the
 * namespaces through `ns_fork`, as the kernel's do.
 *
 * Mount namespaces: 1000 made at once by root, each with a mount of its own,
 * each seeing a copy of each of the boot's mounts but the memfd mount, which
 * it sees as it is, and its own mount, and none of the others', the first
 * namespace seeing the boot's mounts and none of the copies; one made from
 * the 500th seeing a copy of the 500th's mount; a copy hidden from its
 * namespace alone; every mount and id gone with the namespaces;
 * user.max_mnt_namespaces of the first user namespace as Linux derives it
 * from memory, refusing the namespace past it with ENOSPC, and the limit of
 * a user namespace below it counted per user, as Linux's ucounts are; a
 * namespace made while a remount is under way, without a wait; and a
 * writable remount without privilege of a private bind that another
 * remount seals read-only while it waits, refused with EPERM once the wait
 * ends.
 *
 * Id maps: 340 extents written in descending order, read back sorted by the
 * first id as Linux 4.15 lists a map past five extents, every id mapped both
 * ways and those between them not; a 341st extent and an overlap refused
 * with EINVAL and the map left unwritten; and an extent of a namespace below
 * that its parent's ids do not hold in one extent refused with EPERM, which
 * a map of five extents and one of 340 both answer.
 */
#include "proc/ns.c"
#include "fs/vfs_ns.c"

extern int printf(const char *format, ...);
extern int snprintf(char *str, unsigned long size, const char *format, ...);
extern int strncmp(const char *a, const char *b, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);

/* The mount namespaces the first rows make at once, and the slots of the
 * test's mount table: the permanent ones, and for each namespace its copies
 * of the seven it copies and a mount of its own, and some to spare. */
#define SPACES 1000
#define SLOTS  (VFS_PERMANENT + SPACES * VFS_PERMANENT + 64)

/* The frames the test's machine has: 1 GiB. */
#define FRAMES 262144u

struct percpu cpu_table[NR_CPUS];
process_t *proc_list;
static struct vfs_mount *table[SLOTS];
static int unmounts, copies, moves;
static int failures;
/* The directories every process of the rows shares, which the moves of a
 * new namespace leave as they are (`vfs_root_set`). */
static struct fs_struct dirs;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                          \
    do {                                                  \
        if (!(condition)) {                               \
            printf("ns-limits-test: fail: %s\n", detail); \
            failures++;                                   \
        }                                                 \
    } while (0)

/* What the namespace code calls outside the two sources. */

size_t frame_total_count(void)
{
    return FRAMES;
}

int vfs_mount_slots(void)
{
    return SLOTS;
}

struct vfs_mount *vfs_mount_at(int idx)
{
    return idx >= 0 && idx < SLOTS && table[idx] && table[idx]->state != VFS_FREE ? table[idx]
                                                                                  : NULL;
}

/** Let the record `m` go as the kernel's unmount does: its view's words are
 * freed with it. */
int vfs_unmount_record(struct vfs_mount *m)
{
    int i;

    for (i = VFS_PERMANENT; i < SLOTS; i++) {
        if (table[i] == m) {
            vfs_mount_view_free(m);
            free(m);
            table[i] = NULL;
            unmounts++;
            return 0;
        }
    }
    return -EBUSY;
}

struct vfs_mount *vfs_memfd_mount(void)
{
    return table[4];
}

struct vfs_mount *vfs_root_mount(void)
{
    return table[0];
}

/** vfs.c's copy, as far as the views go: a record in the first free slot
 * past the permanent ones, active, seen by `id` alone, with the flags of
 * `c` and the records it copies. */
struct vfs_mount *vfs_mount_copy(struct vfs_mount *c, struct vfs_mount *parent, uint32_t id)
{
    struct vfs_mount *m;
    int i;

    for (i = VFS_PERMANENT; i < SLOTS && table[i]; i++) {
    }
    m = i < SLOTS ? calloc(1, sizeof(*m)) : NULL;
    if (!m || idset_add(&m->ns_seen, id) != 0) {
        if (m) {
            vfs_mount_view_free(m);
        }
        free(m);
        return NULL;
    }
    m->state = VFS_ACTIVE;
    m->flags = c->flags;
    m->parent = parent;
    m->copy_of = c;
    m->origin = c->origin ? c->origin : c;
    table[i] = m;
    copies++;
    return m;
}

/** The moves of a process into a new namespace, counted and not made, since
 * the rows' processes share one record. */
void vfs_root_set(struct process *p, struct vfs_mount *mnt, void *node)
{
    (void)p;
    (void)mnt;
    (void)node;
    moves++;
}

void vfs_cwd_set(struct process *p, struct vfs_mount *mnt, void *node)
{
    (void)p;
    (void)mnt;
    (void)node;
    moves++;
}

int proc_cwd_set(struct process *p, const char *path)
{
    (void)p;
    (void)path;
    return 0;
}

/** vfs.c's: a bind's own flags, the only mounts the rows remount. */
uint32_t vfs_mount_flags(const struct vfs_mount *m)
{
    return m->flags;
}

/** vfs.c's wait for the unmounts draining their mount; the rows unmount
 * nothing while a namespace is made, so there is none to wait for. */
void vfs_mntns_settle(void)
{}

/** vfs.c's generation of the mounts, which no row reads. */
void vfs_mounts_changed(void)
{}

void vfs_reap_detached(void)
{
    int i;

    for (i = VFS_PERMANENT; i < SLOTS; i++) {
        if (table[i] && table[i]->state == VFS_ACTIVE && vfs_mount_unseen(table[i]) &&
            table[i]->refs == 0) {
            vfs_unmount_record(table[i]);
        }
    }
}

void net_ns_gone(struct netns *ns)
{
    (void)ns;
}

struct netdev *netif_ns_lo_new(void)
{
    return NULL;
}

void ipc_ns_gone(struct ipcns *ns)
{
    (void)ns;
}

int mq_root_made(struct ipcns *ns)
{
    (void)ns;
    return 0;
}

process_t *get_process(uint32_t pid)
{
    (void)pid;
    return NULL;
}

/* What runs while a remount waits for another: another process's call,
 * set by a row, else the other remount's end. */
static void (*while_waiting)(void);

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** The wait for a remount (`vfs_mntns_wait`): a wait that would sleep runs
 * the row's other process, or ends the other remount, as the wake would. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    void (*other)(void) = while_waiting;

    (void)wq;
    if (!cond(arg)) {
        while_waiting = NULL;
        if (other) {
            other();
        } else {
            vfs_mntns_release();
        }
    }
    return cond(arg) ? 0 : -EINVAL;
}

/* The calls of the descriptor, signal and setns paths, which no row takes. */

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -EFAULT;
}

int fd_alloc(fd_table_t *t)
{
    (void)t;
    return -EMFILE;
}

fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    (void)t;
    (void)fd;
    return NULL;
}

int32_t pidfd_lookup(int fd, struct process **out, int *nonblock)
{
    (void)fd;
    (void)out;
    (void)nonblock;
    return -EBADF;
}

int32_t proc_files_unshare(void)
{
    return 0;
}

int32_t proc_fs_unshare(void)
{
    return 0;
}

void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    (void)t;
    (void)signo;
    (void)info;
}

int pid_max = PID_MAX_LIMIT;

int proc_group_held(uint32_t id, int sessions, int unused, const process_t *gone)
{
    (void)id;
    (void)sessions;
    (void)unused;
    (void)gone;
    return 0;
}

uint32_t proc_last_pid(void)
{
    return 0;
}

void proc_set_last_pid(uint32_t pid)
{
    (void)pid;
}

void proc_cred_commit(process_t *p)
{
    p->cred_id++;
}

/** A mount made by the current process in the first free slot past the
 * permanent ones, active and seen by its namespace alone, as `mount_place`
 * makes one; NULL when the table is full. */
static struct vfs_mount *mount_new(void)
{
    struct vfs_mount *m;
    int i;

    for (i = VFS_PERMANENT; i < SLOTS && table[i]; i++) {
    }
    m = i < SLOTS ? calloc(1, sizeof(*m)) : NULL;
    if (!m || vfs_mount_view_new(m) != 0) {
        CHECK(0, "a record and its view");
        free(m);
        return NULL;
    }
    m->state = VFS_ACTIVE;
    table[i] = m;
    return m;
}

/** Make the boot's permanent mounts, seen by the first namespace. */
static void boot(void)
{
    int i;

    current_proc = NULL;
    for (i = 0; i < VFS_PERMANENT; i++) {
        table[i] = calloc(1, sizeof(struct vfs_mount));
        CHECK(table[i] && vfs_mount_view_new(table[i]) == 0, "a permanent mount");
        table[i]->state = VFS_ACTIVE;
    }
}

/** Give `p` the kernel's ids `uid`, as the clone path's parent. */
static void as_user(process_t *p, uint32_t uid)
{
    memset(p, 0, sizeof(*p));
    p->fs = &dirs;
    p->pid = 100;
    p->ruid = p->euid = p->suid = uid;
    p->rgid = p->egid = p->sgid = uid;
}

/** The mounts past the permanent ones that the current process sees. */
static int seen_past_boot(void)
{
    int i, n = 0;

    for (i = VFS_PERMANENT; i < SLOTS; i++) {
        n += table[i] && vfs_mount_seen(table[i]);
    }
    return n;
}

/** The copy the current process's namespace sees of `m`, a mount of the
 * first namespace's or one made by mount(2), by the record its copies were
 * first copied from; NULL when it sees none. */
static struct vfs_mount *copy_seen(const struct vfs_mount *m)
{
    int i;

    for (i = VFS_PERMANENT; i < SLOTS; i++) {
        if (table[i] && table[i]->origin == m && vfs_mount_seen(table[i])) {
            return table[i];
        }
    }
    return NULL;
}

/**
 * A thousand mount namespaces made at once, a mount in each, and the view
 * of each: its copies of the boot's mounts, the memfd mount, its own mount,
 * and nothing of the others'. Then a namespace made from one of them, a
 * copy hidden from its namespace, and the end of them all.
 */
static void thousand(void)
{
    static process_t root, kids[SPACES], copy;
    static struct vfs_mount *own[SPACES];
    struct vfs_mount *again, *dev;
    int i, j, ok;
    uint32_t top = 0;

    as_user(&root, 0);
    for (i = 0; i < SPACES; i++) {
        as_user(&kids[i], 0);
        if (ns_fork(&kids[i], &root, CLONE_NEWNS) != 0) {
            CHECK(0, "the namespaces past the 32nd are made");
            return;
        }
        top = proc_mntns(&kids[i])->id > top ? proc_mntns(&kids[i])->id : top;
        current_proc = &kids[i];
        own[i] = mount_new();
    }
    CHECK(top == SPACES, "the ids are the lowest free, 1 to 1000");
    CHECK(mnt_ids.count == SPACES + 1, "a live id for each namespace and the first");
    CHECK(copies == SPACES * (VFS_PERMANENT - 1),
          "each namespace copies the boot's mounts but the memfd mount");
    CHECK(moves == SPACES, "each new namespace moves its process's root to its copy");
    for (i = 0, ok = 1; i < SPACES; i++) {
        current_proc = &kids[i];
        for (j = 0; j < VFS_PERMANENT; j++) {
            ok &= j == 4 ? vfs_mount_seen(table[j])
                         : !vfs_mount_seen(table[j]) && copy_seen(table[j]) != NULL;
        }
        ok &= own[i] && vfs_mount_seen(own[i]) && seen_past_boot() == VFS_PERMANENT;
    }
    CHECK(ok, "each namespace sees its copies of the boot's mounts, the memfd mount and its own");
    current_proc = &root;
    CHECK(seen_past_boot() == 0, "the first namespace sees no namespace's mount");
    for (j = 0, ok = 1; j < VFS_PERMANENT; j++) {
        ok &= vfs_mount_seen(table[j]);
    }
    CHECK(ok, "and the boot's mounts as they are");

    as_user(&copy, 0);
    current_proc = &kids[500];
    dev = copy_seen(table[1]);
    CHECK(ns_fork(&copy, &kids[500], CLONE_NEWNS) == 0, "a namespace made from the 500th");
    current_proc = &copy;
    again = copy_seen(own[500]);
    CHECK(again && again->copy_of == own[500] && !vfs_mount_seen(own[500]) &&
              seen_past_boot() == VFS_PERMANENT,
          "it sees a copy of each mount the 500th sees, and none of the 500th's records");
    CHECK(copy_seen(table[1]) && copy_seen(table[1])->copy_of == dev,
          "its copy of /dev is a copy of the 500th's");

    current_proc = &kids[700];
    dev = copy_seen(table[1]);
    vfs_mount_hide(dev);
    CHECK(dev && !vfs_mount_seen(dev) && copy_seen(table[1]) == NULL,
          "a copy hidden from the 700th");
    current_proc = &kids[699];
    CHECK(copy_seen(table[1]) != NULL, "and the 699th's copy still seen");

    for (i = 0; i < SPACES; i++) {
        current_proc = &kids[i];
        ns_release(&kids[i]);
    }
    CHECK(unmounts == SPACES * VFS_PERMANENT,
          "each namespace's end unmounts its copies and its mount, and the hidden copy goes");
    current_proc = &copy;
    CHECK(seen_past_boot() == VFS_PERMANENT, "the namespace made from the 500th keeps its own");
    ns_release(&copy);
    CHECK(unmounts == (SPACES + 1) * VFS_PERMANENT && seen_past_boot() == 0,
          "and its end unmounts them");
    for (j = 0, ok = 1; j < VFS_PERMANENT; j++) {
        ok &= table[j]->ns_seen.count == 1 && idset_has(&table[j]->ns_seen, 0);
    }
    CHECK(ok, "the boot's mounts are seen by the first namespace alone again");
    CHECK(mnt_ids.count == 1 && mnt_list == NULL, "every id but the first is free");
}

/**
 * user.max_mnt_namespaces: the first user namespace's is half the threads
 * Linux allows the memory, and a namespace past a limit is ENOSPC; a user
 * namespace below has its own limit, per user, and its namespaces count
 * against the first's for its owner too.
 */
static void limits(void)
{
    static process_t root, kids[41], uparent, user, inner[3];
    int i, made = 0, last = 0;
    struct userns *u;

    CHECK(ns_max_mnt(&init_userns) ==
              (int32_t)((uint64_t)FRAMES * 4096u / (8u * NS_THREAD_SIZE) / 2u),
          "the first limit is half of Linux's max_threads for the memory");
    CHECK(ns_set_max_mnt(&init_userns, -1) == -EINVAL, "a negative limit is EINVAL");
    CHECK(ns_set_max_mnt(&init_userns, 40) == 0, "the limit set to 40");
    as_user(&root, 0);
    current_proc = &root;
    for (i = 0; i < 41; i++) {
        as_user(&kids[i], 0);
        last = ns_fork(&kids[i], &root, CLONE_NEWNS);
        made += last == 0;
    }
    CHECK(made == 40, "40 namespaces are made under a limit of 40");
    CHECK(last == -ENOSPC, "the 41st is ENOSPC");

    as_user(&uparent, 1000);
    as_user(&user, 1000);
    CHECK(ns_fork(&user, &uparent, CLONE_NEWUSER | CLONE_NEWNS) == 0,
          "uid 1000 makes a user and a mount namespace; its count is its own");
    u = user.userns;
    CHECK(u && u->max_mnt == NS_LIMIT_MAX, "a new user namespace's limit is INT_MAX");
    if (!u) {
        return;
    }
    u->max_mnt = 2;
    for (i = 0; i < 3; i++) {
        memset(&inner[i], 0, sizeof(inner[i]));
        inner[i].fs = &dirs;
        inner[i].ruid = inner[i].euid = 1000;
        inner[i].rgid = inner[i].egid = 1000;
    }
    current_proc = &user;
    CHECK(ns_fork(&inner[0], &user, CLONE_NEWNS) == 0, "the user namespace's second");
    CHECK(ns_fork(&inner[1], &user, CLONE_NEWNS) == -ENOSPC,
          "the third past the user namespace's limit of 2 is ENOSPC");
    u->max_mnt = NS_LIMIT_MAX;
    ns_set_max_mnt(&init_userns, 2);
    CHECK(ns_fork(&inner[2], &user, CLONE_NEWNS) == -ENOSPC,
          "and past the first's limit for uid 1000, which counts them too");
    ns_release(&inner[0]);
    ns_release(&inner[1]);
    ns_release(&inner[2]);
    ns_release(&user);
    for (i = 0; i < 41; i++) {
        ns_release(&kids[i]);
    }
    ns_set_max_mnt(&init_userns, NS_LIMIT_MAX);
    CHECK(mnt_ids.count == 1 && mnt_list == NULL, "every id but the first is free again");
}

static process_t waiter_parent, waiter;

/**
 * A namespace made while a remount is under way is made at once: each
 * namespace has copies of its own, whose flags no remount of another's
 * changes, so the making need not wait. A making that waited would end the
 * remount in the test's wait.
 */
static void remount_wait(void)
{
    as_user(&waiter_parent, 0);
    as_user(&waiter, 0);
    current_proc = &waiter_parent;
    vfs_mntns_hold();
    CHECK(ns_fork(&waiter, &waiter_parent, CLONE_NEWNS) == 0 && remounts == 1,
          "a namespace is made while a remount is under way, without a wait");
    vfs_mntns_release();
    ns_release(&waiter);
    CHECK(mnt_ids.count == 1 && mnt_list == NULL, "every id but the first is free after it");
}

/* The bind `remount_locked` remounts, which its other remount seals, and
 * the process without privilege that remounts it. */
static struct vfs_mount *sealed;
static process_t unprivileged;

/** The other remount of `remount_locked`, which seals the bind read-only
 * while the first waits, and ends. */
static void seal_bind(void)
{
    sealed->flags |= VFS_MS_RDONLY;
    vfs_mntns_release();
}

/**
 * A writable remount without privilege of a private bind, which waits for
 * another remount that seals the bind read-only meanwhile: under the hold
 * it must find the seal and answer EPERM, holding nothing, where a check
 * made only before the wait would clear the read-only flag. Without the
 * seal the same remount is held, and a remount that keeps the flag is held
 * after the seal.
 */
static void remount_locked(void)
{
    as_user(&unprivileged, 1000);
    current_proc = &unprivileged;
    sealed = mount_new();
    if (!sealed) {
        return;
    }
    CHECK(vfs_mntns_hold_unprivileged(sealed, 0) == 0 && remounts == 1,
          "a writable remount of an unsealed private bind is held");
    vfs_mntns_release();
    vfs_mntns_hold();
    while_waiting = seal_bind;
    CHECK(vfs_mntns_hold_unprivileged(sealed, 0) == -EPERM,
          "a writable remount that waited while the bind was sealed is EPERM");
    CHECK(while_waiting == NULL && remounts == 0 && (sealed->flags & VFS_MS_RDONLY),
          "the refused remount holds nothing and the seal stays");
    CHECK(vfs_mntns_hold_unprivileged(sealed, VFS_MS_RDONLY) == 0 && remounts == 1,
          "a remount that keeps the seal is held");
    vfs_mntns_release();
    vfs_unmount_record(sealed);
    sealed = NULL;
    current_proc = NULL;
}

/** The map text of `n` extents, `2k 3k 1` for k from n - 1 down to 0, into
 * `out` of `cap` bytes; its length. */
static uint32_t descending(char *out, int cap, int n)
{
    int k, at = 0;

    for (k = n - 1; k >= 0; k--) {
        at += snprintf(out + at, (unsigned long)(cap - at), "%d %d 1\n", 2 * k, 3 * k);
    }
    return (uint32_t)at;
}

/** Whether the map of `ns` maps every id `2k` to the kernel's `3k` and back
 * for k under 340, and the ids between them to none. */
static int maps_both_ways(const struct userns *ns)
{
    uint32_t k;

    for (k = 0; k < NS_EXTENTS; k++) {
        if (ns_uid_to_kernel(ns, 2u * k) != 3 * (int64_t)k ||
            ns_uid_from_kernel(ns, 3u * k) != 2u * k || ns_uid_to_kernel(ns, 2u * k + 1u) != -1 ||
            ns_uid_from_kernel(ns, 3u * k + 1u) != NS_OVERFLOW_ID) {
            return 0;
        }
    }
    return 1;
}

/**
 * A map of 340 extents, written by root outside the namespace, in descending
 * order: read back by the first id, and mapped both ways; a 341st extent and
 * an overlap refused. Then the maps of a namespace below a map of five and
 * one of 340: an extent its parent holds in one extent maps, and one across
 * two is EPERM.
 */
static void maps(void)
{
    static process_t root, child, grand, grand2;
    static char text[NS_EXTENTS * 16], read_back[NS_MAP_TEXT_MAX], line[40];
    struct userns *u, *v;
    uint32_t len;
    int lines = 0, i;

    as_user(&root, 0);
    current_proc = &root;
    as_user(&child, 0);
    CHECK(ns_fork(&child, &root, CLONE_NEWUSER) == 0, "a user namespace");
    u = child.userns;
    if (!u) {
        return;
    }
    len = descending(text, (int)sizeof(text), NS_EXTENTS + 1);
    CHECK(len < 4096u, "341 short lines fit the page a write takes");
    CHECK(ns_map_write(u, NS_MAP_UID, text, len, &root) == -EINVAL, "a 341st extent is EINVAL");
    CHECK(u->uid_map.extents == 0, "and the map is not written");
    CHECK(ns_map_write(u, NS_MAP_UID, "0 0 2\n1 5 1\n", 12, &root) == -EINVAL,
          "an overlap is EINVAL");
    len = descending(text, (int)sizeof(text), NS_EXTENTS);
    CHECK(ns_map_write(u, NS_MAP_UID, text, len, &root) == (int32_t)len, "340 extents are taken");
    CHECK(u->uid_map.extents == NS_EXTENTS && u->uid_map.forward && u->uid_map.reverse,
          "kept in the two sorted copies");
    CHECK(maps_both_ways(u), "every id maps both ways, and those between none");
    ns_map_text(u, NS_MAP_UID, &root, read_back, (int)sizeof(read_back));
    for (i = 0; read_back[i]; i++) {
        lines += read_back[i] == '\n';
    }
    CHECK(lines == NS_EXTENTS && i == NS_EXTENTS * 33, "340 lines of 33 bytes read back");
    snprintf(line, sizeof(line), "%10u %10u %10u\n", 0u, 0u, 1u);
    CHECK(strncmp(read_back, line, 33) == 0, "the first line is the lowest first id");
    snprintf(line, sizeof(line), "%10u %10u %10u\n", 400u, 600u, 1u);
    CHECK(strncmp(read_back + 200 * 33, line, 33) == 0, "the 201st is first id 400");
    CHECK(ns_map_write(u, NS_MAP_UID, "0 0 1\n", 6, &root) == -EPERM, "a map is written once");

    child.ns_granted = 1;
    child.euid = child.ruid = 3u * 7u;
    current_proc = &child;
    as_user(&grand, 3u * 7u);
    CHECK(ns_fork(&grand, &child, CLONE_NEWUSER) == 0, "a user namespace below it");
    v = grand.userns;
    if (!v) {
        return;
    }
    CHECK(ns_map_write(v, NS_MAP_UID, "0 400 2\n", 8, &child) == -EPERM,
          "parent ids across two of the 340 extents are EPERM");
    CHECK(ns_map_write(v, NS_MAP_UID, "0 400 1\n", 8, &child) == 8, "one the parent holds maps");
    CHECK(ns_uid_to_kernel(v, 0) == 600 && ns_uid_from_kernel(v, 600) == 0,
          "through the parent's map to the kernel's id");

    as_user(&grand2, 0);
    current_proc = &root;
    CHECK(ns_fork(&grand2, &root, CLONE_NEWUSER) == 0, "a user namespace of five extents");
    v = grand2.userns;
    if (!v) {
        return;
    }
    CHECK(ns_map_write(v, NS_MAP_UID, "0 100 2\n2 300 2\n4 104 1\n", 24, &root) == 24,
          "a map of three extents");
    ns_release(&grand);
    as_user(&grand, 0);
    grand2.ns_granted = 1;
    current_proc = &grand2;
    CHECK(ns_fork(&grand, &grand2, CLONE_NEWUSER) == 0, "a namespace below it");
    CHECK(grand.userns && ns_map_write(grand.userns, NS_MAP_UID, "0 0 5\n", 6, &grand2) == -EPERM,
          "ids whose ends map five apart but across three extents are EPERM");
    ns_release(&grand);
    ns_release(&grand2);
    ns_release(&child);
}

/** Run the rows; 0 when every check held. */
int main(void)
{
    boot();
    thousand();
    limits();
    remount_wait();
    remount_locked();
    maps();
    if (failures) {
        printf("ns-limits-test: %d failures\n", failures);
        return 1;
    }
    printf("ns-limits-test: all checks passed\n");
    return 0;
}
