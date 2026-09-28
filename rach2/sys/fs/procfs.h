#ifndef _PROCFS_H_
#define _PROCFS_H_

#include "../lib/kint.h"

struct vpath;
struct fd_entry;
struct vfs_ref;

/*
 * procfs: the synthetic /proc, generated per query from the process table.
 * Enough of it for what musl and busybox read: /proc/<pid>/stat and cmdline,
 * /proc/<pid>/fd/<n> magic symlinks (ttyname reads /proc/self/fd/N), and PID
 * directory enumeration (busybox ps). Nothing is stored on disk.
 */

/* st_dev for /proc nodes, distinct from KRFFS (0) and devfs (DEVFS_FSID). */
#define PROCFS_FSID 6u

/** Resolve a vpath pid (SYNTH_PID_SELF is the caller) to a live pid, or -1. */
int procfs_resolve_pid(int pid);

/** The id of the process, or of the thread for a path under
 * /proc/<pid>/task/<tid>, whose files the path `vp` shows; -1 when the
 * process is gone or the thread is not one of its threads. */
int procfs_resolve_id(const struct vpath *vp);

/** The link count of process `pid`'s task directory: 2 and one for each of
 * its live threads. */
uint32_t procfs_task_links(int pid);

/** Open a /proc/<pid>/{stat,cmdline} file; returns the fd or -errno. */
int procfs_open(const struct vpath *vp, int flags);

/** Render the /proc/<pid>/fd/<fd> symlink target into buf (NOT NUL-terminated);
 *  returns the byte length or -errno. */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap);

/**
 * The target of /proc/<pid>/exe, the path of the program `pid` runs, into
 * `buf` of `cap` bytes with no terminator: the present path of the program's
 * file on the KRFFS root, which follows a rename or a relocation of it; the
 * path its exec found it at, followed by " (deleted)" as Linux names one, for
 * a file there that has lost its name; and the path its exec found it at on
 * any other type, whose node has no path of its own to give. Its length,
 * -ENOENT for no such process or no program, or -EACCES for a caller that may
 * not read the process's memory, as Linux asks a ptrace read check of the
 * reader.
 */
int procfs_readlink_exe(int pid, char *buf, int cap);

/** The same text without the " (deleted)": the name an execve of the link
 * gives its new image. */
int procfs_exe_name(int pid, char *buf, int cap);

/**
 * The file /proc/<pid>/exe reaches, which the resolver goes on from: the
 * mount and the node of the program's file the process's address space keeps
 * (`mm_exe_open`), which stay while nothing sleeps. 0, -ENOENT for no such
 * process or no program, or -EACCES as for the link's text.
 */
int procfs_exe_file(int pid, struct vfs_ref *out);

/** PID-directory enumeration: the nth live pid (n >= 0), or -1 past the end. */
int procfs_nth_pid(int n);

/* Stable synthetic inodes for stat. */
uint32_t procfs_ino_dir(void);
uint32_t procfs_ino_cmdline(void);

/** The inode of /proc/net and of /proc/net/dev. */
uint32_t procfs_ino_netdir(void);
uint32_t procfs_ino_netdev(void);
uint32_t procfs_ino_disk(void);
uint32_t procfs_ino_uptime(void);
uint32_t procfs_ino_statsys(void);
uint32_t procfs_ino_meminfo(void);
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
uint32_t procfs_fd_ino(struct fd_entry *e);
uint32_t procfs_ino_pid(int pid);

/** The owner /proc/<pid>/oom_score_adj of process `pid` has, whose effective
 * user ID may write it beside a privileged caller: the process's effective
 * user and group IDs while its address space is dumpable, and 0 otherwise.
 * 0, or -ENOENT when there is no such process. */
int procfs_oom_owner(int pid, uint32_t *uid, uint32_t *gid);

/** The same owner for a descriptor open on an oom_score_adj file: 0 with
 * `*uid` and `*gid` set, or -1 when `e` is no such descriptor or its process
 * is gone. */
int procfs_fd_oom_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid);

#endif /* _PROCFS_H_ */
