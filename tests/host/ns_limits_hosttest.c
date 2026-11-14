/**
 * Drive the namespace limits of proc/ns.c and fs/vfs_ns.c past the old ones:
 * a thousand mount namespaces with a mount each, and id maps of 340 extents.
 *
 * host-deps: rach2/sys/lib/idset.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 *
 * The test includes both sources whole. Its mount table holds the boot's
 * permanent mounts and the records the rows make, and its unmount lets a
 * record go as the kernel's would, so that the end of a namespace and the
 * reaper act on it. Processes are records of the test's with the ids and
 * namespaces a row gives them; the clone and unshare paths make the
 * namespaces through `ns_fork`, as the kernel's do.
 *
 * Mount namespaces: 1000 made at once by root, each with a mount of its own,
 * each seeing the boot's mounts and its own and none of the others', the
 * first namespace seeing none of them; one made from the 500th seeing its
 * mount; a mount hidden from one namespace alone; every mount and id gone
 * with the namespaces; user.max_mnt_namespaces of the first user namespace
 * as Linux derives it from memory, refusing the namespace past it with
 * ENOSPC, and the limit of a user namespace below it counted per user, as
 * Linux's ucounts are.
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
 * test's mount table: the permanent ones, a mount for each namespace and
 * one to spare. */
#define SPACES 1000
#define SLOTS  (VFS_PERMANENT + SPACES + 1)

/* The frames the test's machine has: 1 GiB. */
#define FRAMES 262144u

struct percpu cpu_table[NR_CPUS];
process_t *proc_list;
static struct vfs_mount *table[SLOTS];
static int unmounts;
static int failures;

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

process_t *get_process(uint32_t pid)
{
    (void)pid;
    return NULL;
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

/** A mount made by the current process in slot `slot`, active and seen by
 * its namespace alone, as `mount_alloc` makes one. */
static struct vfs_mount *mount_in(int slot)
{
    struct vfs_mount *m = calloc(1, sizeof(*m));

    if (!m || vfs_mount_view_new(m) != 0) {
        CHECK(0, "a record and its view");
        free(m);
        return NULL;
    }
    m->state = VFS_ACTIVE;
    table[slot] = m;
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

/**
 * A thousand mount namespaces made at once, a mount in each, and the view
 * of each: the boot's mounts, its own, and nothing of the others'. Then a
 * namespace made from one of them, a mount hidden from one, and the end of
 * them all.
 */
static void thousand(void)
{
    static process_t root, kids[SPACES], copy;
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
        mount_in(VFS_PERMANENT + i);
    }
    CHECK(top == SPACES, "the ids are the lowest free, 1 to 1000");
    CHECK(mnt_ids.count == SPACES + 1, "a live id for each namespace and the first");
    for (i = 0, ok = 1; i < SPACES; i++) {
        current_proc = &kids[i];
        for (j = 0; j < VFS_PERMANENT; j++) {
            ok &= vfs_mount_seen(table[j]) && vfs_mount_shared(table[j]);
        }
        ok &= vfs_mount_seen(table[VFS_PERMANENT + i]) && seen_past_boot() == 1 &&
              !vfs_mount_shared(table[VFS_PERMANENT + i]);
    }
    CHECK(ok, "each namespace sees the boot's mounts and its own alone");
    current_proc = &root;
    CHECK(seen_past_boot() == 0, "the first namespace sees no namespace's mount");

    as_user(&copy, 0);
    current_proc = &kids[500];
    CHECK(ns_fork(&copy, &kids[500], CLONE_NEWNS) == 0, "a namespace made from the 500th");
    current_proc = &copy;
    CHECK(vfs_mount_seen(table[VFS_PERMANENT + 500]) && seen_past_boot() == 1,
          "it sees what the 500th sees");
    CHECK(vfs_mount_shared(table[VFS_PERMANENT + 500]), "the 500th's mount is shared now");

    current_proc = &kids[700];
    vfs_mount_hide(table[1]);
    CHECK(!vfs_mount_seen(table[1]), "a mount hidden from the 700th");
    current_proc = &kids[699];
    CHECK(vfs_mount_seen(table[1]), "and still seen by the 699th");

    for (i = 0; i < SPACES; i++) {
        current_proc = &kids[i];
        ns_release(&kids[i]);
    }
    CHECK(unmounts == SPACES - 1, "each namespace's end unmounts what it alone saw");
    CHECK(table[VFS_PERMANENT + 500] != NULL, "a mount another namespace sees stays");
    current_proc = &copy;
    ns_release(&copy);
    CHECK(unmounts == SPACES && seen_past_boot() == 0, "the last view's end unmounts it");
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
    maps();
    if (failures) {
        printf("ns-limits-test: %d failures\n", failures);
        return 1;
    }
    printf("ns-limits-test: all checks passed\n");
    return 0;
}
