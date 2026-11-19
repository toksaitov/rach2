/*
 * inotify.c: the inotify instances, their watches and their queues, as
 * inotify.h describes them.
 */

#include "inotify.h"

#include "anon.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../fs/name.h"
#include "../fs/vfs.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

#define IN_NONBLOCK 04000
#define IN_CLOEXEC  02000000

#define POLLIN   0x001
#define FIONREAD 0x541B

int inotify_max_user_instances = 128;
int inotify_max_user_watches = 8192;
int inotify_max_queued_events = 16384;

/* A record as a read returns it: the watch descriptor, the mask, the cookie
 * and the length of the name, then the name, padded with NULs to a multiple
 * of the record's size. */
#define EVENT_HEAD 16u

struct inotify;

struct watch {
    struct watch *next; /* the list of every watch of the machine */
    struct inotify *owner;
    const struct vfs_mount *mnt; /* the base mount of the file system (vfs.h) */
    uint64_t ino;
    int32_t wd;
    uint32_t mask;
    int isdir;
};

/* A queued event, with `len` bytes of name after it. */
struct event {
    struct event *next;
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
};

struct inotify {
    struct anon_obj anon;
    struct inotify *next; /* the list of every instance, for the limits */
    wait_queue_t wq;
    struct event *head;
    struct event *tail;
    uint32_t queued;
    uint32_t bytes; /* what a read of the whole queue returns */
    /* The IN_Q_OVERFLOW event, queued once at most, which needs no heap. */
    struct event overflow;
    int overflow_queued;
    int32_t next_wd; /* the watch descriptor tried next, from 1 */
    uint32_t uid;    /* the real uid of its maker, whose limits it counts in */
};

int inotify_watches;
static struct watch *watches;
static struct inotify *instances;
static uint32_t cookies;

/*
 * The watches that want each event a descriptor call reports, IN_ACCESS to
 * IN_OPEN by bit number, IN_ATTRIB among them, counted by a hash of their
 * file (`file_wants`) and, for a watch of a directory, of their file system
 * alone (`dir_wants`), so that a call without the big lock can tell that no
 * watch hears its event and take no lock, as Linux tests the inode's and
 * the parent's i_fsnotify_mask before it takes any (`fsnotify_parent`,
 * include/linux/fsnotify.h; `__fsnotify_parent` and `fsnotify`,
 * fs/notify/fsnotify.c). A count covers every watch whose key falls in its
 * slot, so a count above 0 may be another file's, which costs the caller
 * the big lock and nothing else. The counts change under the big lock with
 * the list (`account`) and are read without it.
 */
#define WANT_BITS  6
#define FILE_SLOTS 256
#define DIR_SLOTS  64
#define WANT_DESC  (IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE | IN_OPEN)

static uint32_t file_wants[WANT_BITS][FILE_SLOTS];
static uint32_t dir_wants[WANT_BITS][DIR_SLOTS];

/** The slot of `file_wants` of the file `ino` of the base mount `base`. */
static uint32_t file_slot(const struct vfs_mount *base, uint64_t ino)
{
    uint32_t h = (uint32_t)ino ^ (uint32_t)(ino >> 32) ^ (uint32_t)((uintptr_t)base >> 4);

    return (h * 2654435761u) >> 24;
}

/** The slot of `dir_wants` of the base mount `base`. */
static uint32_t dir_slot(const struct vfs_mount *base)
{
    return ((uint32_t)((uintptr_t)base >> 4) * 2654435761u) >> 26;
}

/** Count the watch `w` with the events of `mask` in or out of the tables
 * by `sign`, 1 or -1, under the big lock: in with its mask when it is made
 * and out when it goes, and, when its mask changes, in with the new before
 * out with the old, so that a reader never finds a wanted event uncounted. */
static void account(const struct watch *w, uint32_t mask, int sign)
{
    uint32_t f = file_slot(w->mnt, w->ino), d = dir_slot(w->mnt);
    int b;

    for (b = 0; b < WANT_BITS; b++) {
        if (mask & WANT_DESC & (1u << b)) {
            file_wants[b][f] += (uint32_t)sign;
            if (w->isdir) {
                dir_wants[b][d] += (uint32_t)sign;
            }
        }
    }
}

/** Whether a watch counted in slot `slot` of `file_wants` may want an
 * event of `mask`. */
