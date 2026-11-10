#ifndef _PROCFS_H_
#define _PROCFS_H_

/*
 * procfs: the files of /proc and /sys, made from the kernel's tables when
 * they are opened, the targets of their links, and the inode numbers stat
 * gives them; the classification of their paths is synthfs.h's. A `pid`
 * argument is the kernel's id of a process or thread, as
 * `procfs_resolve_pid` and `procfs_resolve_id` give it, unless its comment
 * says otherwise.
 */

#include "../lib/kint.h"

struct vpath;
struct fd_entry;
struct vfs_ref;

/* st_dev for /proc nodes, distinct from KRFFS (0) and devfs (DEVFS_FSID). */
#define PROCFS_FSID 6u
/* The device number /sys reports for its nodes. */
#define SYSFS_FSID  7u
/* The device number of a namespace's file, which /proc/<pid>/ns's links
 * reach, Linux's nsfs's 0:4. */
#define NSFS_DEV    4u

/** The kernel's id of the process or thread whose /proc directory has the
 * number `pid` in the caller's PID namespace, or of the caller's process for
 * SYNTH_PID_SELF; -1 when there is none that has not been reaped. */
int procfs_resolve_pid(int pid);

/** Whether the caller may see process `pid`'s memory map and descriptors,
 * as Linux's ptrace read check asks: a thread of its own process, a
 * privileged caller, or one with its real user ID when it is not privileged.
 * 0 when there is no such process. */
int procfs_may_read(int pid);

/** The id of the process, or of the thread for a path under
 * /proc/<pid>/task/<tid>, whose files the path `vp` shows; -1 when the
 * process is gone or the thread is not one its task directory lists: a
 * thread of another process, or one that has ended other than the leader. */
int procfs_resolve_id(const struct vpath *vp);

/** The link count of process `pid`'s task directory: 2 and one for each of
 * its live threads. */
uint32_t procfs_task_links(int pid);

/**
 * Open the file of /proc or /sys at the path `vp`, with open(2)'s `flags`,
 * in the calling process's table. Returns the descriptor or a negative
 * errno: -ENOENT for a file of a process that is gone, -EEXIST for O_CREAT
 * with O_EXCL, -ENOTDIR for O_DIRECTORY, -EACCES for an open for writing or
 * with O_CREAT of a file that takes no write, and for a caller who may not
 * open the file as it asks, -ENOMEM when the heap has no room for the text,
 * -EOVERFLOW for a /proc/disk text past its bound, or the error of making
 * the text, such as -EACCES for the memory of a process the caller may not
 * read.
 *
 * A file is a snapshot: its text is made at the open, and every read of the
 * open file description is served from it until the last close. The files
 * a write reaches have descriptors of their own, whose reads and writes
 * reach the value as it is then: /proc/<pid>/oom_score_adj and
 * /proc/<pid>/mem, and, opened for writing, a writable /proc/sys or /sys
 * file and /proc/<pid>/uid_map, gid_map and setgroups.
 */
int procfs_open(const struct vpath *vp, int flags);

/** Whether `e` is a descriptor of a /proc/PID/mem, whose offset is an
 * address. */
int procfs_is_mem(const struct fd_entry *e);

/**
 * The target of /proc/<pid>/fd/<fd> into `buf` of `cap` bytes, at least
 * one, cut to `cap` - 1 bytes and NUL-terminated. Returns its length,
 * -ENOENT for no such process or no such open descriptor, or -ENOMEM.
 */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap);

/**
 * The target of /proc/<pid>/exe, the path of the program `pid` runs, into
 * `buf` of `cap` bytes with no terminator: the present path of the program's
 * selected entry, relative to the reader's root, followed by " (deleted)"
 * after that entry is removed, even if another hard link survives. Types
 * without a recorded entry use their per-node path and link count; the
 * saved exec path is used when no present path can be read.
 * Its length, -ENOENT for no such process or no program, or -EACCES for a
 * caller that may not read the process's memory, as Linux asks a ptrace
 * read check of the reader.
 */
int procfs_readlink_exe(int pid, char *buf, int cap);

/** The same text without the " (deleted)": the name an execve of the link
 * gives its new image. */
int procfs_exe_name(int pid, char *buf, int cap);

/**
 * The text of /proc/<pid>/cwd, or of /proc/<pid>/root with `root` set, into
 * `buf` of `cap` bytes: the working directory's path as getcwd gives it, or
 * the path of the directory chroot made the root, "/" when it made none.
 * Returns the length, -ENOENT for a process that is gone, -EACCES for a
 * caller that may not read its memory, as Linux asks of these links what it
 * asks of exe, or the error of finding the root's path (`vfs_node_path`).
 */
