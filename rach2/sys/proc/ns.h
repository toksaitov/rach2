#ifndef _NS_H_
#define _NS_H_

#include "../lib/kint.h"
#include "../irq.h"
#include "process.h"

/*
 * Namespaces: the views of the system a process has, as Linux divides them
 * and unshare(1) and the browser's sandbox ask for them. A user namespace
 * maps user and group ids and scopes privilege; a PID namespace numbers its
 * processes from 1; a mount namespace has its own mount table; a network
 * namespace its own sockets and no interface; a UTS namespace its own host
 * and domain names; and an IPC namespace its own System V objects and POSIX
 * message queues. See ns.Readme.md.
 *
 * A process's ids, privilege checks and file owners stay the kernel's, those
 * of the first namespaces, throughout; a user namespace changes what the
 * calls that report or take ids see. A record whose pointer is NULL is in
 * the first namespace of that kind.
 */

#define NS_USER  0
#define NS_PID   1
#define NS_MNT   2
#define NS_NET   3
#define NS_UTS   4
#define NS_IPC   5
#define NS_KINDS 6

/* The id extents a map of a user namespace may have, as /proc/PID/uid_map
 * takes them since Linux 4.15; and those a map keeps in the namespace's
 * record, searched in the order written, as Linux keeps its first five. */
#define NS_EXTENTS      340
#define NS_BASE_EXTENTS 5

/* The id a map does not cover reads as at boot, Linux's overflowuid and gid. */
#define NS_OVERFLOW_ID 65534u

/* What every namespace has: its references, its inode number in nsfs, its
 * kind, and the user namespace that owns it, NULL for the first. */
struct ns_common {
    uint32_t refs;
    uint32_t inum;
    uint32_t kind;
    struct userns *owner;
};

/* `count` ids from `first` in the namespace are the kernel's from `lower`. */
struct ns_extent {
    uint32_t first;
    uint32_t lower;
    uint32_t count;
};

/* A map of a user namespace: up to NS_BASE_EXTENTS extents in `base`, in
 * the order written; past that, as on Linux, two copies from the heap, one
 * sorted by the first id in the namespace and one by the first kernel id,
 * each searched by halves. */
struct ns_idmap {
    uint32_t extents; /* 0 until the map is written; written once */
    struct ns_extent base[NS_BASE_EXTENTS];
    struct ns_extent *forward; /* by `first`, NULL up to NS_BASE_EXTENTS */
    struct ns_extent *reverse; /* by `lower`, NULL up to NS_BASE_EXTENTS */
};

struct userns {
    struct ns_common c;
    struct userns *parent;
    /* 0 for the first, and one more for each level below; under NS_LEVELS. */
    uint32_t level;
    /* The effective ids of the process that made it, the kernel's. */
    uint32_t owner_uid;
    uint32_t owner_gid;
    struct ns_idmap uid_map;
    struct ns_idmap gid_map;
    /* Whether setgroups is denied, as /proc/PID/setgroups's "deny" sets it,
     * which an unprivileged writer of gid_map must do first; a namespace
     * below one that denies it denies it too, whatever this says. */
    uint32_t setgroups_denied;
    /* Its user.max_mnt_namespaces: the mount namespaces a user may own at
     * once in it and in those below it (`mntns_charge_ok`). The first's is
     * NS_LIMIT_UNSET until first read, then half the threads Linux would
     * allow the machine's memory; the others' start at 0x7FFFFFFF. */
    int32_t max_mnt;
};

/* The first user namespace's max_mnt before it is first read. */
#define NS_LIMIT_UNSET (-1)

struct pidns {
    struct ns_common c;
    struct pidns *parent;
    /* 0 for the first, and one more for each level below; under NS_LEVELS. */
    uint32_t level;
    /* The last number given out in it, 0 before its first process; and the
     * kernel's id of that process, its init, 0 before it is made and after
     * it ends. */
    uint32_t last;
    uint32_t reaper;
};