static int file_wanted(uint32_t slot, uint32_t mask)
{
    int b;

    for (b = 0; b < WANT_BITS; b++) {
        if ((mask & (1u << b)) && *(volatile const uint32_t *)&file_wants[b][slot] != 0) {
            return 1;
        }
    }
    return 0;
}

/** Whether a watch of a directory counted in slot `slot` of `dir_wants`
 * may want an event of `mask`. */
static int dir_wanted(uint32_t slot, uint32_t mask)
{
    int b;

    for (b = 0; b < WANT_BITS; b++) {
        if ((mask & (1u << b)) && *(volatile const uint32_t *)&dir_wants[b][slot] != 0) {
            return 1;
        }
    }
    return 0;
}

/** Put `ev` at the tail of the queue of `in` and wake its readers. */
static void append(struct inotify *in, struct event *ev)
{
    ev->next = NULL;
    if (in->tail) {
        in->tail->next = ev;
    } else {
        in->head = ev;
    }
    in->tail = ev;
    in->queued++;
    in->bytes += EVENT_HEAD + ev->len;
    wq_wake(&in->wq, WQ_DATA);
}

/** Queue the IN_Q_OVERFLOW event of `in`, unless it is queued already. */
static void queue_overflow(struct inotify *in)
{
    if (in->overflow_queued) {
        return;
    }
    in->overflow.wd = -1;
    in->overflow.mask = IN_Q_OVERFLOW;
    in->overflow.cookie = 0;
    in->overflow.len = 0;
    in->overflow_queued = 1;
    append(in, &in->overflow);
}

/** Whether the strings `a` and `b` are equal. */
static int same_name(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * Queue an event on `in`. One the same as the last one queued and not yet
 * read is dropped, as Linux merges it, so a file written a thousand times
 * between two reads is one IN_MODIFY. A full queue, or a heap with no room,
 * loses the event, and the reader is told by one IN_Q_OVERFLOW.
 */
static void queue_event(struct inotify *in, int32_t wd, uint32_t mask, uint32_t cookie,
                        const char *name)
{
    uint32_t nlen = name ? (uint32_t)k_strlen(name) : 0;
    uint32_t len = name ? (nlen + EVENT_HEAD) & ~(EVENT_HEAD - 1u) : 0;
    struct event *last = in->tail;
    struct event *ev;

    if (last && last->wd == wd && last->mask == mask && last->cookie == cookie &&
        last->len == len && (len == 0 || same_name((const char *)(last + 1), name))) {
        return;
    }
    if ((int32_t)in->queued >= inotify_max_queued_events) {
        queue_overflow(in);
        return;
    }
    ev = kmalloc(sizeof(*ev) + len);
    if (!ev) {
        queue_overflow(in);
        return;
    }
    ev->wd = wd;
    ev->mask = mask;
    ev->cookie = cookie;
    ev->len = len;
    if (len) {
        k_memset(ev + 1, 0, len);
        k_memcpy(ev + 1, name, nlen);
    }
    append(in, ev);
}

/** Remove the watch `w`, telling its instance with IN_IGNORED. */
static void drop(struct watch *w)
{
    struct watch **link = &watches;

    queue_event(w->owner, w->wd, IN_IGNORED, 0, NULL);
    while (*link && *link != w) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = w->next;
    }
    account(w, w->mask, -1);
    inotify_watches--;
    vfs_mount_watches(w->mnt, -1);
    kfree(w);
}

/**
 * Queue the event `mask` for `w` when it asks for any of it, reporting the
 * events it asked for with IN_ISDIR as given, and end a one-shot watch
 * after its first. Returns 1 when the watch went.
 */
static int deliver(struct watch *w, uint32_t mask, uint32_t cookie, const char *name)
{
    uint32_t want = mask & w->mask & IN_ALL_EVENTS;

    if (!want) {
        return 0;
    }
    queue_event(w->owner, w->wd, want | (mask & IN_ISDIR), cookie, name);
    if (w->mask & IN_ONESHOT) {
        drop(w);
        return 1;
    }
    return 0;
}

/**
 * `inotify_file` for an event of a file whose name its description had,
 * `unlinked` nonzero once that name is gone: a watch set with
 * IN_EXCL_UNLINK hears nothing of it then, as Linux's
 * `fsnotify_handle_inode_event` drops a path event of an unlinked dentry
 * for such a mark (fs/notify/fsnotify.c).
 */
