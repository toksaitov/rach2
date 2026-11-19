#ifndef _INOTIFY_H_
#define _INOTIFY_H_

#include "../lib/kint.h"

/*
 * inotify: a descriptor that reports changes to the files a program watches,
 * as Linux's does. `inotify_add_watch` names a file or a directory and the
 * events wanted of it; the kernel queues an event on the descriptor when one
 * happens, and a read returns the queue as `struct inotify_event` records. A
 * watch of a directory also reports what happens to the names in it: a file
 * made, removed or moved, and the data events of a file opened by a name in
 * it.
 *
 * The file systems do not know of it. The calls that change a file tell it:
 * the namespace operations of `vfs.c` with the mount, the directory's inode
 * number and the name, and its attribute changes and the descriptor calls
 * with the file's mount and inode number, the descriptor calls through the
 * open file description, which has both. A watch is kept by the base mount
 * of the file system (`vfs_mount_base`) and the inode number, so a change
 * seen through a bind mount reaches it. A caller tests `inotify_watches`
 * before it tells of a change, or `inotify_mount_watched` where it knows the
 * mount, and `inotify_desc` tests the latter itself, so a machine with no
 * watch, or none on the file's file system, pays one comparison for each.
 * A descriptor call without the big lock asks `inotify_desc_wants` first,
 * and takes the lock for `inotify_desc` only when a watch may hear it.
 */

struct isr_params;
struct vfs_mount;
struct open_file;

/* The event bits and flags of Linux's inotify(7). */
#define IN_ACCESS        0x00000001u
#define IN_MODIFY        0x00000002u
#define IN_ATTRIB        0x00000004u
#define IN_CLOSE_WRITE   0x00000008u
#define IN_CLOSE_NOWRITE 0x00000010u
#define IN_OPEN          0x00000020u
#define IN_MOVED_FROM    0x00000040u
#define IN_MOVED_TO      0x00000080u
#define IN_CREATE        0x00000100u
#define IN_DELETE        0x00000200u
#define IN_DELETE_SELF   0x00000400u
#define IN_MOVE_SELF     0x00000800u
#define IN_UNMOUNT       0x00002000u
#define IN_Q_OVERFLOW    0x00004000u
#define IN_IGNORED       0x00008000u
#define IN_ALL_EVENTS    0x00000FFFu
#define IN_ONLYDIR       0x01000000u
#define IN_DONT_FOLLOW   0x02000000u
#define IN_EXCL_UNLINK   0x04000000u
#define IN_MASK_CREATE   0x10000000u
#define IN_MASK_ADD      0x20000000u
#define IN_ISDIR         0x40000000u
#define IN_ONESHOT       0x80000000u

/* How many watches the machine has; every hook does nothing while it is 0. */
extern int inotify_watches;

/**
 * Whether a watch is on the file system of `mnt`, which may be NULL, as
 * Linux's `fsnotify_sb_has_watchers` asks of a superblock before any event
 * (include/linux/fsnotify.h:34-37, 81): one word read, which a caller
 * without the big lock may make, so that a read, an open or a close of a
 * file elsewhere takes no lock for inotify.
 */
int inotify_mount_watched(const struct vfs_mount *mnt);

/* fs.inotify, at Linux's defaults: the instances and the watches one user
 * may have, and the events an instance may queue before the next one is
 * lost and an IN_Q_OVERFLOW takes its place. A value written below what a
 * user or a queue holds takes nothing away; it refuses the next. */
extern int inotify_max_user_instances;
extern int inotify_max_user_watches;
extern int inotify_max_queued_events;

/** An event of the file `ino` of `mnt` itself, to its own watches: IN_ISDIR
 * is added for a watch of a directory. */
void inotify_file(struct vfs_mount *mnt, uint64_t ino, uint32_t mask);

/** An event of the entry `name` of the directory `dir_ino` of `mnt`, to the
 * directory's watches, with `cookie` pairing the two events of a move. */