struct mntns {
    struct ns_common c;
    /* Its id in a mount's view (`ns_seen` in vfs.h), the lowest free when
     * it was made; the first's is 0. */
    uint32_t id;
    /* The effective uid of its maker, the kernel's, whom it is charged to
     * in its owner and the owner's ancestors (`mntns_charge_ok`). */
    uint32_t maker_uid;
    /* The list of the live mount namespaces but the first. */
    struct mntns *next;
    struct mntns *prev;
};

struct netdev;

struct netns {
    struct ns_common c;
    /* The namespace's own `lo` (netif_ns_lo), from the heap; NULL in the
     * first, whose interfaces are the machine's. */
    struct netdev *lo;
};

struct utsns {
    struct ns_common c;
    /* Each at most 64 bytes and a NUL, as uname(2) returns them. */
    char hostname[65];
    char domainname[65];
};

struct ipcns {
    struct ns_common c;
};

/* The namespaces of a process besides its user and PID namespaces, shared by
 * the processes that inherit them and changed by a copy, as Linux's nsproxy
 * is: `pid_children` is the PID namespace its next children are made in. */
struct nsset {
    uint32_t refs;
    struct mntns *mnt;
    struct netns *net;
    struct utsns *uts;
    struct ipcns *ipc;
    struct pidns *pid_children;
};

/* The first namespace of each kind, static and never freed. */
extern struct userns init_userns;
extern struct pidns init_pidns;
extern struct mntns init_mntns;
extern struct netns init_netns;
extern struct utsns init_utsns;
extern struct ipcns init_ipcns;

/** The namespaces of `p`, the first of their kinds where `p` has none of
 * its own. */
struct userns *proc_userns(const process_t *p);
struct pidns *proc_pidns(const process_t *p);
struct mntns *proc_mntns(const process_t *p);
struct netns *proc_netns(const process_t *p);
struct utsns *proc_utsns(const process_t *p);
struct ipcns *proc_ipcns(const process_t *p);

/**
 * Whether `p` has privilege in the user namespace `ns`, NULL for the first,
 * as Linux's `cap_capable` answers for CAP_SYS_ADMIN. `p` has it when `ns`
 * is its own user namespace or one below it and `p` is privileged in its
 * own; or when `ns` is below `p`'s own and the namespace on the way up from
 * `ns` whose parent is `p`'s own was made by a process of `p`'s effective
 * uid. In its own namespace `p` is privileged while it holds CAP_SYS_ADMIN
 * there (`cap_word`). A namespace that is not `p`'s own or below it gives 0.
 */
int ns_privileged(const process_t *p, const struct userns *ns);

/** Whether `p` has privilege over the namespace `c`: privilege in the user
 * namespace that owns it, the first for none. */
int ns_owner_privileged(const process_t *p, const struct ns_common *c);

/** The kernel's id of the id `id` of the namespace `ns`, or -1 when its map
 * does not cover it. */
int64_t ns_uid_to_kernel(const struct userns *ns, uint32_t id);
int64_t ns_gid_to_kernel(const struct userns *ns, uint32_t id);

/* kernel.overflowuid and overflowgid: the user and group ids an id a map
 * does not cover reads as, NS_OVERFLOW_ID at boot, from 0 to 65535. */
extern int ns_overflowuid;
extern int ns_overflowgid;

/** The id in `ns` of the kernel's id `kid`, `ns_overflowuid` or
 * `ns_overflowgid` when its map does not cover it. */
uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t kid);
uint32_t ns_gid_from_kernel(const struct userns *ns, uint32_t kid);

/** Whether the maps of `ns` cover both the kernel's user id `uid` and its
 * group id `gid`, as an exec needs before it applies a file's set-id bits. */
int ns_ids_mapped(const struct userns *ns, uint32_t uid, uint32_t gid);

/* The files of /proc/PID that show a user namespace's maps. */
#define NS_MAP_UID       0
#define NS_MAP_GID       1
#define NS_MAP_SETGROUPS 2

/* The bytes the text of the longest map takes, its NUL among them: a line
 * of three ten-column numbers for each extent. */