static void file_event(struct vfs_mount *mnt, uint64_t ino, uint32_t mask, int unlinked)
{
    struct watch *w, *next;

    for (w = watches; w; w = next) {
        next = w->next;
        if (w->mnt == vfs_mount_base(mnt) && w->ino == ino &&
            !(unlinked && (w->mask & IN_EXCL_UNLINK))) {
            (void)deliver(w, mask | (w->isdir ? IN_ISDIR : 0u), 0, NULL);
        }
    }
}

/** `inotify_child` with `unlinked` as `file_event` takes it. */
static void child_event(struct vfs_mount *mnt, uint64_t dir_ino, const char *name, uint32_t mask,
                        uint32_t cookie, int unlinked)
{
    struct watch *w, *next;

    for (w = watches; w; w = next) {
        next = w->next;
        if (w->mnt == vfs_mount_base(mnt) && w->ino == dir_ino && w->isdir &&
            !(unlinked && (w->mask & IN_EXCL_UNLINK))) {
            (void)deliver(w, mask, cookie, name);
        }
    }
}

void inotify_file(struct vfs_mount *mnt, uint64_t ino, uint32_t mask)
{
    file_event(mnt, ino, mask, 0);
}

void inotify_child(struct vfs_mount *mnt, uint64_t dir_ino, const char *name, uint32_t mask,
                   uint32_t cookie)
{
    child_event(mnt, dir_ino, name, mask, cookie, 0);
}

int inotify_mount_watched(const struct vfs_mount *mnt)
{
    return mnt && *(volatile const int *)&vfs_mount_base(mnt)->inotify_watched != 0;
}

int inotify_desc_wants(const struct open_file *ofd, uint32_t mask)
{
    const struct vfs_mount *base;

    if (!ofd || !inotify_mount_watched(ofd->vmnt)) {
        return 0;
    }
    base = vfs_mount_base(ofd->vmnt);
    if (file_wanted(file_slot(base, ofd->ino), mask)) {
        return 1;
    }
    if (ofd->leaf) {
        return file_wanted(file_slot(base, ofd->parent_ino), mask);
    }
    return ofd->name && dir_wanted(dir_slot(base), mask);
}

int inotify_dir_wants(const struct vfs_mount *mnt, uint32_t mask)
{
    return inotify_mount_watched(mnt) && dir_wanted(dir_slot(vfs_mount_base(mnt)), mask);
}

int inotify_desc_dir(const struct open_file *ofd, uint64_t *dir_ino, const char **leaf)
{
    if (ofd->leaf) {
        *dir_ino = ofd->parent_ino;
        *leaf = ofd->leaf;
        return 0;
    }
    if (ofd->name && ofd->vmnt) {
        return vfs_name_dir(ofd->name, vfs_mount_base(ofd->vmnt), dir_ino, leaf);
    }
    return -1;
}

/**
 * Whether the name the open file of `ofd` was opened by is gone, Linux's
 * `d_unlinked`: what its name record says, or, without one, whether the
 * file has no link left.
 */
static int desc_unlinked(const struct open_file *ofd)
{
    struct vfs_stat st;

    if (ofd->name) {
        return vfs_name_deleted(ofd->name);
    }
    return ofd->vnode && vfs_getattr(ofd->vmnt, ofd->vnode, &st) == 0 && st.nlink == 0;
}

void inotify_desc(struct open_file *ofd, uint32_t mask)
{
    const char *leaf;
    uint64_t dir_ino;
    int unlinked;

    if (!ofd || !inotify_mount_watched(ofd->vmnt)) {
        return;
    }
    unlinked = desc_unlinked(ofd);
    file_event(ofd->vmnt, ofd->ino, mask, unlinked);
    if (inotify_desc_dir(ofd, &dir_ino, &leaf) == 0) {
        child_event(ofd->vmnt, dir_ino, leaf,
                    mask | ((ofd->watch_flags & OFD_WATCH_DIR) ? IN_ISDIR : 0u), 0, unlinked);
    }
}

void inotify_gone(struct vfs_mount *mnt, uint64_t ino)
{
    struct watch *w, *next;

    for (w = watches; w; w = next) {
        next = w->next;
        if (w->mnt == vfs_mount_base(mnt) && w->ino == ino &&
            !deliver(w, IN_DELETE_SELF | (w->isdir ? IN_ISDIR : 0u), 0, NULL)) {
            drop(w);
        }
    }
}

