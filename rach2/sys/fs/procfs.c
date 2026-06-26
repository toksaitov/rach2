/*
 * procfs.c: the synthetic /proc, generated from the process table.
 *
 * A /proc text file is snapshotted into a small pooled buffer when opened and
 * served from there (so a read sees a consistent moment, and dup/fork share the
 * snapshot until the last descriptor closes; the per-open-file-description
 * lifetime). The text is built with bounded append helpers and hand-rolled
 * integer formatting (the kernel has no snprintf and avoids 64-bit division).
 */

#include "procfs.h"
#include "synthfs.h"
#include "fd.h"
#include "file.h"
#include "devfs.h"
#include "krffs.h"
#include "../tty/pty.h"
#include "../io/poll.h"
#include "../proc/process.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"

#define PROCFS_O_CLOEXEC 02000000

/* Inode namespace for /proc: the directory, then a per-pid base. */
#define PROCFS_INO_DIR 0x100u
#define PROCFS_INO_PID_BASE 0x200u

/* A /proc text file, snapshotted at open and served through file_ops. */
struct procfs_file {
    char data[512];
    int len;
    int used;
    int origin_pid;  /* the /proc/<pid> this snapshot came from (for fd readlink) */
    int origin_file; /* SYNTH_PROC_STAT or SYNTH_PROC_CMDLINE */
};

/* bounded text builders (no snprintf; cap leaves room for a NUL) */

static void ap_ch(char *buf, int cap, int *len, char c)
{
    if (*len < cap - 1) {
        buf[*len] = c;
        (*len)++;
    }
}

static void ap_str(char *buf, int cap, int *len, const char *s)
{
    while (*s) {
        ap_ch(buf, cap, len, *s++);
    }
}

/* Append a signed decimal. INT_MIN is handled by negating in unsigned space. */
static void ap_int(char *buf, int cap, int *len, int v)
{
    char tmp[12];
    int n = 0;
    unsigned int uv;

    if (v < 0) {
        ap_ch(buf, cap, len, '-');
        uv = (unsigned int)(-(v + 1)) + 1u;
    } else {
        uv = (unsigned int)v;
    }
    if (uv == 0) {
        ap_ch(buf, cap, len, '0');
        return;
    }
    while (uv) {
        tmp[n++] = (char)('0' + (uv % 10u));
        uv /= 10u;
    }
    while (n) {
        ap_ch(buf, cap, len, tmp[--n]);
    }
}

/* queries over the process table */

/**
 * Resolve a /proc pid token to a live pid, or -1.
 *
 * SYNTH_PID_SELF maps to the caller's pid. A numeric pid must name a process
 * that exists and is not PROC_UNUSED.
 */
int procfs_resolve_pid(int pid)
{
    process_t *p;

    if (pid == SYNTH_PID_SELF) {
        return current_proc ? (int)current_proc->pid : -1;
    }
    if (pid < 0) {
        return -1;
    }
    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -1;
    }
    return pid;
}

static char proc_state_char(proc_state_t s)
{
    switch (s) {
        case PROC_RUNNING:
        case PROC_READY:
            return 'R';
        case PROC_BLOCKED:
            return 'S';
        case PROC_ZOMBIE:
            return 'Z';
        default:
            return 'R';
    }
}

/**
 * Format /proc/<pid>/stat into `buf`, returning its length or -ENOENT.
 *
 * Emits the space-separated fields ps and friends parse. comm (field 2) is
 * wrapped in parentheses, with any stray parens scrubbed so a naive parser that
 * scans for the closing ')' is not fooled. Accounting fields we do not track
 * (faults, cpu time, sizes) are zero, but every field is present and in order.
 */