#define NS_MAP_TEXT_MAX (NS_EXTENTS * 33 + 1)

/**
 * The text of /proc/PID/uid_map, gid_map or setgroups, `which`, of the user
 * namespace `ns`, as `reader` reads it: a line of ten-column numbers for
 * each extent, the ids in `ns`, the kernel's ids as the reader's namespace
 * sees them, or as `ns`'s parent sees them when the reader is in `ns`, and
 * the count, in the order written, or by the first id for a map of more
 * than NS_BASE_EXTENTS extents, as Linux lists one; or "allow" or "deny".
 * Into `out` of `cap` bytes, cut short
 * where it does not fit, NUL-terminated; its length.
 */
int ns_map_text(const struct userns *ns, int which, const process_t *reader, char *out, int cap);

/**
 * A write by `writer` of the `len` bytes of `text` to the file `which` of
 * `ns`, as Linux takes it: a map is written once, by a process in `ns` or
 * its parent privileged in `ns`, and maps ids its parent maps; a writer not
 * privileged in the parent maps one id, its own effective id, and for the
 * groups only after setgroups was denied. setgroups takes "deny", or
 * "allow" when it was not denied, before the group map is written, from a
 * writer privileged in `ns`. The first namespace's files are not written.
 * The length, or -EPERM, -ENOMEM, or -EINVAL, for more than NS_EXTENTS
 * lines among others.
 */
int32_t ns_map_write(struct userns *ns, int which, const char *text, uint32_t len,
                     const process_t *writer);

/** Whether setgroups(2) is denied to `p`, in its user namespace or an
 * ancestor. */
int ns_setgroups_denied(const process_t *p);

/** The same for the current process's user namespace: what it sees of a
 * kernel id, and the kernel id of what it names, -1 for none. */
uint32_t uid_seen(uint32_t kid);
uint32_t gid_seen(uint32_t kid);
int64_t uid_named(uint32_t id);
int64_t gid_named(uint32_t id);

/**
 * The PID of the process or thread whose kernel id is `kid` as the current
 * process sees it: its number in the current PID namespace, 0 when it is
 * not in that namespace or below it.
 */
uint32_t pid_seen(uint32_t kid);

/** The kernel id of the process or thread the current process names `pid`,
 * 0 for none it can see; `pid` itself in the first PID namespace. */
uint32_t pid_named(uint32_t pid);

/** The process or thread the current process names `pid`, as `pid_named`
 * finds it, or NULL for none it can see. No `pid` names the idle process,
 * whose kernel id 0 is also `pid_named`'s answer for none, so 0 gives NULL. */
process_t *proc_named(uint32_t pid);

/** pid_seen for the viewer `viewer` in place of the current process. */
uint32_t pid_seen_by(const process_t *viewer, uint32_t kid);

/**
 * The child `child` takes `parent`'s namespaces, and new ones for the
 * CLONE_NEW flags of `flags`, and its numbers in the PID namespaces it is
 * in. 0, or -EINVAL for CLONE_NEWUSER or CLONE_NEWPID with CLONE_THREAD,
 * CLONE_NEWUSER with CLONE_FS, or CLONE_THREAD when the parent's next
 * children have a PID namespace other than its own; -EPERM for a namespace
 * the parent may not make, -EUSERS past NS_LEVELS, -ENOSPC for a mount
 * namespace past a user's max_mnt_namespaces, or -ENOMEM. A child that
 * failed may have taken some of the references, which the release of its
 * record drops (`ns_release`).
 */
int32_t ns_fork(process_t *child, const process_t *parent, uint32_t flags);

/** Drop `p`'s references to its namespaces, at the end of its record. */
void ns_release(process_t *p);

/** At an exec: the process's privilege in its user namespace is that of its
 * new effective uid, as Linux gives an image its capabilities. */
void ns_exec(process_t *p);

/* The capabilities Linux names, 0 to CAP_LAST_CAP, in two words: the second
 * has the nine above 31. CAP_SETPCAP controls PR_CAPBSET_DROP,
 * CAP_SYS_CHROOT controls chroot, and CAP_SYS_ADMIN controls namespace
 * operations and global privilege (`nocaps` in process.h). */