void inotify_unmount(struct vfs_mount *mnt)
{
    struct watch *w, *next;

    for (w = watches; w; w = next) {
        next = w->next;
        if (w->mnt == mnt) {
            queue_event(w->owner, w->wd, IN_UNMOUNT, 0, NULL);
            drop(w);
        }
    }
}

uint32_t inotify_cookie(void)
{
    cookies = cookies == 0xFFFFFFFFu ? 1u : cookies + 1u;
    return cookies;
}

/** Whether the description of `e` is non-blocking. */
static int nonblocking(const fd_entry_t *e)
{
    return e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
}

/** The wait condition of a read: the instance `arg` has an event queued. */
static int readable(void *arg)
{
    return ((struct inotify *)arg)->head != NULL;
}

/**
 * read(2) of an instance: as many whole records as fit in `count` bytes,
 * waiting for the first. A buffer too small for the first is EINVAL, as on
 * Linux. Each record is taken off the queue before its copy, which can sleep
 * in a page fault, so a second reader meanwhile reads the next; a record
 * whose copy fails is lost, and the read returns what was copied before it,
 * or -EFAULT. A read of an empty queue is -EAGAIN when the description is
 * non-blocking, and -EINTR when a signal ends its wait.
 */
static int32_t inotify_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct inotify *in = (struct inotify *)e->priv;
    uint32_t done = 0;

    while (!in->head) {
        if (nonblocking(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&in->wq, readable, in) < 0) {
            return -EINTR;
        }
    }
    while (in->head && EVENT_HEAD + in->head->len <= count - done) {
        struct event *ev = in->head;
        uint32_t head[4];
        int failed;

        in->head = ev->next;
        if (!in->head) {
            in->tail = NULL;
        }
        in->queued--;
        in->bytes -= EVENT_HEAD + ev->len;
        head[0] = (uint32_t)ev->wd;
        head[1] = ev->mask;
        head[2] = ev->cookie;
        head[3] = ev->len;
        failed = copy_to_user((void __user *)(buf + done), head, EVENT_HEAD) != 0 ||
                 (ev->len &&
                  copy_to_user((void __user *)(buf + done + EVENT_HEAD), ev + 1, ev->len) != 0);
        if (ev == &in->overflow) {
            in->overflow_queued = 0;
        } else {
            kfree(ev);
        }
        if (failed) {
            return done ? (int32_t)done : -EFAULT;
        }
        done += EVENT_HEAD + head[3];
    }
    return done ? (int32_t)done : -EINVAL;
}

/** poll of an instance: POLLIN among `events` while an event is queued,
 * and its wait queue. */
static short inotify_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct inotify *in = (struct inotify *)e->priv;

    if (wq) {
        *wq = &in->wq;
    }
    return (short)((in->head ? POLLIN : 0) & events);
}

/** FIONREAD writes the bytes a read of the whole queue returns at `arg`,
 * and is 0 or -EFAULT; another request is -ENOTTY. */
static int32_t inotify_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct inotify *in = (struct inotify *)e->priv;

    if (request != FIONREAD) {
        return -ENOTTY;
    }
    return copy_to_user((void __user *)arg, &in->bytes, sizeof(in->bytes)) == 0 ? 0 : -EFAULT;
}

/** The last close: the watches go without events, then the queue. */
static void inotify_close(fd_entry_t *e)
{
    struct inotify *in = (struct inotify *)e->priv;
    struct watch **wlink = &watches;
    struct inotify **ilink = &instances;

    while (*wlink) {
        struct watch *w = *wlink;

        if (w->owner == in) {
            *wlink = w->next;
            account(w, w->mask, -1);
            inotify_watches--;
            vfs_mount_watches(w->mnt, -1);
            kfree(w);
        } else {
            wlink = &w->next;
        }
    }
    while (in->head) {
        struct event *ev = in->head;

        in->head = ev->next;
        if (ev != &in->overflow) {
            kfree(ev);
        }
    }
    while (*ilink && *ilink != in) {
        ilink = &(*ilink)->next;
    }
    if (*ilink) {
        *ilink = in->next;
    }
    kfree(in);
    e->priv = NULL;
}

static const struct file_ops inotify_ops = {inotify_read,  NULL, inotify_poll, inotify_ioctl,
                                            inotify_close, NULL, NULL,         0};

int inotify_is(int fd)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;

    return e && e->ops == &inotify_ops;
}

