#ifndef _NS_H_
#define _NS_H_

#include "../lib/kint.h"
#include "../isr.h"
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
 * takes them before Linux 4.15. */
#define NS_EXTENTS 5

/* The id a map does not cover reads as, Linux's overflowuid and gid. */
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

struct userns {
    struct ns_common c;
    struct userns *parent;
    uint32_t level;
    /* The effective ids of the process that made it, the kernel's. */
    uint32_t owner_uid;
    uint32_t owner_gid;
    struct ns_extent uid_map[NS_EXTENTS];
    uint32_t uid_extents;
    struct ns_extent gid_map[NS_EXTENTS];
    uint32_t gid_extents;
    /* Whether setgroups is denied, as /proc/PID/setgroups's "deny" sets it,
     * which an unprivileged writer of gid_map must do first. */
    uint32_t setgroups_denied;
};

struct pidns {
    struct ns_common c;
    struct pidns *parent;
    uint32_t level;
    /* The last number given out in it, and the kernel's id of its first
     * process, its init, 0 until it is made. */
    uint32_t last;
    uint32_t reaper;
};

struct mntns {
    struct ns_common c;
    /* Its bit in a mount's `ns_hidden` (vfs.h). */
    uint32_t bit;
};

struct netns {
    struct ns_common c;
};

struct utsns {
    struct ns_common c;
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
 * Whether `p` has privilege in the user namespace `ns`: privileged in its
 * own user namespace, which is `ns` or one of its ancestors, or the owner of
 * `ns`'s parent... as Linux's ns_capable answers for a process that keeps
 * every capability or none. A process of the first namespace with effective
 * uid 0 has it everywhere.
 */
int ns_privileged(const process_t *p, const struct userns *ns);

/** Whether `p` has privilege over the namespace `c`, in its owner. */
int ns_owner_privileged(const process_t *p, const struct ns_common *c);

/** The kernel's id of the id `id` of the namespace `ns`, or -1 when its map
 * does not cover it. */
int64_t ns_uid_to_kernel(const struct userns *ns, uint32_t id);
int64_t ns_gid_to_kernel(const struct userns *ns, uint32_t id);

/** The id in `ns` of the kernel's id `kid`, NS_OVERFLOW_ID when its map
 * does not cover it. */
uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t kid);
uint32_t ns_gid_from_kernel(const struct userns *ns, uint32_t kid);

/** Whether the maps of `ns` cover both the kernel's user id `uid` and its
 * group id `gid`, as an exec needs before it applies a file's set-id bits. */
int ns_ids_mapped(const struct userns *ns, uint32_t uid, uint32_t gid);

/* The files of /proc/PID that show a user namespace's maps. */
#define NS_MAP_UID       0
#define NS_MAP_GID       1
#define NS_MAP_SETGROUPS 2

/**
 * The text of /proc/PID/uid_map, gid_map or setgroups, `which`, of the user
 * namespace `ns`, as `reader` reads it: a line of ten-column numbers for
 * each extent, the ids in `ns`, the kernel's ids as the reader's namespace
 * sees them, or as `ns`'s parent sees them when the reader is in `ns`, and
 * the count; or "allow" or "deny". Into `out` of `cap` bytes,
 * NUL-terminated; its length.
 */
int ns_map_text(const struct userns *ns, int which, const process_t *reader, char *out, int cap);

/**
 * A write by `writer` of the `len` bytes of `text` to that file of `ns`,
 * as Linux takes it: a map is written once, by a process in `ns` or its
 * parent privileged in `ns`, and maps ids its parent maps; a writer not
 * privileged in the parent maps one id, its own effective id, and for the
 * groups only after setgroups was denied. setgroups takes "deny", or
 * "allow" when it was not denied, before the group map is written. The
 * length, or -EPERM or -EINVAL.
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
 * 0 for none it can see. */
uint32_t pid_named(uint32_t pid);

/** pid_seen for the viewer `viewer` in place of the current process. */
uint32_t pid_seen_by(const process_t *viewer, uint32_t kid);

/**
 * The child `child` takes `parent`'s namespaces, and new ones for the
 * CLONE_NEW flags of `flags`, and its numbers in the PID namespaces it is
 * in. 0, or -EINVAL for a flag a thread may not have, -EPERM for a
 * namespace the parent may not make, -EUSERS past NS_LEVELS, -ENOMEM.
 */
int32_t ns_fork(process_t *child, const process_t *parent, uint32_t flags);

/** Drop `p`'s references to its namespaces, at the end of its record. */
void ns_release(process_t *p);

/** At an exec: the process's privilege in its user namespace is that of its
 * new effective uid, as Linux gives an image its capabilities. */
void ns_exec(process_t *p);

/* The capabilities Linux names above 31, in the second word of a set. */
#define NS_CAPS_HIGH 0x1FFu

/** Give `p` every capability in its user namespace, as making or joining
 * one does; and whether it holds any there. A process privileged in its
 * namespace holds all of them in effect: the sets capset narrows are
 * reported, and only their emptiness changes what the process may do. */
void ns_caps_fill(process_t *p);
int ns_caps_held(const process_t *p);

/** unshare (310) and setns (346). */
int32_t sys_unshare(isr_params_t *params);
int32_t sys_setns(isr_params_t *params);

/**
 * The init of the PID namespace of `p`, when `p` is not in the first: the
 * kernel id children are given to when their parent ends, 0 for the
 * system's init. A namespace whose init has ended has none, and a process
 * made in it is refused.
 */
uint32_t ns_reaper(const process_t *p);

/** The init of a PID namespace, `p`, is ending: every other process of the
 * namespace is sent SIGKILL. */
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
 * caller drops with `ns_put`, for /proc/PID/ns.
 */
struct ns_common *ns_of(const process_t *p, int kind);
void ns_put(struct ns_common *c);

/** The name of kind `kind` as /proc/PID/ns names its file, "user" to
 * "ipc"; NULL past them. */
const char *ns_kind_name(int kind);

/** Write "KIND:[INUM]", the target of a /proc/PID/ns link, for `c` into
 * `out`, of at least 24 bytes. */
void ns_link_name(const struct ns_common *c, char *out);

/** A descriptor open on `c`, with O_CLOEXEC from `oflag`, as an open of a
 * /proc/PID/ns file gives; or -errno. */
int32_t nsfs_open(struct ns_common *c, uint32_t oflag);

/** The namespace the descriptor `e` is open on, NULL for another kind of
 * descriptor. */
struct fd_entry;
struct ns_common *nsfs_ns(const struct fd_entry *e);

/* What the other subsystems do for their namespaces, defined with them: a
 * new mount namespace's bit is given every mount the bit `from` has
 * (fs/vfs.c), and a mount namespace's end unmounts what only it had; a
 * network namespace's end closes nothing, since its sockets hold it
 * (network/netif.c); an IPC namespace's end removes its objects
 * (ipc/sysv.c). */
void vfs_mntns_copy(uint32_t from, uint32_t to);
void vfs_mntns_gone(uint32_t bit);

/** The bits of the mount namespaces that exist. */
uint32_t mntns_live_bits(void);
void net_ns_gone(struct netns *ns);
void ipc_ns_gone(struct ipcns *ns);

#endif /* _NS_H_ */