#define CAP_LAST_CAP   40
#define CAP_HIGH_WORD  0x1FFu
#define CAP_SETPCAP    8
#define CAP_SYS_CHROOT 18
#define CAP_SYS_ADMIN  21

/**
 * Whether `p`'s ids give it the capabilities it has not lost: its
 * effective uid is 0 in its user namespace, or it made or joined that
 * namespace and has not since run an exec or changed its effective uid
 * (`ns_granted`).
 */
int cap_ids(const process_t *p);

/**
 * Word `i` of the capabilities `p` holds, its effective and permitted sets
 * alike: those it has not lost (`cap_lost`) when its ids give them
 * (`cap_ids`) or an exec gave it its ambient set (`amb_granted`); none
 * otherwise.
 */
uint32_t cap_word(const process_t *p, uint32_t i);

/** Whether `p` holds the capability `cap`, 0 to CAP_LAST_CAP. */
int cap_holds(const process_t *p, uint32_t cap);

/** Make `lost0` and `lost1` the capabilities `p` has lost, `cap_lost`, and
 * set `nocaps` by whether CAP_SYS_ADMIN is among them. */
void cap_store(process_t *p, uint32_t lost0, uint32_t lost1);

/** Give `p` every capability in its user namespace, whatever its ids there,
 * a full bounding set and empty inheritable and ambient sets, as making or
 * joining one does on Linux. */
void ns_caps_fill(process_t *p);

/**
 * unshare(2) takes the flags. CLONE_NEWUSER gives the caller, which must be
 * a process of one thread, a new user namespace, in which it has every
 * capability, and needs no privilege. The other CLONE_NEW flags need
 * privilege in the caller's user namespace, or in the new one with
 * CLONE_NEWUSER, and give the caller new namespaces of those kinds, except
 * that CLONE_NEWPID's is the one its next children are made in, and is
 * -EINVAL when those already have a namespace other than the caller's.
 * CLONE_FILES and CLONE_FS give it a descriptor table and directories of its
 * own, and CLONE_NEWNS and CLONE_NEWUSER imply CLONE_FS. CLONE_SYSVSEM is
 * taken and does nothing, and CLONE_THREAD, CLONE_SIGHAND and CLONE_VM are
 * taken for a process of one thread. A cgroup or time namespace, or a flag
 * outside these, in the upper half of an x86-64 long too, is -EINVAL.
 * 0, or -EINVAL, -EPERM, -EUSERS, -ENOSPC or -ENOMEM.
 */
long sys_unshare(isr_params_t *params);

/**
 * setns(2) takes a descriptor open on a namespace, as an open of a
 * /proc/PID/ns file gives, and the namespace's CLONE_NEW flag or 0 for any
 * kind. Joining a user namespace other than its own needs a process of one
 * thread whose directories are its own, privileged in that namespace, and
 * gives it every capability there. Joining another kind needs privilege in
 * the caller's user namespace and in the one that owns the target. A PID
 * namespace must be the caller's or below it, and becomes the one its next
 * children are made in; a mount namespace needs directories of the
 * caller's own. A pidfd in place of the descriptor takes the CLONE_NEW
 * flags of one or more kinds, and the caller joins each of those namespaces
 * of its process or none: the user namespace first, the others then judged
 * with the capabilities it gives. 0, or -EBADF for no descriptor, -EINVAL
 * for one that is neither a namespace's nor a pidfd, a flag of another kind
 * or a caller these rules exclude, -EPERM without the privilege, -ESRCH for
 * a pidfd's process that has ended, or -ENOMEM.
 */
long sys_setns(isr_params_t *params);

/**
 * The init of the PID namespace of `p`, when `p` is not in the first: the
 * kernel id children are given to when their parent ends, 0 for the
 * system's init. A namespace whose init has ended has none, and a fork into
 * it fails with -ENOMEM.
 */