/** The watches of the instances of `uid`. */
static uint32_t user_watches(uint32_t uid)
{
    struct watch *w;
    uint32_t n = 0;

    for (w = watches; w; w = w->next) {
        n += w->owner->uid == uid;
    }
    return n;
}

/** The next watch descriptor of `in`: counting up from 1, as Linux does,
 * past any still in use when the count comes round. */
static int32_t next_wd(struct inotify *in)
{
    struct watch *w;
    int32_t wd;

    for (;;) {
        wd = in->next_wd;
        in->next_wd = wd == 0x7FFFFFFF ? 1 : wd + 1;
        for (w = watches; w; w = w->next) {
            if (w->owner == in && w->wd == wd) {
                break;
            }
        }
        if (!w) {
            return wd;
        }
    }
}

int32_t inotify_watch(int fd, struct vfs_mount *mnt, uint64_t ino, int isdir, uint32_t mask)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);
    uint32_t keep = mask & (IN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK);
    struct inotify *in;
    struct watch *w;
    uint32_t old;

    if (!e || e->ops != &inotify_ops) {
        return -EINVAL;
    }
    in = (struct inotify *)e->priv;
    for (w = watches; w; w = w->next) {
        if (w->owner == in && w->mnt == vfs_mount_base(mnt) && w->ino == ino) {
            if (mask & IN_MASK_CREATE) {
                return -EEXIST;
            }
            old = w->mask;
            w->mask = (mask & IN_MASK_ADD) ? w->mask | keep : keep;
            account(w, w->mask, 1);
            account(w, old, -1);
            return w->wd;
        }
    }
    if ((int32_t)user_watches(in->uid) >= inotify_max_user_watches) {
        return -ENOSPC;
    }
    w = kzalloc(sizeof(*w));
    if (!w) {
        return -ENOMEM;
    }
    w->owner = in;
    w->mnt = vfs_mount_base(mnt);
    w->ino = ino;
    w->isdir = isdir;
    w->mask = keep;
    w->wd = next_wd(in);
    w->next = watches;
    watches = w;
    account(w, w->mask, 1);
    inotify_watches++;
    vfs_mount_watches(w->mnt, 1);
    return w->wd;
}

long sys_inotify_rm_watch(struct isr_params *params)
{
    fd_entry_t *e = fd_get(current_proc->fds, (int)SC_ARG(params, 0));
    int32_t wd = (int32_t)SC_ARG(params, 1);
    struct watch *w;

    if (!e) {
        return -EBADF;
    }
    if (e->ops != &inotify_ops) {
        return -EINVAL;
    }
    for (w = watches; w; w = w->next) {
        if (w->owner == (struct inotify *)e->priv && w->wd == wd) {
            drop(w);
            return 0;
        }
    }
    return -EINVAL;
}

/** A new instance with the flags `flags`, and a descriptor of the caller's
 * on it: the descriptor, -EINVAL for another flag, -EMFILE past the user's
 * instances, -ENOMEM, or `fd_alloc`'s error. */
static int32_t inotify_open(uint32_t flags)
{
    struct inotify *in;
    uint32_t mine = 0;
    fd_entry_t *e;
    int fd;

    if (flags & ~(uint32_t)(IN_NONBLOCK | IN_CLOEXEC)) {
        return -EINVAL;
    }
    for (in = instances; in; in = in->next) {
        mine += in->uid == current_proc->ruid;
    }
    if ((int32_t)mine >= inotify_max_user_instances) {
        return -EMFILE;
    }
    in = kzalloc(sizeof(*in));
    if (!in) {
        return -ENOMEM;
    }
    in->anon.name = "inotify";
    wait_queue_init(&in->wq);
    in->next_wd = 1;
    in->uid = current_proc->ruid;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(in);
        return fd;
    }
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &inotify_ops;
    e->priv = in;
    e->flags = FD_FLAG_READ | ((flags & IN_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    if (flags & IN_NONBLOCK) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    in->next = instances;
    instances = in;
    fd_install(current_proc->fds, fd);
    return fd;
}

long sys_inotify_init1(struct isr_params *params)
{
    return inotify_open((uint32_t)SC_ARG(params, 0));
}

/** inotify_init has no arguments, and the register of a first argument,
 * which i386 code keeps its global offset table in, comes back as it
 * went. */
long sys_inotify_init(struct isr_params *params)
{
    (void)params;
    return inotify_open(0);
}