void inotify_child(struct vfs_mount *mnt, uint64_t dir_ino, const char *name, uint32_t mask,
                   uint32_t cookie);

/** An event of an open file: to its own watches, and to those of its
 * directory, under its name there (`inotify_desc_dir`); nothing while there
 * is no watch, or for a description that is not of a file of a mount.
 * Under the big lock. */
void inotify_desc(struct open_file *ofd, uint32_t mask);

/**
 * Whether an event of `mask` of the open file of `ofd` may reach a watch:
 * one of the file, or, when the open recorded its directory, of that
 * directory, or, when the description holds its name record (`name`), of
 * any directory of its file system, may want it. Without the big lock, as
 * Linux tests the inode's and the parent's i_fsnotify_mask before an event
 * (`__fsnotify_parent`, fs/notify/fsnotify.c): 0 means no watch wants it
 * now, so that the read, open, write or close of a file nobody watches
 * takes no lock for inotify, though another file of its file system is
 * watched. A watch added meanwhile may miss the event, as on Linux.
 */
int inotify_desc_wants(const struct open_file *ofd, uint32_t mask);

/** Whether a watch of a directory of the file system of `mnt` may want an
 * event of `mask`; without the big lock, as `inotify_desc_wants`. */
int inotify_dir_wants(const struct vfs_mount *mnt, uint32_t mask);

/**
 * The directory of the open file of `ofd` for inotify, its inode number
 * into `*dir_ino` and the file's name in it into `*leaf`: what the open
 * recorded (`leaf`, `parent_ino`), else what the description's name record
 * holds now (`vfs_name_dir`). 0, or -1 when there is neither. Under the big
 * lock; `*leaf` stays the description's.
 */
int inotify_desc_dir(const struct open_file *ofd, uint64_t *dir_ino, const char **leaf);

/** The file `ino` of `mnt` has no name left: IN_DELETE_SELF to its watches,
 * then IN_IGNORED, and the watches go. */
void inotify_gone(struct vfs_mount *mnt, uint64_t ino);

/** `mnt` is being unmounted: IN_UNMOUNT and IN_IGNORED to every watch of a
 * file on it, and the watches go. A watch is kept by the base mount, so the
 * unmount of a bind mount ends none. */
void inotify_unmount(struct vfs_mount *mnt);

/** A new cookie, never 0, for the IN_MOVED_FROM and IN_MOVED_TO of one
 * rename. */
uint32_t inotify_cookie(void);

/** Whether the descriptor `fd` of the caller is an inotify instance. */
int inotify_is(int fd);

/**
 * Add the watch the caller's `inotify_add_watch` resolved to the instance
 * at `fd`: the file `ino` of `mnt`, a directory when `isdir`, for the events
 * of `mask`. A second watch of the same file by the same instance changes
 * the first, whose descriptor it returns. Returns the watch descriptor,
 * -EINVAL when `fd` is not an instance, -EEXIST under IN_MASK_CREATE for a
 * file already watched, -ENOSPC past the user's watches, or -ENOMEM.
 */
int32_t inotify_watch(int fd, struct vfs_mount *mnt, uint64_t ino, int isdir, uint32_t mask);

/** inotify_init1(2) takes the flags IN_NONBLOCK and IN_CLOEXEC, and
 * inotify_init(2) none. Returns the descriptor of a new instance, -EINVAL
 * for another flag, -EMFILE past the user's instances (the sysctl
 * fs.inotify.max_user_instances) or the descriptors a process may have, or
 * -ENOMEM. */
long sys_inotify_init(struct isr_params *params);
long sys_inotify_init1(struct isr_params *params);

/** inotify_rm_watch(2) takes the instance and the watch descriptor, and
 * removes the watch with an IN_IGNORED event. Returns 0, -EBADF, or -EINVAL
 * for a descriptor that is not an instance or a watch it does not have. */
long sys_inotify_rm_watch(struct isr_params *params);

#endif /* _INOTIFY_H_ */