uint32_t ns_reaper(const process_t *p);

/** Called as `p` ends: when `p` is the init of its PID namespace, the
 * namespace loses its init, and every other process in it or below it that
 * has not ended is sent SIGKILL. Otherwise it does nothing. */
void ns_reaper_gone(const process_t *p);

/* The bits of CLONE_NEW flags clone and unshare take. */
#define CLONE_NEWNS     0x00020000u
#define CLONE_NEWCGROUP 0x02000000u
#define CLONE_NEWUTS    0x04000000u
#define CLONE_NEWIPC    0x08000000u
#define CLONE_NEWUSER   0x10000000u
#define CLONE_NEWPID    0x20000000u
#define CLONE_NEWNET    0x40000000u
#define CLONE_NEW_ANY \
    (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)

/**
 * The namespace of kind `kind` of the process `p`, with a reference the
 * caller drops with `ns_put`, for /proc/PID/ns; NULL past the kinds.
 */
struct ns_common *ns_of(const process_t *p, int kind);

/**
 * Drop a reference to `c`, which may be NULL. The last one frees the
 * namespace and does what the end of its kind does: a mount namespace's
 * mounts and bit go (`vfs_mntns_gone`), a network or IPC namespace's
 * objects are let go (`net_ns_gone`, `ipc_ns_gone`), and the references it
 * had to its owner, and to its parent for a user or PID namespace, are
 * dropped in turn. The first namespaces are never freed.
 */
void ns_put(struct ns_common *c);

/** The name of kind `kind` as /proc/PID/ns names its file, "user" to
 * "ipc"; NULL past them. */
const char *ns_kind_name(int kind);

/** Write "KIND:[INUM]", the target of a /proc/PID/ns link, for `c` into
 * `out`, of at least 24 bytes. */
void ns_link_name(const struct ns_common *c, char *out);

/** A descriptor of the current process open on `c`, read-only, with
 * O_CLOEXEC from `oflag`, as an open of a /proc/PID/ns file gives; or
 * -errno. The descriptor has a reference to `c` until it is closed. */
int32_t nsfs_open(struct ns_common *c, uint32_t oflag);

struct fd_entry;
/** The namespace the descriptor `e` is open on, NULL for another kind of
 * descriptor or none. */
struct ns_common *nsfs_ns(const struct fd_entry *e);

/* What the other subsystems do for their namespaces, defined with them. */

/** Make the mount namespace of id `to` see every mount that of id `from`
 * sees: 0, or -ENOMEM, after which `to` sees none (fs/vfs_ns.c). */
int vfs_mntns_copy(uint32_t from, uint32_t to);

/** The end of the mount namespace of id `id`: unmount what it alone saw,
 * and take the id from every mount's view (fs/vfs_ns.c). */
void vfs_mntns_gone(uint32_t id);

/** The user.max_mnt_namespaces of `ns`, as /proc/sys/user shows it to a
 * process of `ns`. */
int32_t ns_max_mnt(struct userns *ns);

/** Set the user.max_mnt_namespaces of `ns` to `v`, from 0 to 0x7FFFFFFF:
 * 0, or -EINVAL out of that range. It limits the namespaces made after. */
int ns_set_max_mnt(struct userns *ns, int32_t v);

/** The end of the network namespace `ns`: its sockets and connections,
 * which have no reference to it, move to a namespace no process is in,
 * nothing is closed, and the datagrams on their way to it are dropped
 * (network/socket/socket.c). */
void net_ns_gone(struct netns *ns);

/** A new network namespace's `lo`, down and with no address, as Linux makes
 * one, from the heap: the namespace frees it with `kfree` when it ends.
 * NULL when memory is short (network/netif.c). */
struct netdev *netif_ns_lo_new(void);

/** The end of the IPC namespace `ns`: its semaphore sets and message queues
 * are removed, its shared memory segments are marked for removal, and its
 * POSIX message queues lose their names (ipc/sysv.c). */
void ipc_ns_gone(struct ipcns *ns);

#endif /* _NS_H_ */