static int procfs_gen_stat(int pid, char *buf, int cap)
{
    process_t *p;
    int len = 0;
    int i;

    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -ENOENT;
    }

    ap_int(buf, cap, &len, pid);             /* 1 pid */
    ap_str(buf, cap, &len, " (");            /* 2 comm */
    if (p->comm[0]) {
        for (i = 0; i < (int)sizeof(p->comm) && p->comm[i]; i++) {
            char c = p->comm[i];
            ap_ch(buf, cap, &len, (c == '(' || c == ')') ? '_' : c);
        }
    } else {
        ap_ch(buf, cap, &len, '?');
    }
    ap_str(buf, cap, &len, ") ");
    ap_ch(buf, cap, &len, proc_state_char(p->state)); /* 3 state */
    ap_ch(buf, cap, &len, ' ');
    ap_int(buf, cap, &len, (int)p->parent_pid); /* 4 ppid */
    ap_ch(buf, cap, &len, ' ');
    ap_int(buf, cap, &len, pid); /* 5 pgrp (no job control yet: self) */
    ap_ch(buf, cap, &len, ' ');
    ap_int(buf, cap, &len, pid); /* 6 session */
    ap_str(buf, cap, &len, " 0 -1"); /* 7 tty_nr, 8 tpgid */
    /* 9..13 flags,minflt,cminflt,majflt,cmajflt; 14..17 utime,stime,cutime,cstime */
    ap_str(buf, cap, &len, " 0 0 0 0 0 0 0 0 0");
    ap_str(buf, cap, &len, " 20 0"); /* 18 priority, 19 nice */
    ap_str(buf, cap, &len, " 1 0 0"); /* 20 num_threads, 21 itrealvalue, 22 starttime */
    ap_str(buf, cap, &len, " 0 0"); /* 23 vsize, 24 rss */
    ap_ch(buf, cap, &len, '\n');
    buf[len] = '\0';
    return len;
}

/**
 * Format /proc/<pid>/cmdline into `buf`, returning its length or -ENOENT.
 *
 * argv is NUL-separated. We do not retain the full argv, so the program name is
 * emitted as the single argument (busybox falls back to comm).
 */
static int procfs_gen_cmdline(int pid, char *buf, int cap)
{
    process_t *p;
    int len = 0;
    int i;

    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -ENOENT;
    }
    for (i = 0; i < (int)sizeof(p->comm) && p->comm[i]; i++) {
        ap_ch(buf, cap, &len, p->comm[i]);
    }
    if (len > 0) {
        ap_ch(buf, cap, &len, '\0'); /* argv[0] terminator */
    }
    return len;
}

/**
 * Render the /proc/<pid>/fd/<fd> symlink target into `buf`, or -ENOENT.
 *
 * The target string matches what each descriptor type reports through the real
 * Linux /proc: a path for files and dirs, /dev/console, /dev/pts/N, /dev/ptmx,
 * and the pipe:[1]/socket:[1]/anon_inode forms. musl ttyname_r relies on the
 * /dev/pts/N spelling.
 */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap)
{
    process_t *p;
    fd_entry_t *e;
    int len = 0;

    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -ENOENT;
    }
    if (fd < 0 || fd >= MAX_FDS) {
        return -ENOENT;
    }
    e = &p->fds.entries[fd];
    if (e->type == FD_TYPE_NONE) {
        return -ENOENT;
    }

    switch (e->type) {
        case FD_TYPE_CONSOLE:
            ap_str(buf, cap, &len, "/dev/console");
            break;
        case FD_TYPE_DEVICE:
            ap_str(buf, cap, &len, "/dev/");
            ap_str(buf, cap, &len, devfs_fd_name(e));
            break;
        case FD_TYPE_PIPE:
            ap_str(buf, cap, &len, "pipe:[1]");
            break;
        case FD_TYPE_PTS:
            /* a pty slave: /dev/pts/N (so musl ttyname_r can match it). */
            ap_str(buf, cap, &len, "/dev/pts/");
            ap_int(buf, cap, &len, pty_slave_index(e));
            break;
        case FD_TYPE_PTM:
            ap_str(buf, cap, &len, "/dev/ptmx");
            break;
        case FD_TYPE_SOCKET:
            ap_str(buf, cap, &len, "socket:[1]");
            break;
        case FD_TYPE_FILE:
        case FD_TYPE_DIR:
            if (e->node) {
                char path[256]; /* the same path capacity the syscall layer uses */
                if (krffs_node_path(e->node, path, sizeof(path)) == 0) {
                    ap_str(buf, cap, &len, path); /* the full absolute path */
                } else {
                    ap_ch(buf, cap, &len, '/');
                }
            } else {
                ap_ch(buf, cap, &len, '/'); /* the root directory */
            }
            break;
        case FD_TYPE_SYNTH_DIR: {
            /* a /dev or /proc directory fd: its reconstructed path */
            struct synth_dir *sd = (struct synth_dir *)e->priv;
            char path[64];
            if (sd) {
                synth_dir_path(sd->kind, sd->pid, path, (int)sizeof(path));
                ap_str(buf, cap, &len, path);
            }
            break;
        }
        case FD_TYPE_PROC: {
            /* a /proc/<pid>/{stat,cmdline} snapshot fd: its origin path */
            struct procfs_file *pf = (struct procfs_file *)e->priv;
            if (pf) {
                ap_str(buf, cap, &len, "/proc/");
                ap_int(buf, cap, &len, pf->origin_pid);
                ap_ch(buf, cap, &len, '/');
                ap_str(buf, cap, &len,
                       pf->origin_file == SYNTH_PROC_CMDLINE ? "cmdline" : "stat");
            }
            break;
        }
        default:
            ap_str(buf, cap, &len, "anon_inode:[device]"); /* fb/mouse: best effort */
            break;
    }
    return len;
}