int procfs_readlink_dir(int pid, int root, char *buf, int cap);

/**
 * The target of /proc/<pid>/ns/<kind>, "KIND:[INUM]", into `buf` of `cap`
 * bytes with no terminator; `kind` is one of proc/ns.h's NS_ values.
 * Returns its length, -ENOENT for no such process or namespace, or -EACCES
 * for a caller that may not read the process's memory.
 */
int procfs_readlink_ns(int pid, int kind, char *buf, int cap);

/** The inode number of process `pid`'s namespace of `kind`; 0 when there is
 * no such process or namespace. */
uint32_t procfs_ns_ino(int pid, int kind);

/** Open process `pid`'s namespace of `kind` with open(2)'s `flags`: a
 * descriptor on the namespace (`nsfs_open`), -ENOENT, or -EACCES as for the
 * link's text, and for an open for writing. */
int procfs_open_ns(int pid, int kind, int flags);

/**
 * The file /proc/<pid>/exe reaches, which the resolver goes on from: the
 * mount and the node of the program's file the process's address space keeps
 * (`mm_exe_open`), which stay while nothing sleeps. 0, -ENOENT for no such
 * process or no program, or -EACCES as for the link's text.
 */
int procfs_exe_file(int pid, struct vfs_ref *out);

/* Hold the selected entry of the same executable, after the caller has
 * obtained its file with no intervening sleep; NULL if none is recorded. */
struct vfs_name;
struct vfs_name *procfs_exe_entry(int pid);

/**
 * The object the /proc link `vp` refers to, which the resolver goes on from
 * when more of a path comes after the link: for /proc/PID/fd/N, the mount and
 * the node of the file or directory the descriptor is open on; for
 * /proc/PID/cwd and root, those of the process's working directory and root.
 * The node is not held, and stays while nothing sleeps. A directory of /dev,
 * /proc or /sys that a descriptor is open on, or a working directory there,
 * has no node, and its path from the root is written into `buf` of `cap`
 * bytes, without a terminator, instead. Returns 0 with `out` filled, the
 * path's length, or a negative errno: -ENOENT for no such process or
 * descriptor, -EACCES for a caller that may not read the process's memory,
 * as Linux asks a ptrace read check of these links, and -ENOTDIR for a
 * descriptor on anything that is neither a file nor a directory, a pipe, a
 * socket or a device, which no path goes on past.
 */
int procfs_link_target(const struct vpath *vp, struct vfs_ref *out, char *buf, int cap);

/** PID-directory enumeration: the pid in the caller's PID namespace of the
 * `n`th process it sees, counting from 0, one for each thread group, or -1
 * past the end. */
int procfs_nth_pid(int n);

/*
 * The inode numbers stat gives /proc and its files outside /proc/<pid>, one
 * function for each, named for the file: /proc itself (`procfs_ino_dir`),
 * /proc/cmdline, /proc/net and /proc/net/dev (`procfs_ino_netdir`,
 * `procfs_ino_netdev`), /proc/disk, /proc/stat (`procfs_ino_statsys`) and
 * the rest. Each number is fixed.
 */
uint32_t procfs_ino_dir(void);
uint32_t procfs_ino_cmdline(void);
uint32_t procfs_ino_netdir(void);
uint32_t procfs_ino_netdev(void);
uint32_t procfs_ino_disk(void);
uint32_t procfs_ino_uptime(void);
uint32_t procfs_ino_statsys(void);
uint32_t procfs_ino_meminfo(void);
uint32_t procfs_ino_cpuinfo(void);
uint32_t procfs_ino_loadavg(void);
uint32_t procfs_ino_diskstats(void);
uint32_t procfs_ino_version(void);
uint32_t procfs_ino_partitions(void);
uint32_t procfs_ino_vmstat(void);
uint32_t procfs_ino_slabinfo(void);
uint32_t procfs_ino_swaps(void);
uint32_t procfs_ino_locks(void);
/** The inode of the /proc/sys entry `entry`, and of /proc/sys itself. */
uint32_t procfs_ino_sys(int entry);
uint32_t procfs_ino_sysdir(void);
uint32_t procfs_ino_mounts(void);
uint32_t procfs_ino_usb(void);
uint32_t procfs_ino_audio(void);
uint32_t procfs_ino_loop(void);
uint32_t procfs_ino_interrupts(void);
uint32_t procfs_ino_console(void);
uint32_t procfs_ino_filesystems(void);
uint32_t procfs_ino_netroute(void);
uint32_t procfs_ino_netarp(void);
uint32_t procfs_ino_nettcp(void);
/** The inode of the /proc/net file of origin `origin`, one with a name from
 * `synth_net_name`: those `synth_net_file` gives and the two files of
 * /proc/net/stat. */
uint32_t procfs_ino_net(int origin);
struct vfs_mount;
/** The device stat gives the files of the mount `m`, entry `i` of the
 * table (`vfs_fill_stat64`): 0 on the KRFFS root; the node of /dev it was
 * mounted from, in Linux's encoding, as a disk's or a loop device's mount
 * has it; else the synthetic types' own numbers, and the entry's index plus
 * 16 for any other mount. */
uint32_t procfs_mount_dev(int i, const struct vfs_mount *m);

/** The name of the `idx`-th file system type /proc/filesystems lists, in
 * `*name`, which sysfs(2) numbers the types by: 1 when the type stands on
 * no device, 0 when it does, or -1 past the last. */
int procfs_filesystem_at(int idx, const char **name);

/** The id /proc/PID/mountinfo gives the mount `m`, its entry's index plus
 * one, which statx reports as STATX_MNT_ID; 0 for NULL or a mount not in
 * the table. */
uint32_t procfs_mount_id(const struct vfs_mount *m);

/** The id of the /dev, /proc or /sys mount a file of device `dev`,
 * DEVFS_FSID, PROCFS_FSID or SYSFS_FSID, is on: `hint`'s when it is of that
 * type, else the first such mount the caller sees; 0 for another device. */
uint32_t procfs_synth_mount_id(uint64_t dev, const struct vfs_mount *hint);

/** The inode of /proc/net/stat. */
uint32_t procfs_ino_netstatdir(void);
/** The inodes of /proc/sysvipc and of its file of origin `origin`. */
uint32_t procfs_ino_sysvipcdir(void);
uint32_t procfs_ino_sysvipc(int origin);
/** The inodes of /proc/acpi and of /proc/acpi/call. */
uint32_t procfs_ino_acpidir(void);
uint32_t procfs_ino_acpicall(void);
/** The inodes of /proc/fs and of /proc/fs/lockwait. */
uint32_t procfs_ino_fsdir(void);
uint32_t procfs_ino_lockwait(void);

/** Whether `e`, a descriptor of type FD_TYPE_PROC, is open on a file of
 * /sys, which fstat and fstatfs report as sysfs's. */
int procfs_fd_is_sys(struct fd_entry *e);

/** The inode of the /sys directory `node`, with its device `key`, when
 * `file` is 0, and of its attribute `file` - 1 when `file` is positive; a
 * PCI function or a USB device has its uevent as `file` 1 and its link as
 * `file` 2. */
uint32_t sysfs_ino(int node, int key, int file);

/** The key of the `n`th device of `node`, SYSFS_PCI_DEVICE,
 * SYSFS_USB_DEVICE or SYSFS_CPUN, counting from zero, or -1 when there are
 * no more. */
int sysfs_device_key(int node, int n);

/** Whether the device, interface, disk node or processor `key` of the keyed
 * /sys directory `node` (SYSFS_KEYED) is there. */
int sysfs_device_exists(int node, int key);

/** Whether the PCI function `key` is there and of the VGA class, so that
 * its directory has boot_vga. */
int sysfs_pci_is_vga(int key);

/** The inode an open descriptor of /proc or /sys, `e`, has by the file it
 * was opened on, so that fstat reports what the stat of its path does; 0
 * when `e` is no such descriptor. */
uint32_t procfs_fd_ino(struct fd_entry *e);

/** The inode of the directory of process `pid` under /proc, which its files
 * share: a base and the low 16 bits of `pid`. */
uint32_t procfs_ino_pid(int pid);

/** The owner of the files and directories of process `pid` under /proc,
 * whose effective user ID may write its oom_score_adj beside a privileged
 * caller: the process's effective user and group IDs while it has an address
 * space that is dumpable, and 0 otherwise. 0, or -ENOENT when there is no
 * such process. */
int procfs_oom_owner(int pid, uint32_t *uid, uint32_t *gid);

/** The same owner for a descriptor open on a file of /proc/<pid>, into
 * `*uid` and `*gid`: 1 for oom_score_adj, uid_map, gid_map and setgroups,
 * whose mode lets the owner write them, and 0 for another file; -1 when `e`
 * is no such descriptor or its process is gone. */
int procfs_fd_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid);

/** The permission bits of `e` when it is open on a /proc/sys or /sys file,
 * or /proc/acpi/call, a privileged caller may write: 0600 for
 * /proc/acpi/call, which root alone reads, and 0644 for the others; 0 when
 * `e` is no such descriptor. */
int procfs_fd_writable(struct fd_entry *e);

#endif /* _PROCFS_H_ */