/**
 * Return the pid of the n-th live process in process-table order, or -1.
 *
 * Lets a /proc directory read walk the table by index without exposing slots.
 */
int procfs_nth_pid(int n)
{
    int i;
    int seen = 0;

    if (n < 0) {
        return -1;
    }
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED) {
            if (seen == n) {
                return (int)proc_table[i].pid;
            }
            seen++;
        }
    }
    return -1;
}

uint32_t procfs_ino_dir(void)
{
    return PROCFS_INO_DIR;
}

uint32_t procfs_ino_pid(int pid)
{
    return PROCFS_INO_PID_BASE + (uint32_t)(pid & 0xffff);
}

/* the /proc file backing: a snapshot served through file_ops */

static struct procfs_file proc_files[8];

static int32_t procfs_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off, avail, n;

    if (!f) {
        return 0;
    }
    off = e->ofd->offset;
    if (off >= (uint32_t)f->len) {
        return 0; /* end of the snapshot */
    }
    avail = (uint32_t)f->len - off;
    n = count < avail ? count : avail;
    if (copy_to_user(buf, f->data + off, n) != 0) {
        return -EFAULT;
    }
    e->ofd->offset = off + n;
    return (int32_t)n;
}

static int32_t procfs_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EBADF; /* these /proc files are read-only */
}

static short procfs_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    (void)e;
    if (wq) {
        *wq = NULL;
    }
    return (short)(events & POLLIN); /* always has data (or EOF) ready */
}

static int32_t procfs_ioctl(fd_entry_t *e, uint32_t request, uint32_t arg)
{
    (void)e;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

static void procfs_close(fd_entry_t *e)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    if (f) {
        f->used = 0; /* return the snapshot slot to the pool */
    }
}

static const struct file_ops procfs_file_ops = {procfs_read, procfs_write, procfs_poll,
                                                procfs_ioctl, procfs_close};

/**
 * Open a /proc/<pid>/{stat,cmdline} file, returning a descriptor or -errno.
 *
 * The text is generated and snapshotted into a pooled slot at open, then served
 * read-only through procfs_file_ops; the slot returns to the pool on the last
 * close. These files are read-only regular files, so the open-flag checks are
 * ordered to report the conventional errno: O_CREAT|O_EXCL on an existing file
 * is -EEXIST first, then O_DIRECTORY is -ENOTDIR, then any write or create
 * intent is -EACCES.
 */
int procfs_open(const struct vpath *vp, int flags)
{
    int pid;
    struct procfs_file *f = NULL;
    fd_entry_t *e;
    int slot, fd, len;

    pid = procfs_resolve_pid(vp->pid);
    if (pid < 0) {
        return -ENOENT;
    }
    if ((flags & 0100 /* O_CREAT */) && (flags & 0200 /* O_EXCL */)) {
        return -EEXIST;
    }
    if (flags & 0200000 /* O_DIRECTORY */) {
        return -ENOTDIR;
    }
    if ((flags & 3) != 0 || (flags & 0100 /* O_CREAT */)) {
        return -EACCES;
    }

    for (slot = 0; slot < 8; slot++) {
        if (!proc_files[slot].used) {
            f = &proc_files[slot];
            break;
        }
    }
    if (!f) {
        return -ENFILE;
    }

    if (vp->procfile == SYNTH_PROC_STAT) {
        len = procfs_gen_stat(pid, f->data, (int)sizeof(f->data));
    } else {
        len = procfs_gen_cmdline(pid, f->data, (int)sizeof(f->data));
    }
    if (len < 0) {
        return len;
    }
    f->len = len;
    f->used = 1;
    f->origin_pid = pid;
    f->origin_file = vp->procfile;

    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        f->used = 0;
        return -EMFILE;
    }
    e = &current_proc->fds.entries[fd];
    e->type = FD_TYPE_PROC;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &procfs_file_ops;
    e->priv = f;
    e->ofd->offset = 0;
    e->flags = FD_FLAG_READ;
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}
